# `<pci>starfive`: StarFive JH7110 PCIe host driver

Root-port driver for the two PLDA XpressRICH PCIe 2.0 x1 controllers in the
StarFive JH7110 (VisionFive 2, Orange Pi RV, Milk-V Mars, Star64...). It
exposes each port as a Haiku PCI domain. The stock PCI bus manager then
enumerates the domain, assigns BARs and loads `xhci` (VIA VL805) and `nvme`.

License: GPL-2.0-only (Linux-derived). See `THIRD_PARTY.md`. This is a
separate add-on and is not part of the kernel.

## Design

* `StarFivePCIe.{h,cpp}` contains `StarFivePCIeController`, a
  `pci_controller_module_info` implementation shaped like Haiku's
  `DWPCIController`. One instance runs per enabled `starfive,jh7110-pcie`
  FDT node.
* `PldaMsiController` is one MSI vector pool shared by both ports. Haiku has
  only one global `MSIInterface`. Both ports decode MSI the same way
  (address `IMSI_ADDR` = PCI 0x190 via the root-port BAR0 at 0, data =
  vector 0..31), so a single pool of 32 Haiku vectors works. Each port's
  interrupt handler dispatches its own `ISTATUS_MSI` bits into that pool.
  Multi-message MSI and MSI-X (NVMe) are supported, because MSI-X uses the
  same address/data pairs.
* INTx: each port gets 4 Haiku vectors. In `finalize()`, the driver walks the
  bus below the root port and swizzles pins through bridges. It writes the
  vector number into each function's `PCI_interrupt_line`. The bus manager
  reads that register back (`_RefreshDeviceInfo`), and `xhci`/`nvme` use it
  as their IRQ. The DT `interrupt-map` is not used. Upstream points it at the
  PCIe-local intc node. The vendor DT wrongly points it at PLIC 1..4.
* Config space is the PLDA "cfg" window. ATR entry 0 is set to config
  translation, ECAM layout (`bus<<20 | dev<<15 | fn<<12 | reg`) with native
  1/2/4-byte accesses, as in Linux `pci_generic_config_*`. The root port
  BAR0/1 read as 0 and ignore writes (Linux `starfive_pcie_hide_rc_bar`).
  Only device 0 is valid on bus 0 and on the root port's secondary bus
  (U-Boot `starfive_pcie_addr_valid`). Without that rule, Unsupported
  Request completions show up.
* Both device-tree bindings are accepted:

  | | upstream Linux (v6.12 checked) | vendor (U-Boot 2021.10 / SDK kernels) |
  |---|---|---|
  | reg-names | `"cfg"`, `"apb"` (16 MB, 1 MB) | `"reg"`, `"config"` (16 MB, 256 MB) |
  | syscon | `starfive,stg-syscon = <&stg_syscon>` | `<&stg_syscon AR AW RP_NEP>` (checked against the built-in values) |
  | PERST# | `perst-gpios = <&sysgpio 26/28 ACTIVE_LOW>` | pinctrl states only. The driver falls back to GPIO 26/28 |
  | INTx map | `&pcie_intcN 1..4` | `&plic 1..4` (wrong, ignored) |

  The port number comes from the apb base address (0x2b000000 is port 0,
  0x2c000000 is port 1), else from `linux,pci-domain`. Haiku's FDT bus
  registers disabled nodes too, so `status` is checked in `SupportsDevice`.

## Register map (with sources)

All addresses are from Linux `arch/riscv/boot/dts/starfive/jh7110.dtsi`
(v6.12). They match the vendor U-Boot dtsi.

| Block | Base | Source |
|---|---|---|
| PCIe0 apb (PLDA bridge) / cfg | 0x2b000000 / 0x9_4000_0000 | jh7110.dtsi `pcie@940000000` |
| PCIe1 apb / cfg | 0x2c000000 / 0x9_c000_0000 | jh7110.dtsi `pcie@9c0000000` |
| PCIe0/1 MMIO32 window | 0x30000000 / 0x38000000, 128 MB each | `ranges` |
| PCIe0/1 MMIO64 pref window | 0x9_0000_0000 / 0x9_8000_0000, 1 GB each | `ranges` |
| PCIe0/1 PLIC interrupt | 56 / 57 | `interrupts` |
| PCIe PHY0 / PHY1 | 0x10210000 / 0x10220000 | `pciephy0/1` |
| STGCRG (clocks + resets) | 0x10230000 | `stgcrg` |
| STG syscon | 0x10240000 | `stg_syscon` |
| SYSCRG | 0x13020000 | `syscrg` |
| sys GPIO/pinctrl | 0x13040000 | `sysgpio` |

