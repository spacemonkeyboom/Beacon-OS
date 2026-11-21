<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# Third-party sources: DesignWare Ethernet QoS driver (dwmac)

## License of this add-on

| File | License | Why |
|------|---------|-----|
| `Dwmac.h` | GPL-2.0-only | Register map and bit definitions from U-Boot `dwc_eth_qos.c` (GPL-2.0) and Linux `dwmac4*.h` (GPL-2.0-only); JH7110 clock/reset/syscon facts. |
| `Dwmac.cpp` | GPL-2.0-only | Bring-up, MTL/MAC/DMA setup, descriptor handling, stop/drain and MDIO access adapted from U-Boot `dwc_eth_qos.c`; JH7110 mode/clock handling following Linux `dwmac-starfive.c` and the vendor board file (GPL-2.0+). |
| `YtPhy.cpp` | GPL-2.0-or-later | YT8531 extended registers, DT tuning fields and status decode from vendor U-Boot `drivers/net/phy/motorcomm.c` (GPL-2.0+). |
| `kernel_interface.cpp` | MIT (Beacon) | New device manager / FDT glue. It includes the GPL header, so the linked add-on is GPL. |
| `Jamfile`, `README.md`, `THIRD_PARTY.md` | MIT (Beacon) | Build files and docs. |

The add-on as a whole is **GPL-2.0-only** (U-Boot's `dwc_eth_qos.c` is
GPL-2.0 without "or later"; GPL-2.0+ parts combine into that). It is a
separate kernel add-on and is not linked into the core kernel.

## Sources

### orangepi-xunlong/u-boot-orangepi, branch v2021.10-jh7110 (commit d7274dfee9dbcc8b8f39d459d2dcb596abd9e157)
Local sparse clone: `~/beacon/refs/jh7110/u-boot-orangepi-full` (files read
with `git show HEAD:<path>`; not checked out there).

| Path | SPDX | Used for |
|------|------|----------|
| drivers/net/dwc_eth_qos.c | GPL-2.0 | Register layout, `eqos_start` order (MTL TSF/RSF, TQS/RQS from HW_FEATURE1, flow-control thresholds, RXQ0EN DCB, MCBCQEN, pause, CST/ACS, OSP, RBSZ, PBLx8 + DSL, TX PBL, SYSBUS value, ring setup, RX tail at last descriptor), `eqos_send`/`recv`/`free_pkt` descriptor handling, `eqos_stop` drain loops, MDIO read/write, `eqos_adjust_link` speed/duplex bits, JH7110 config (MDIO CR 250-300, 64-bit AXI, 64-byte descriptor slots), TX clock rates per speed |
| drivers/net/phy/motorcomm.c | GPL-2.0+ | YT8531 ID/mask, ext register access, `ytphy_of_config`, `ytphy_of_inverted`, `ytphy_parse_status` |
| arch/riscv/cpu/jh7110/cache.c | GPL-2.0+ | Facts: L2 Flush64 at 0x2010000+0x200, per 64-byte line, used for flush and invalidate |
| configs/starfive_visionfive2_defconfig | - | Facts: `STARFIVE_JH7110_L2CC_FLUSH=y`, `DWC_ETH_QOS_STARFIVE=y`, `PHY_YUTAI=y` |
| board/starfive/visionfive2/starfive_visionfive2.c | GPL-2.0+ | Facts: interface select registers/values, TX mux handling, OTP MAC |
| arch/riscv/include/asm/arch-jh7110/jh7110-regs.h | GPL-2.0+ | Facts: syscon/CRG bases, GMAC select fields |
| arch/riscv/cpu/jh7110/pll.c | GPL-2.0+ | Facts: PLL0 register fields |
| drivers/clk/starfive/clk-jh7110.c, include/dt-bindings/clock/starfive-jh7110-clkgen.h, include/dt-bindings/reset/starfive-jh7110.h | GPL-2.0+ | Cross-check of GMAC clock and reset indices |

`~/beacon/refs/jh7110/u-boot-orangepi-2021.10-jh7110/arch/riscv/dts/jh7110.dtsi`
and `starfive_visionfive2.dts` (GPL-2.0 OR MIT): the DT passed to Haiku
(compatible, reg, `interrupts = <7>` / `<78>`, `phy-mode = "rgmii-id"`, PHY
child nodes with the YT8531 tuning properties, no `reg` in the PHY nodes).

