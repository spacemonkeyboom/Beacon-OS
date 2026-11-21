<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# dwmac: Synopsys DesignWare Ethernet QoS (eqos 5.20)

Native Haiku network driver for the Synopsys DesignWare Ethernet QoS
controller (Linux "stmmac"/"dwmac4", U-Boot "eqos"). The first target is the
StarFive JH7110 (Orange Pi RV, VisionFive 2):

| Node | Address | PLIC (macirq) | Clocks / resets |
|------|---------|---------------|-----------------|
| gmac0 | 0x16030000 | 7 | AONCRG (ahb, axi, tx) + SYSCRG (gtx, ptp, gtxc); AONRST 0/1 |
| gmac1 | 0x16040000 | 78 | SYSCRG (ahb, axi, tx, ptp, gtxc); SYSRST 66/67 |

PHYs: Motorcomm YT8531 over RGMII ("rgmii-id").

Status (2026-11-20, branch beacon-eth2): **pending hardware test**. Attach
ran on the board in rv14-rv18 (clocks, reset, PHY ID 0x4f51e91b, rings); no
packet has been exchanged yet. Build-tested with -Werror and link-checked
against kernel_riscv64 exports by `~/beacon/tools/eth2-compile-check.sh`.
Back in the image (`build/jam/packages/Haiku`, `dwmac@riscv64`, so the
minimum and the nightly/regular riscv64 images both carry it).

### eth2 changes (why it was left out, and what changed)

dwmac was dropped from rv19 because rv16/rv18 panicked on first boot. Those
panics happened before the interface was opened (DMA not started, ring
registers zero) and look like the RISC-V stale-TLB / PTE-race bugs fixed
since. The review found real bugs anyway, now fixed:

* Link was never seen: the stack registers `ETHER_SET_LINK_STATE_SEM` and
  then only asks for the link when the semaphore is released. A watchdog
  thread now polls the PHY once a second.
* gmac1 (no PHY on the Orange Pi RV) failed attach with clocks open and its
  reset released. Attach failure and uninit now stop the DMA, assert the
  resets and gate the clocks this driver opened (`_PowerDown`), before the
  DMA memory is freed. A controller firmware left out of reset is held in
  reset before any clock is opened.
* No recovery: fatal bus errors, RX/TX DMA "stopped", or TX descriptors the
  DMA never completes (3 s with link) now stop, soft-reset and restart the
  DMA with fresh rings (at most every 5 s). RX "suspended" re-arms the tail.
* Without link a full TX ring blocked the stack: it now returns `ENETDOWN`.
* DT `starfive,tx-use-rgmii-clk` (mainline DTs) leaves TX clock rates alone,
  like Linux. The vendor U-Boot DT (what Haiku gets today) lacks it, so the
  vendor U-Boot rate handling stays in use.

X512's MIT driver (X547/haiku `visionfive2`, 2023) was compared and not
used: it is an unfinished skeleton (`Start()` returns `B_ERROR`, empty clock/
reset hooks, no PHY or link code, no cache maintenance, `SendData` returns
`B_ERROR`, `Recv` uses the TX ring). Its register structs would not make
this driver simpler; nothing was copied from it.

## Why a native driver

The FreeBSD compat layer expects PCI or a FreeBSD bus (simplebus/ofw) with
`bus_dma`, `ifmedia`, `miibus` and a FreeBSD clock/reset framework
(`clk_get_by_ofw_*`, `hwreset_*`). Haiku has none of that for FDT on
riscv64, and FreeBSD's `dwc` driver is the older GMAC, not EQoS. A native
device-manager driver (like `virtio_net`) needs only the FDT bus Haiku
already has, so it is the shorter path.

## How it attaches

    fdt node (starfive,jh7110-eqos-5.20 | starfive,jh7110-dwmac | snps,dwmac-5.20)
      -> drivers/network/dwmac/driver_v1   (this add-on, B_KEEP_DRIVER_LOADED)
        -> publishes /dev/net/dwmac/<n>     (device_v1)
          -> network stack "ethernet" device module

FDT nodes are generic device nodes, so the device manager looks for a driver
under all of `drivers/` when it reprobes after the boot volume is mounted.
The add-on lives in `add-ons/kernel/drivers/network/dwmac` (not a boot
module; networking is not needed to boot). Both gmac nodes attach if their
`status` is okay.