PLDA bridge registers (offsets in apb). Source: Linux `pcie-plda.h`.

| Offset | Name | Use |
|---|---|---|
| 0x080 | GEN_SETTINGS | bit0 RP_ENABLE |
| 0x09c | PCIE_PCI_IDS_DW1 | class code (0x0604) << 16, keep revision |
| 0x0b4 | PCI_MISC | bit15 PHY_FUNCTION_DIS |
| 0x0fc | PCIE_WINROM | bit3 PREF_MEM_WIN_64_SUPPORT |
| 0x180/0x184 | IMASK_LOCAL / ISTATUS_LOCAL | 16-22 AXI/PCIe errors, 24-27 INTA-D, 28 MSI, 29 AER, 30 misc, 31 SYS_ERR. Mask 1 = enabled, status write-1-to-clear |
| 0x188/0x18c | IMASK_HOST / ISTATUS_HOST | cleared, unused |
| 0x190 | IMSI_ADDR | MSI doorbell (PCI address 0x190 via BAR0=0) |
| 0x194 | ISTATUS_MSI | 32 MSI vectors, write-1-to-clear |
| 0x3f0 | PMSG_SUPPORT_RX | bit2 LTR forwarding (must be cleared or the system hangs, per Linux and U-Boot) |
| 0x600/0x604 | ATR0_PCIE_WIN0 SRCADDR_PARAM / SRC_ADDR | inbound: size field 0x25 means 2^38 identity window for DMA |
| 0x800 + 0x20*n | ATR0_AXI4_SLV0 entry n: +0 SRCADDR_PARAM (addr lo, size<<1, enable), +4 SRC_ADDR hi, +8 TRSL lo, +0xc TRSL hi, +0x10 TRSL_PARAM (0 mem, 1 config) | outbound windows |
| 0x1000 | root port type-1 config header | BAR0/1 written to 0 |

STG syscon. Source: Linux `pcie-starfive.c`; U-Boot vendor DT cells give the
same absolute offsets.

| Port base | +0x78 AR | +0x7c AW | +0xe8 RP_NEP | +0x170 LNKSTA |
|---|---|---|---|---|
| PCIe0 0x048 | 0x0c0: [22:8] AXI4_SLVL_AR, func in [20:17] | 0x0c4: [14:0] AXI4_SLVL_AW, func in [12:9]; [19:18] CKREF_SRC=2; bit22 CLKREQ | 0x130 bit8 K_RP_NEP | 0x1b8 bit5 DATA_LINK_ACTIVE |
| PCIe1 0x1f8 | 0x270 | 0x274 | 0x2e0 | 0x368 |

PHY0 USB3/PCIe mode (Linux `phy-jh7110-pcie.c`, U-Boot `STG_SYSCON_328/500`):
syscon 0x148 [21:20] PHY mode (0 = PCIe), 0x1f4 [3:2] bus width, bit4 USB3
enable. PHY regs: +0x28 KVCO level = 0x91, +0x80 KVCO tune = 0xc, +0x7c bit4
USB3 PLL SSC.

## Clocks, resets and syscon bits this driver needs

Haiku has no JH7110 clock or reset driver, so the driver sets these bits
directly. It only sets bits that are off. Sources: Linux
`clk-starfive-jh71x0.{c,h}` (clock reg = base + 4*id, bit 31 = enable),
`reset-starfive-jh7110.c` (assert reg; status bit 1 = released),
dt-bindings `starfive,jh7110-crg.h`.

| What | Register | Bit |
|---|---|---|
| SYSCLK_NOC_BUS_STG_AXI (id 96, critical) | SYSCRG + 0x180 | 31 |
| SYSRST_NOC_BUS_STG_AXI (id 30) | SYSCRG + 0x2f8 (status 0x308) | 30 |
| STGCLK_PCIE_SLV_MAIN (id 14, critical) | STGCRG + 0x38 | 31 |
| STGCLK_PCIE0_AXI_MST0 / APB / TL (8/9/10) | STGCRG + 0x20/0x24/0x28 | 31 |
| STGCLK_PCIE1_AXI_MST0 / APB / TL (11/12/13) | STGCRG + 0x2c/0x30/0x34 | 31 |
| STGRST_PCIE0 AXI_MST0, AXI_SLV0, AXI_SLV, BRG, CORE, APB (11-16) | STGCRG + 0x74 (status 0x78) | 11-16 cleared |
| STGRST_PCIE1 same set (17-22) | STGCRG + 0x74 (status 0x78) | 17-22 cleared |
| STG syscon RP_NEP, CKREF_SRC=2, CLKREQ | see table above | |
| PERST# | sys GPIO 26 (port 0) / 28 (port 1): DOUT reg 0x040 + 4*(pin/4), DOEN reg 0x000 + 4*(pin/4), field at bit 8*(pin%4) (DOUT 7 bits, DOEN 6 bits; DOEN 0 = output) | active low |