### Linux kernel, tag v6.12
Read 2026-11-20 from https://raw.githubusercontent.com/torvalds/linux/v6.12/
(public source, used for facts and bit layouts; screened, nothing unusual).

| Path | SPDX | Used for |
|------|------|----------|
| drivers/net/ethernet/stmicro/stmmac/dwmac4_dma.h | GPL-2.0-only | DMA_STATUS MAC/MTL bits, channel register offsets (INTR_ENA +0x34, STATUS +0x60, CUR desc), 4.10+ interrupt enable/status bits |
| drivers/net/ethernet/stmicro/stmmac/dwmac4_descs.h | GPL-2.0-only | TDES2/TDES3/RDES3 bit layouts (IOC, FD, LD, BUF1V, ES, CTXT, lengths) |
| drivers/net/ethernet/stmicro/stmmac/dwmac4.h | GPL-2.0-only | MAC register offsets (packet filter, INT_STATUS/EN, PHYIF, DEBUG, HW_FEATURE0-3, ADDR64 field) |
| drivers/net/ethernet/stmicro/stmmac/dwmac-starfive.c | GPL-2.0+ | Interface select values (RGMII 1, RMII 4, 3-bit field), TX clock rates per speed |

Already in `~/beacon/refs/jh7110/linux/`:

| Path | Used for |
|------|----------|
| arch/riscv/boot/dts/starfive/jh7110.dtsi | gmac clocks/resets, interrupts 7/78, `starfive,syscon` args (aon 0xc/0x12, sys 0x90/0x2), ccache node; no `dma-noncoherent` |
| include/dt-bindings/clock/starfive,jh7110-crg.h, include/dt-bindings/reset/starfive,jh7110-crg.h (GPL-2.0 OR MIT) | Clock indices 97-111 (SYS), 2-8 (AON); resets SYS 66/67, AON 0/1 |
| drivers/clk/starfive/clk-starfive-jh71x0.h, clk-starfive-jh7110-sys.c | Clock register layout (enable 31, invert 30, mux 27:24, div 23:0), GMAC clock types/parents/max dividers |
| drivers/reset/starfive/reset-starfive-jh7110.c, reset-starfive-jh71x0.c | Reset assert/status offsets (SYS 0x2f8/0x308, AON 0x38/0x3c) and polarity |

MMIO addresses from the MAC/MTL/MMC dwmac4 layout not quoted above (MMC
interrupt masks 0x70c/0x710/0x800, VERSION 0x110) are standard dwmac4
register offsets as used by Linux `stmmac_mmc_core.c` / `common.h`.

### Haiku (MIT)

| Path | Used for |
|------|----------|
| src/add-ons/kernel/drivers/network/ether/virtio/virtio_net.cpp | driver/device module split, ioctl set, net_buffer receive/send pattern |
| src/add-ons/kernel/busses/mmc/dw_mmc (Beacon) | FDT attach pattern, big-endian property reads, JH7110 CRG access style |

### eth2 additions (2026-11-20)

| Source | Used for |
|--------|----------|
| Linux v6.12 `dwmac4_dma.h` (GPL-2.0-only) | DMA_DEBUG_STATUS0 (0x100c) RPS0/TPS0 fields and process-state values used by the watchdog |
| Linux `jh7110-orangepi-rv.dts` (GPL-2.0 OR MIT, `~/beacon/refs/jh7110/linux/master`) and `dwmac-starfive.c` | Facts: only gmac0 is enabled on the Orange Pi RV; `starfive,tx-use-rgmii-clk` means no TX clock rate changes |
| Haiku `src/add-ons/kernel/network/devices/ethernet/ethernet.cpp` (MIT) | Fact: the stack waits on the link-state semaphore instead of polling, and frees the buffer when `ETHER_SEND_NET_BUFFER` fails |

Reviewed, **not used**: X512's MIT dwmac/StarfiveClock/StarfiveReset from
X547/haiku branch `visionfive2` (2023, saved in
`~/beacon/refs/rv-ports-2026-11-20/x547-visionfive2-src/`). It is an
unfinished skeleton, so no code was copied and its licence does not apply
here. The watchdog, power-down and stall-reset code is new Beacon code inside
the GPL-2.0-only `Dwmac.cpp`.
