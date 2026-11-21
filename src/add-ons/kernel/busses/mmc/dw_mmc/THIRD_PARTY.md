<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# Third-party sources: DesignWare MMC host driver (dw_mmc)

Local copies of the upstream files are in
`~/beacon/refs/jh7110/dwmmc-2026-11-20/`. Their SHA-256 sums are in
`~/beacon/refs/jh7110/SHA256SUMS`. All were downloaded on 2026-11-20.

## License of this add-on

| File | License | Why |
|------|---------|-----|
| `DwMmc.h` | GPL-2.0-or-later | Register map and bit definitions from Linux `dw_mmc.h` and U-Boot `dwmmc.h`. |
| `DwMmc.cpp` | GPL-2.0-or-later | Reset, CIU clock update, divider, command flags, FIFO PIO and error recovery adapted from Linux `dw_mmc.c` and U-Boot `dw_mmc.c`. The reply-type table follows Haiku's MIT `sdhci.cpp`, which may be included in a GPL file. |
| `Jh7110Gpio.h`, `Jh7110Gpio.cpp` | MIT (Beacon) | Minimal JH7110 SYS GPIO helper. Register layout **facts only** (DOEN 0x000 / DOUT 0x040 / GPIOIN 0x118 / PADCFG 0x120 + 4*pin, byte per pin, GPOUT_LOW/HIGH = 0/1, GPOEN_ENABLE = 0, PADCFG bits, SD0 pads 64-74) from Linux v6.12 `drivers/pinctrl/starfive/pinctrl-starfive-jh7110.c` and `pinctrl-starfive-jh7110-sys.c` (GPL-2.0-only) and `include/dt-bindings/pinctrl/starfive,jh7110-pinctrl.h`. No code copied. |
| `kernel_interface.cpp` | MIT (Beacon) | New device manager and FDT glue with no third-party code. It includes the GPL header, so the linked add-on is GPL. |
| `Jamfile`, `README.md`, `THIRD_PARTY.md` | MIT (Beacon) | Build files and docs. |

The add-on as a whole is **GPL-2.0-or-later**. It is a separate kernel
add-on (`busses/mmc/dw_mmc`) and is not linked into the core kernel.

GPL-2.0-or-later works because the inputs are Linux `dw_mmc.[ch]`
(GPL-2.0-or-later) and U-Boot `dw_mmc.c`/`dwmmc.h` (GPL-2.0+, the same
thing). No GPL-2.0-only code was copied. The GPL-2.0-only StarFive glue
(`dw_mmc-starfive.c`) was only read for facts: its compatible string, and
that HS200/DDR need sample-phase tuning, which this driver does not do.

## Sources

### Linux kernel, tag v6.12 (commit adc218676eef25575469234709c2d87185ca223a)
Repo: https://github.com/torvalds/linux

| Path | SPDX | Used for |
|------|------|----------|
| drivers/mmc/host/dw_mmc.h | GPL-2.0-or-later | Register offsets, CTRL/CMD/INT/STATUS/FIFOTH/HCON bit layouts |
| drivers/mmc/host/dw_mmc.c | GPL-2.0-or-later | `dw_mci_ctrl_reset`, `mci_send_cmd` (clock update), `dw_mci_setup_bus` (divider, CLKENA low power), `dw_mci_prepare_command` (STOP/PRV_DAT_WAIT/RESP/USE_HOLD flags), `dw_mci_wait_while_busy`, `dw_mci_reset` (recovery), probe order (HCON width, FIFOTH, VERID data offset, INTMASK, INT_ENABLE), error flag sets, PIO read/write structure, R2 register order |
| drivers/mmc/host/dw_mmc-starfive.c | GPL-2.0 (only) | Facts only, no code: compatible `starfive,jh7110-mmc`, tuning register exists |
| drivers/mmc/host/dw_mmc-pltfm.c | GPL-2.0-or-later | Read only (platform probe flow) |

### U-Boot, tag v2024.10 (commit f919c3a889f0ec7d63a48b5d0ed064386b0980bd)
Repo: https://github.com/u-boot/u-boot

