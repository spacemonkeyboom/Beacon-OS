# Third-party sources: StarFive JH7110 PCIe driver

Local copies of everything listed here are in `~/beacon/refs/jh7110/`. Their
SHA-256 sums are in `~/beacon/refs/jh7110/SHA256SUMS`. All were downloaded
on 2026-11-20.

## License of this add-on

| File | License | Why |
|------|---------|-----|
| `StarFivePCIe.cpp` | GPL-2.0-only | Linux- and U-Boot-derived register maps, sequences and logic. It also has Haiku MIT code (ranges parser, module glue shape), which is allowed inside a GPL file. |
| `StarFivePCIe.h` | GPL-2.0-only | Register definitions taken from Linux headers and drivers. |
| `kernel_interface.cpp` | MIT (Haiku) | Copied from `designware/kernel_interface.cpp`. It has no Linux code. It only includes the GPL header, so the linked add-on as a whole is GPL-2.0-only. |
| `Jamfile`, `README.md`, `THIRD_PARTY.md` | MIT (Beacon/Haiku build files and docs) | |

The add-on as a whole is **GPL-2.0-only**. It is a separate kernel add-on
(`<pci>starfive`) and is not linked into the core kernel.

GPL-2.0-only is the right choice because the inputs are a mix of
"GPL-2.0" (meaning only), "GPL-2.0+" and "GPL-2.0-or-later" files. The only
license that satisfies all of them is GPL-2.0-only.

## Sources

### Linux kernel: tag v6.12 (commit 06090c9b622a7e1f797e775db4c035e0d779b76e)
Repo: https://github.com/torvalds/linux

| Path | SPDX | Used for |
|------|------|----------|
| drivers/pci/controller/plda/pcie-starfive.c | GPL-2.0+ | STG syscon offsets and bits, host init sequence, RC BAR hiding, link wait |
| drivers/pci/controller/plda/pcie-plda.h | GPL-2.0 | PLDA bridge register map, interrupt bits, inline init helpers |
| drivers/pci/controller/plda/pcie-plda-host.c | GPL-2.0 | ATR window programming (`plda_pcie_setup_window`), INTx/MSI demux, ECAM offset |
| drivers/pci/controller/plda/pcie-microchip-host.c | GPL-2.0 | Reference only (IMSI_ADDR use, IMASK init). No code taken |
| drivers/phy/starfive/phy-jh7110-pcie.c | GPL-2.0+ | KVCO tuning, PCIe/USB3 mode bits |
| drivers/clk/starfive/clk-starfive-jh71x0.{c,h} | GPL-2.0 | Clock register layout (base + 4*id, bit 31 gate) |
| drivers/clk/starfive/clk-starfive-jh7110-{stg,sys}.c | GPL-2.0 | Which PCIe clocks exist and which are critical |
| drivers/reset/starfive/reset-starfive-jh7110.c | GPL-2.0-or-later | Reset assert/status offsets (STG 0x74/0x78, SYS 0x2f8/0x308) |
| drivers/reset/starfive/reset-starfive-jh71x0.c | GPL-2.0-or-later | Reset polarity and status polling |
| drivers/pinctrl/starfive/pinctrl-starfive-jh7110{,-sys}.c | GPL-2.0 | GPIO DOUT/DOEN register layout (PERST#) |
| include/dt-bindings/clock/starfive,jh7110-crg.h | GPL-2.0 OR MIT | Clock IDs |
| include/dt-bindings/reset/starfive,jh7110-crg.h | GPL-2.0 OR MIT | Reset IDs |
| include/dt-bindings/pinctrl/starfive,jh7110-pinctrl.h | GPL-2.0 OR MIT | GPOUT/GPOEN values |
| arch/riscv/boot/dts/starfive/jh7110.dtsi, jh7110-common.dtsi, jh7110-starfive-visionfive-2.dtsi | GPL-2.0 OR MIT | Addresses, interrupts, PERST# pins. Reference only |
| Documentation/devicetree/bindings/pci/starfive,jh7110-pcie.yaml | GPL-2.0-only OR BSD-2-Clause | Binding reference |

### Linux kernel: master (commit 65e4fd942f72a9dba0e4e257290c4fa1aa756854 at download time)
| arch/riscv/boot/dts/starfive/jh7110-orangepi-rv.dts | GPL-2.0 OR MIT | Copyright (C) 2025 Icenowy Zheng. Confirms that Orange Pi RV enables pcie0 and pcie1 with the common VF2 pins. Reference only |

### U-Boot (Orange Pi vendor): orangepi-xunlong/u-boot-orangepi, branch v2021.10-jh7110, commit d7274dfee9dbcc8b8f39d459d2dcb596abd9e157
Repo: https://github.com/orangepi-xunlong/u-boot-orangepi

| Path | SPDX | Used for |
|------|------|----------|
| drivers/pci/pcie_starfive.c | GPL-2.0+ | Vendor DT binding (reg-names "reg"/"config", syscon cells), ATR config workaround comment, only-one-device rule |
| arch/riscv/dts/jh7110.dtsi, jh7110-u-boot.dtsi, starfive_visionfive2.dts, starfive_visionfive2-u-boot.dtsi | GPL-2.0 OR MIT | Vendor DT layout. Reference only |
| board/starfive/visionfive2/{spl.c,starfive_visionfive2.c}, arch/riscv/cpu/jh7110/spl.c, include/configs/starfive-visionfive2.h (GPL-2.0+), configs/starfive_visionfive2_defconfig (no header; U-Boot is GPL-2.0+) | GPL-2.0+ | Shows what the firmware does before Beacon runs. Reference only |

### U-Boot (upstream): tag v2024.10 (commit 573d69af36cfb10d58e189656b03a659654b0cd9)
Repo: https://github.com/u-boot/u-boot

| drivers/pci/pcie_starfive_jh7110.c, drivers/pci/pcie_plda_common.{c,h}, arch/riscv/dts/jh7110.dtsi, board/starfive/visionfive2/*, drivers/clk/starfive/clk-jh7110.c | GPL-2.0+ (the dtsi is GPL-2.0 OR MIT) | Cross-check only. No code taken |

### Haiku: hrev60441 (branch `beacon` base)
| src/add-ons/kernel/busses/pci/designware/{DWPCIController.cpp,DWPCIController.h,MsiInterruptCtrlDW.cpp,kernel_interface.cpp} | MIT, Copyright 2022 Haiku, Inc. | Module structure, FDT ranges parser, MSIInterface pattern |