DT data used: `reg`, `interrupts[0]` (fallback 7/78 if missing),
`phy-mode`, `local-mac-address`/`mac-address`, and the PHY child node
(`ethernet-phy@N` directly under the MAC, or under `mdio`): `reg`/unit
address and the vendor YT8531 tuning properties `rxc_dly_en`,
`rgmii_sw_dr`, `rgmii_sw_dr_2`, `rgmii_sw_dr_rxc`, `rx_delay_sel`,
`tx_delay_sel_fe`, `tx_delay_sel`, `tx_inverted_10/100/1000`.

## Bring-up (JH7110)

Firmware is not trusted: vendor U-Boot's `eqos_stop()` gates the clocks and
asserts the resets after any network command, so they may be off.

1. Log reset state, set the PHY interface select (RGMII = 1) in AON syscon
   +0xc bits 20:18 (gmac0) or SYS syscon +0x90 bits 4:2 (gmac1).
2. Open the clock gates (bit 31) like Linux and the vendor U-Boot clock list.
   Muxes, inverters and dividers are left as firmware set them, and logged.
3. Reset pulse (assert, deassert, poll status) with clocks running.
4. DMA software reset (DMA_MODE.SWR). If it stalls (some PHYs stop the RGMII
   RX clock without a cable), attach anyway and retry on open.
5. Find the PHY (DT address first, then scan 0-31), apply the vendor YT8531
   tuning, advertise 10/100/1000 + pause, restart autonegotiation.
6. MAC address: DT `local-mac-address`/`mac-address` (U-Boot adds it), else
   what firmware left in MAC_ADDR0, else a fixed locally administered
   02:be:ac:07:11:0N (logged as a WARNING; not unique).
7. On open: MTL/MAC/DMA setup in vendor U-Boot `eqos_start()` order, rings
   loaded, DMA started. MAC TE/RE, speed/duplex, the GTX divider (from PLL0)
   and the YT8531 TX inversion are set whenever the link comes up.

Link is polled by the network stack's link checker once a second through
`ETHER_GET_LINK_STATE` (PHY interrupts are on a GPIO, no GPIO driver yet).

## DMA and cache coherency (the important part)

JH7110 DMA is treated as **not cache coherent**:

* The display controller here is confirmed non-coherent (PROGRESS.md), and
  the vendor U-Boot builds with `STARFIVE_JH7110_L2CC_FLUSH`, which flushes
  the SiFive L2 (ccache Flush64) for every eqos descriptor/buffer operation,
  for both "flush" and "invalidate".
* Upstream Linux 6.12 does not mark the JH7110 `dma-noncoherent`, and PCIe
  USB DMA has worked without flushes. So gmac may well be coherent. The
  scheme below is correct either way; on coherent hardware the flushes are
  only extra work.
* Uncached mappings are not available: the U74 cores have no Svpbmt and the
  JH7110 has no known uncached DRAM alias, so page attributes cannot make
  RAM uncached.

The design:

* One physically contiguous area below 4 GiB (`B_32_BIT_CONTIGUOUS`) holds
  64 TX + 128 RX descriptors and 2 KiB packet buffers. Below 4 GiB keeps the
  descriptor high-address words zero (the board has 8 GiB).
* Each 16-byte descriptor sits in its own 64-byte cache line (stride 64,
  DMA_CH0_CONTROL.DSL = 6 on the 64-bit AXI bus, the same layout vendor
  U-Boot uses on this board). Flushing one descriptor can never write stale
  data over a neighbour the DMA is updating.
* `_FlushPhys()` writes each line's physical address to ccache
  0x2010000 + 0x200 (Flush64 = write back + invalidate), with
  `fence iorw,iorw` before and after each write.
  * CPU -> device: fill the descriptor (OWN written last, after a fence) or
    the TX buffer, flush, fence, then write the tail pointer.
  * device -> CPU: flush (invalidate) the descriptor line before every OWN
    check, and the RX buffer before reading the frame.
  * RX buffers are flushed again when they go back to the DMA, so no line
    the CPU pulled in can be written back over new DMA data.
  * The CPU never writes a descriptor or buffer while the DMA owns it.
* Tail pointers: TX tail = next free slot (one slot always kept empty); RX
  tail = the most recently refilled descriptor (vendor U-Boot), starting at
  the last one.