The parent clocks `stg_axiahb`, `apb_bus` and `osc` are on from reset and the
SPL, and the driver assumes they are.

### Does U-Boot already do this?

**Only partly, and not reliably.** Checked against the Orange Pi vendor
U-Boot (`v2021.10-jh7110`, d7274df):

* The SPL (`board/starfive/visionfive2/spl.c`) turns on
  `CLK_NOC_BUS_STG_AXI`.
* The PCIe clocks, resets, syscon, ATR and PERST# are only set up by the
  U-Boot PCIe driver at probe time, and that only happens on `pci enum`. The
  default `bootcmd` runs `pci enum` only inside `bootenv_nvme`, which comes
  after `mmc` in `boot_devs=mmc nvme`, and only when `bootmode=flash`. A
  normal SD-card boot never touches PCIe.
* `board_init()` calls `jh7110_usb_init(true)`, the USB 2.0 path. It leaves
  PHY0 in PCIe mode.

So the driver does the full bring-up itself, the same as Linux. If the link
is already up from firmware it says so and re-initialises anyway, which
includes a PERST# cycle.

## Bring-up sequence (as implemented; mirrors Linux `starfive_pcie_host_init`)

1. Parse the FDT and map apb, cfg, PHY, STG syscon, STGCRG, SYSCRG and GPIO.
2. Assert PERST# (drive the GPIO low, output enabled).
3. PHY. Port 0 only: if syscon 0x148 shows USB3 mode, switch it to PCIe.
   Then KVCO tuning.
4. Syscon: K_RP_NEP, CKREF_SRC = 2, CLKREQ.
5. Clocks on, resets deasserted, status polled for up to 1 ms each.
6. Disable PLDA functions 1-3 (AR/AW function select + PCI_MISC), then
   select function 0.
7. RP_ENABLE, RC BAR0/1 = 0, class 0x0604, LTR forwarding off, 64-bit
   prefetchable window on.
8. Wait 100 ms, release PERST#, wait 100 ms, then poll DATA_LINK_ACTIVE 10
   times at 100 ms intervals.
9. ATR: entry 0 = cfg window (config), entries 1..n = `ranges` memory
   windows (identity), inbound 2^38 identity.
10. Interrupts: clear status, allocate INTx vectors and the shared MSI pool,
    install the handler on PLIC 56/57, unmask INTx | MSI | error events.
11. Print the root-port Link Capabilities and Link Status (Gen/width).
12. `finalize()`: INTx routing into `PCI_interrupt_line`.

## Verified vs untested

