# cdns3_jh7110: StarFive JH7110/JH7110S Cadence USB 3 host glue

Powers the on-chip Cadence USB 3 controller (cdns3) of the StarFive JH7110
family, puts it in host mode and publishes an `xhci_platform` device node.
Haiku's `<usb>xhci` add-on (MIT) attaches to that node (attributes
`xhci/phys`, `xhci/size`, `xhci/irq`) and drives the controller as a normal
xHCI. Written for the VisionFive 2 Lite, whose USB-A ports (1x USB 3.0 +
3x USB 2.0 via a hub) hang off this controller. On the VisionFive 2 and the
Orange Pi RV the same block is the USB-C gadget port (`dr_mode` peripheral),
and the driver leaves it alone.

What it does (Linux order): VBUS GPIO26 high and switch GPIO62 low (Lite
only); STG syscon strap = host, suspendm, PLL/refclk bits; usb_125m divider
from the real PLL0 rate (1.25 GHz on the JH7110S, 1.5 GHz on the JH7110);
USB0 clocks and resets; USB 2 PHY clock mode; PCIe PHY 0 KVCO tune and USB 3
mode (USB 3 boards); cdns3 DRD version check, `HOST_BUS_REQ | OTG_DIS`, wait
for `XHCI_READY`; USB 2 PHY host mode (LS keep-alive) and PHY routing.

Device tree: binds to `starfive,jh7110-cdns3` (StarFive SDK U-Boot DT, the
one Beacon boots with) or `starfive,jh7110-usb` (Linux DT), status okay.
Block addresses are the fixed JH7110 ones; the xHCI address and IRQ come from
the `cdns,usb3` child when present (default 0x10110000, PLIC 100).

Not done yet: the cdns3 xHCI start quirks from Linux host.c (EU3S,
CFG_RXDET_P3_EN, LPM_2_STB_SWITCH_EN), suspend/resume, gadget mode.

License: GPL-2.0-only (see THIRD_PARTY.md). Status: untested on hardware.