If the ccache cannot be mapped, the driver logs an ERROR and runs without
flushes (a coherent-only mode).

## Interrupts

### Watchdog

One kernel thread per controller ("dwmac ethernet@16030000 watchdog", low
priority), from attach to uninit, ticking once a second: PHY link poll
(releases the stack's link semaphore on a change), and while open the stall
checks above plus one `stats:` line per minute.

PLIC "macirq" only. Channel 0 RI, TI, RBU, FBE (+ NIS/AIS summaries) are
enabled; MAC and MMC-counter interrupts are masked. The interrupt only
wakes the threads; every wait is capped at 100 ms and the rings are polled
anyway, so a broken PLIC route makes the driver slower, not stuck (logged
once after 16 packets with no interrupts).

## Expected serial lines on success (gmac0)

All lines start with `dwmac: ethernet@16030000:`. Values in `<>` vary.

    dwmac: ethernet@16030000: attaching: regs 0x16030000 (0x10000 bytes), PLIC irq 7, phy-mode "rgmii-id", PHY node ethernet-phy@0
    dwmac: ethernet@16030000: JH7110 gmac0: firmware left reset assert <..> status <..> (mask 0x3: ...)
    dwmac: ethernet@16030000: interface select AON syscon +0xc = <..> -> <..> (mode 1 -> 1, RGMII, phy-mode "rgmii-id")
    dwmac: ethernet@16030000: clock ahb        [  2] <before> -> <after> (div .., mux ..)[ ENABLED by driver]
    ... (axi, tx, gtxclk, ptp, gtxc, then an "(info)" line with the other clocks)
    dwmac: ethernet@16030000: PLL0 <Hz> Hz -> gtxclk <Hz> Hz (divider <n>)
    dwmac: ethernet@16030000: reset pulse done, status <..>
    dwmac: ethernet@16030000: VERSION 0x....52 (Synopsys 5.2, user ..)
    dwmac: ethernet@16030000: HW_FEATURE0 .. 1 .. 2 .. 3 ..
    dwmac: ethernet@16030000: FIFOs: TX <n> bytes, RX <n> bytes, address width <32|40>
    dwmac: ethernet@16030000: firmware left MAC_CONFIG .. / DMA_MODE .. / rings ..
    dwmac: ethernet@16030000: firmware left MAC_ADDR0 <mac> (high ..)
    dwmac: ethernet@16030000: DMA software reset done after ~<n> ms
    dwmac: ethernet@16030000: MAC address c0:74:2b:fc:f5:4d (from DT local-mac-address)
    dwmac: ethernet@16030000: PHY at MDIO address 0 (from DT): ID 0x4f51e91b (Motorcomm YT8531)
    dwmac: ethernet@16030000: PHY before config: BMCR .. BMSR .. ANAR .. CTRL1000 ..
    dwmac: ethernet@16030000: YT8531 before: chip_config(a001) .. rgmii_config1(a003) .. pad_drive(a010) ..
    dwmac: ethernet@16030000: YT8531 DT tuning: rxc_dly_en 0 sw_dr 3 ... rx_delay_sel 10 ... tx_delay_sel 10 ...
    dwmac: ethernet@16030000: YT8531 after: ...
    dwmac: ethernet@16030000: PHY autonegotiation restarted: ANAR 0x0de1 CTRL1000 0x0300 BMCR 0x1340
    dwmac: ethernet@16030000: DMA area 0x........-0x........ (64 TX + 128 RX descriptors at 64-byte stride, 2048-byte buffers), cache maintenance: SiFive ccache Flush64 per 64-byte line
    dwmac: ethernet@16030000: ready: MAC c0:74:2b:fc:f5:4d, PHY 0 id 0x4f51e91b, irq 7, link down (autonegotiation may still be running)
    dwmac: ethernet@16030000: published /dev/net/dwmac/0

When net_server opens the interface:

    dwmac: ethernet@16030000: open
    dwmac: ethernet@16030000: started: MTL tqs .. rqs .., TX pbl .. x8, DSL 6, CH0 ctl 0x00190000 ...
    dwmac: ethernet@16030000: TX clock for 1000 Mbit/s: gtx .., rmii_rtx .., tx mux .. (firmware choice, not changed)
    dwmac: ethernet@16030000: YT8531 tx_inverted for 1000 Mbit/s = 1 (rgmii_config1 ..)
    dwmac: ethernet@16030000: link up, 1000 Mbit/s full duplex (MAC_CONFIG .., PHYIF ..)
    /dev/net/dwmac/0: link up, media 0x...
    dwmac: ethernet@16030000: tx #1: 342 bytes, type 0800 to ff:ff:ff:ff:ff:ff   (DHCP discover)
    dwmac: ethernet@16030000: rx #1: <n> bytes, type 0800 from <router mac> (irqs <n>)

The first 8 TX and RX frames are logged; the first 16 RX drops / TX errors
are logged with their descriptor words.

gmac1 on the Orange Pi RV (no PHY) is expected to fail and power down:

    dwmac: ERROR: ethernet@16040000: no PHY answers on MDIO (addresses 0-31)
    dwmac: ERROR: ethernet@16040000: attach failed: No such device
    dwmac: ethernet@16040000: attach failed: controller held in reset (status ..)
    dwmac: ethernet@16040000: attach failed: 5 clocks gated again

Once a minute while open (bounded, one line):

    dwmac: ethernet@16030000: stats: link up 1000FD, rx <n> (+<n>) errors 0 missed 0, tx <n> (+<n>) errors 0, irqs <n> (rx .. tx .. rbu .. spurious ..), descriptor stalls 0, resets 0 (failed 0), rx re-arms <n>, dma running

On a stall (first 16 logged in full, then every 100th):

    dwmac: ERROR: ethernet@16030000: reset #1: TX descriptor stall (DMA keeps OWN) (CH0 status .. debug .. cur tx .. rx .., tx next .. dirty .., rx next ..)
    dwmac: ethernet@16030000: stopped (...)
    dwmac: ethernet@16030000: DMA software reset done after ~<n> ms
    dwmac: ethernet@16030000: reset #1 done, DMA running

## Test steps (board)

1. Boot the image with a cable from gmac0 (the only RJ45) to a router with
   DHCP. Capture serial; `grep dwmac:` must show the attach lines above,
   gmac1 powered down, then `open`, `started`, `link up, 1000 Mbit/s`.
2. Terminal: `ifconfig` -> `/dev/net/dwmac/0` with the MAC from the DT,
   "Link up" and (after a few seconds) an IPv4 address from DHCP. If not:
   `ifconfig /dev/net/dwmac/0 auto-config`.
3. `ping -c 3 8.8.8.8` (and the router IP) -> 0% loss.
4. Throughput without iperf: `wget -O /dev/null http://speedtest.tele2.net/100MB.zip`
   (or any large file on the LAN); note the rate. Then unplug for 5 s and
   replug: expect `link down` / `link up` lines and a working ping again.
5. Leave it 2+ minutes and check the `stats:` lines: errors, stalls and
   resets should stay 0; `rx re-arms` may grow under load (harmless).
6. If Wi-Fi is also up, disable it (`ifconfig <wlan> down`) for steps 3-4 so
   traffic takes the Ethernet route.


## Known gaps

* No packet exchanged on hardware yet (attach only).
* 10/100 Mbit/s: the GTX divider (max 15 from PLL0) cannot make 25 MHz or
  2.5 MHz. The driver sets the MAC speed bits and the rmii_rtx divider like
  vendor U-Boot, warns, and leaves the TX mux as firmware set it. Gigabit is
  the supported speed.
* Only the vendor YT8531 DT properties are applied; Linux-style
  `rx-internal-delay-ps` etc. are detected and logged, not applied.
* No checksum offload, TSO, jumbo frames, VLAN filtering, EEE/LPI, PTP or
  Wake-on-LAN. All multicast is accepted (no hash filter).
* No PHY interrupt (GPIO); link changes are seen within about a second.
* A fatal bus error or a stall resets the DMA (frames in the rings are
  lost). If the soft reset itself stalls (no RGMII RX clock), the watchdog
  retries every 5 s.
* The MAC fallback is a fixed made-up address when the DT has none.
* Reading the PHY child DT node uses a struct that mirrors the FDT bus
  manager's private `fdt_device` (only `node` is used). If that struct
  changes, update `fdt_device_proxy` in `kernel_interface.cpp`.
* The PLL0 rate formula (`24 MHz * fbdiv / prediv >> postdiv1`) is from the
  vendor/Linux PLL fields; the raw result is logged so it can be checked.
