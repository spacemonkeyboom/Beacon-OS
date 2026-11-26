# Third-party sources: cdns3_jh7110

## License of this add-on

| File | License | Why |
|------|---------|-----|
| `cdns3_jh7110.cpp` | GPL-2.0-only | Register maps, bit values and init sequences from the Linux files below. |
| `Jh7110Gpio.cpp/.h` (built from `../../mmc/dw_mmc/`) | MIT (Beacon) | Shared GPIO helper, see dw_mmc/THIRD_PARTY.md. |
| `Jamfile`, `README.md`, `THIRD_PARTY.md` | MIT | |

The add-on as a whole is **GPL-2.0-only** (inputs are GPL-2.0, GPL-2.0+ and
GPL-2.0-or-later). It is a separate kernel add-on and is not linked into the
kernel or into the MIT `<usb>xhci` add-on; the two talk only through device
manager node attributes.

## Sources

### Linux kernel, torvalds/linux commit 25faec9ce61c01fb45abf812a222c5b43c3cd5c5 (2026-11-25)
Local sparse copy: ~/beacon/refs/vf2lite-2026-11-26/linux-mainline

| Path | SPDX | Used for |
|------|------|----------|
| drivers/usb/cdns3/cdns3-starfive.c | GPL-2.0 | STG syscon USB mode bits, clock/reset bring-up order |
| drivers/usb/cdns3/drd.c, drd.h | GPL-2.0 | DRD version detection, OTG register layout, host-on sequence |
| drivers/phy/starfive/phy-jh7110-usb.c | GPL-2.0+ | USB 2 PHY init and host mode |
| drivers/phy/starfive/phy-jh7110-pcie.c | GPL-2.0+ | KVCO tune, USB 3 mode of PCIe PHY 0 |
| drivers/clk/starfive/clk-starfive-jh7110-pll.c | GPL-2.0 | PLL0 rate formula and registers |
| drivers/clk/starfive/clk-starfive-jh7110-{sys,stg}.c, clk-starfive-jh71x0.h | GPL-2.0 | clock indices, gate/divider bits |
| include/dt-bindings/{clock,reset}/starfive,jh7110-crg.h | GPL-2.0 OR MIT | clock/reset ids |
| arch/riscv/boot/dts/starfive/jh7110*.dts* | GPL-2.0 OR MIT | addresses, IRQ, VF2 Lite usb0 pins |

### StarFive SDK U-Boot, starfive-tech/u-boot JH7110_VisionFive2_devel commit c4c67bb66ae6f41c98537d18cf5c3abc8b97b8e4
Local sparse copy: ~/beacon/refs/vf2lite-2026-11-26/u-boot-starfive

| Path | SPDX | Used for |
|------|------|----------|
| arch/riscv/dts/jh7110.dtsi, starfive_visionfive2*.dts | GPL-2.0 OR MIT | the vendor `usbdrd30` node layout (`starfive,usb2-only`, `dr_mode`) |