| Path | SPDX | Used for |
|------|------|----------|
| include/dwmmc.h | GPL-2.0+ | Register and bit names (cross-check with Linux) |
| drivers/mmc/dw_mmc.c | GPL-2.0+ | FIFO-mode data path (`dwmci_data_transfer`), per-transfer FIFO reset, BLKSIZ/BYTCNT setup, `dwmci_setup_bus`, `dwmci_init` |
| drivers/mmc/snps_dw_mmc.c | GPL-2.0+ | Read only (`fifo-depth`, clock handling) |

### orangepi-xunlong/u-boot-orangepi, branch v2021.10-jh7110 (commit d7274dfee9dbcc8b8f39d459d2dcb596abd9e157)
Repo: https://github.com/orangepi-xunlong/u-boot-orangepi

| Path | SPDX | Used for |
|------|------|----------|
| drivers/mmc/dw_mmc.c | GPL-2.0+ | StarFive change: send the 80 init clocks (`DWMCI_CMD_SEND_INIT`) with the first command |
| include/dwmmc.h | GPL-2.0+ | `DWMCI_CMD_SEND_INIT`, `DWMCI_UHS_REG_EXT` |
| arch/riscv/dts/jh7110.dtsi, starfive_visionfive2.dts | GPL-2.0 OR MIT | Vendor DT as passed to Haiku by vendor U-Boot: `snps,dw-mshc`, 50 MHz card clock, `fifo-depth = <32>`, no card-detect property (already in `~/beacon/refs/jh7110/u-boot-orangepi-2021.10-jh7110/`) |

### Device trees and clock/reset facts (already in `~/beacon/refs/jh7110/`)

| Path | Source | Used for |
|------|--------|----------|
| arch/riscv/boot/dts/starfive/jh7110.dtsi, jh7110-common.dtsi | Linux v6.12 | `mmc0`/`mmc1` reg, interrupts 74/75, `fifo-depth`, `assigned-clock-rates = 50 MHz`, `cd-gpios` on mmc1 |
| arch/riscv/boot/dts/starfive/jh7110-orangepi-rv.dts | Linux master 65e4fd9 | mmc0 is the AP6256 SDIO Wi-Fi (`mmc-pwrseq`, `wifi@1`), mmc1 is the SD slot |
| include/dt-bindings/clock/starfive,jh7110-crg.h, dt-bindings/reset/starfive,jh7110-crg.h | Linux v6.12 (GPL-2.0 OR MIT) | SDIO clock (91-94) and reset (64/65) indices, read-only diagnostics |
| drivers/clk/starfive/clk-starfive-jh71x0.h, drivers/reset/starfive/reset-starfive-jh7110.c, reset-starfive-jh71x0.c | Linux v6.12 | Clock register layout (bit 31 enable, 23:0 divider), reset status offset 0x308 and polarity |

### Beacon SDIO additions (2026-11-20)

| Source | SPDX | Used for |
|--------|------|----------|
| Linux v6.12 drivers/mmc/host/dw_mmc.c, dw_mmc.h (copies in `~/beacon/refs/jh7110/dwmmc-2026-11-20/linux/`) | GPL-2.0-or-later | `SDMMC_INT_SDIO(n)` = bit 16+n, CMD52-to-CCCR-ABORT gets `SDMMC_CMD_STOP`, `dw_mci_prepare_sdio_irq` (low-power gate off for SDIO), ISR ack + mask of the SDIO bit, `ack_sdio_irq` = unmask, final partial FIFO word (`dw_mci_pull_final_bytes`) |
| Linux v6.12 drivers/mmc/core/pwrseq_simple.c, core.c `mmc_power_up()` (copies in `~/beacon/refs/sdio-2026-11-20/linux/`) | GPL-2.0 | Facts: reset asserted before power-up, released after `power_delay_ms` (10), then `post-power-on-delay-ms`, then clock and 10 ms more |
| Linux master arch/riscv/boot/dts/starfive/jh7110-orangepi-rv.dts | GPL-2.0 OR MIT | `wifi_pwrseq` = GPIO62 active low, `&mmc0` `wifi@1` brcm,bcm43456-fmac, `cap-sd-highspeed` |

### Haiku (MIT)

| Path | Used for |
|------|----------|
| src/add-ons/kernel/busses/mmc/sdhci.cpp | Command-to-reply-type mapping, `mmc_bus_interface` usage pattern |