Verified:
* `StarFivePCIe.cpp` and `kernel_interface.cpp` compile with no warnings
  under `-Wall -Wextra` using the riscv64 cross g++ 13.3.0 from
  `~/beacon/haiku/generated.riscv64/cross-tools-riscv64`. The flags were
  built by hand to match Haiku's riscv64 kernel add-on rules (`-march=rv64gc
  -mno-relax -mcmodel=medany -fpic -fno-rtti -fno-exceptions -D_KERNEL_MODE
  ...`). The existing `designware` driver compiles cleanly with the same
  command, which is the control case. The undefined symbols are the same
  kernel exports that `designware` uses, plus `reserve_io_interrupt_vectors`
  and `free_io_interrupt_vectors`.
* Every register offset was cross-checked between Linux v6.12 and the vendor
  U-Boot (absolute syscon offsets 0xc0/0xc4/0x130 and 0x270/0x274/0x2e0
  match).

Not verified (no jam build, no hardware, no emulator models this SoC):
* Linking (`jam -q '<pci>starfive'`) and packaging.
* Everything at run time: link training, config access, BAR assignment,
  INTx, MSI/MSI-X, DMA through the inbound window.
* Whether `get_driver()` on phandle targets (PHY, syscon, GPIO) succeeds this
  early. If it fails, the JH7110 constants are used and nothing is lost.

Known limitations and risks:
* `PCI_interrupt_line` is 8 bits wide, so INTx vectors must be below 256. The
  driver refuses INTx otherwise. MSI vectors have no such limit.
* Haiku's riscv64 `arch_int.cpp` reserves only PLIC sources 0-127 (`TODO:
  read from FDT`), but the JH7110 PLIC has 136. The driver reserves
  128-135 so that dynamic vectors cannot alias real sources. The proper fix
  belongs in `arch_int.cpp` (read `riscv,ndev`).
* `install_io_interrupt_handler()` on a dynamic vector also pokes PLIC
  priority/enable for that number. This is pre-existing Haiku behaviour,
  which `designware` shares. It is harmless above 136.
* INTx cannot be masked per pin from Haiku. All four pins stay unmasked, so a
  device that asserts INTx with no driver attached would storm.
* CLKREQ#/WAKE# pad setup (upstream: input, pull-down/up) is left to
  firmware.
* No I/O-port windows. The JH7110 DTs declare none.
* The VL805 on the VisionFive 2 has its own SPI firmware flash. If the Orange
  Pi RV relies on host-loaded VL805 firmware, as the Raspberry Pi 4 does,
  `xhci` will not come up. Check with the board commands below.

## Test plan on real hardware (serial console)

1. Build only the add-on and the image when the machine is free:
   `jam -q '<pci>starfive'`, then the normal riscv64 image. Boot with the
   serial debug output on (115200 8N1, GPIO 5/6 UART0).
2. Look for lines prefixed with `starfive_pcie:`. Per port you should see:
   apb/cfg addresses, `port N, PLIC interrupt 56/57`, both ranges, ATR
   entries 0-2, `port N link up`, `root port xxxx:xxxx`, `link: max Gen2 x1,
   current Gen2 x1` (or Gen1), and `INTA..INTD -> vectors ...`.
3. Then the PCI bus manager's dump (`pci_print_info`) should show domain 0
   and domain 1. Expect the VL805 at `1106:3483` (class 0c0330) and the NVMe
   SSD (class 010802), each behind a `0604` root port.
4. In KDL: `pci_dump`/`pciinfo` and `ints` (look for the xhci/nvme vectors
   counting up). Then plug in a USB stick and check that `usb_disk` appears.
   With an NVMe disk, check `/dev/disk/nvme/0/raw`.
5. Failure triage:
   * `link down`: print LNKSTA, check the PERST# GPIO (measure the pin, or try
     the other polarity), check PHY0 mode, try `pci enum` in U-Boot first to
     compare.
   * Hang on the first config read: the ATR config entry or clocks/resets.
     Rebuild with `TRACE_STARFIVE_PCIE`.
   * Device found but no interrupts: check the `ints` counters for PLIC
     56/57. If those count but the device vector does not, check the MSI
     bits in `ISTATUS_MSI` (add a dprintf) and try booting with MSI disabled
     to fall back to INTx.
   * `bridge error event`: AXI/PCIe post/fetch/discard errors, usually a bad
     window.

## Read-only checks to run on the board's Debian (Linux)

```sh
cat /proc/device-tree/model; echo
tr '\0' '\n' < /proc/device-tree/compatible
ls /proc/device-tree/soc/ | grep -i pcie
for n in /proc/device-tree/soc/pcie@*; do echo "== $n"; \
  tr '\0' ' ' < $n/reg-names 2>/dev/null; echo; tr '\0' ' ' < $n/status; echo; \
  ls $n; done
hexdump -C /proc/device-tree/soc/pcie@*/perst-gpios 2>/dev/null
hexdump -C /proc/device-tree/soc/pcie@*/starfive,stg-syscon
lspci -nn -tv
lspci -vv          # as root, this also shows the capability details (link speed/width, MSI/MSI-X)
dmesg | grep -iE 'pcie|pci |plda|xhci|vl805|nvme'
cat /proc/interrupts | grep -iE 'pcie|plda|xhci|nvme'
uname -a; cat /proc/cmdline
ls /boot/dtbs/*/starfive/ 2>/dev/null; cat /boot/extlinux/extlinux.conf 2>/dev/null
```

And at the U-Boot prompt (read-only): `pci enum; pci`, `printenv bootcmd
boot_devs bootmode fdtfile`, `fdt addr $fdtcontroladdr; fdt print /soc/pcie@2B000000`.
