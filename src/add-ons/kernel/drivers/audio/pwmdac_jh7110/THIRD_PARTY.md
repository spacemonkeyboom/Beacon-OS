<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# Third-party sources: JH7110 PWM-DAC audio driver (pwmdac_jh7110)

## License of this add-on

| File | License | Why |
|------|---------|-----|
| `PwmdacRegs.h` | GPL-2.0-only | PWMDAC register map, CTRL bit fields and default configuration from Linux `jh7110_pwmdac.c` (GPL-2.0) and the StarFive vendor `pwmdac.h` (GPL-2.0); STATE register notes from the JH7110 TRM. |
| `Pwmdac.cpp` | GPL-2.0-only | CTRL programming order, sample-rate table (N per rate, core clock = 256 x fs) and clock/reset bring-up order adapted from Linux `jh7110_pwmdac.c`; PIO feeding (one stereo frame per WDATA write while the FIFO is not full, frame packing L = 15:0, R = 31:16) adapted from vendor `starfive_pwmdac.c` / `starfive_pwmdac_pcm.c`. Clock, reset and pinmux offsets are facts taken from the files listed below. The timer engine, FIFO probe, buffer handling and statistics are new Beacon code. |
| `Pwmdac.h` | MIT (Beacon) | New. |
| `multi.cpp` | MIT (Beacon) | New hmulti glue; structure follows Haiku's `null_audio` / `hda` drivers (MIT, Haiku Inc.). |
| `kernel_interface.cpp` | MIT (Beacon) | New device manager / FDT glue, same pattern as Beacon's dwmac / dw_mmc drivers and Haiku's `virtio_sound` (MIT). |
| `Jamfile`, `README.md`, `THIRD_PARTY.md` | MIT (Beacon) | Build files and docs. |

The add-on as a whole is **GPL-2.0-only** (the Linux and vendor sources are
GPL-2.0 without "or later"). It is a separate kernel add-on and is not linked
into the core kernel. `src/bin/hmulti_tone` (MIT, Beacon, new) is a separate
program and contains no third-party code.

## Sources

Local copies with SHA256SUMS and SOURCE.txt: `~/beacon/refs/audio-2026-11-20`
(screened for hidden instructions on 2026-11-20: none found).

### Linux v6.12 (tag v6.12, github.com/torvalds/linux)

| Path | SPDX | Used for |
|------|------|----------|
| sound/soc/starfive/jh7110_pwmdac.c | GPL-2.0 | Register layout (WDATA 0x00, CTRL 0x04 bits), `jh7110_pwmdac_set()` order, `init_params()` defaults (8-bit, centre duty, N = 1, MSB inverted), `hw_params()` rate table, `crg_enable()` order, trigger stop = clear ENABLE |
| drivers/clk/starfive/clk-starfive-jh7110-sys.c, clk-starfive-jh71x0.h | GPL-2.0 | Facts: clock register = index x 4, enable bit 31, divider bits 23:0, `audio_root` (max 8, from PLL2), `pwmdac_core` (gate + div max 256), `pwmdac_apb` (gate) |
| drivers/reset/starfive/reset-starfive-jh7110.c, reset-starfive-jh71x0.c | GPL-2.0(+) | Facts: SYS reset assert 0x2f8 / status 0x308, 1 = deasserted, 1 ms poll |
| include/dt-bindings/clock/starfive,jh7110-crg.h, include/dt-bindings/reset/starfive,jh7110-crg.h | GPL-2.0 OR MIT | Facts: PWMDAC_APB 157, PWMDAC_CORE 158, AUDIO_ROOT 16, APB0 12, SYSRST_PWMDAC_APB 96 |
| drivers/pinctrl/starfive/pinctrl-starfive-jh7110.c, -sys.c | GPL-2.0 | Facts: DOEN 0x000 / DOUT 0x040, 8 bits per GPIO, masks 0x3f / 0x7f |
| arch/riscv/boot/dts/starfive/jh7110-pinfunc.h, jh7110-common.dtsi, jh7110.dtsi | GPL-2.0 OR MIT | Facts: GPIO33/34 = GPOUT_SYS_PWMDAC_LEFT/RIGHT (28/29), node at 0x100b0000, no interrupt, DMA channel 22 |

### StarFive vendor kernel (github.com/starfive-tech/linux, branch JH7110_VisionFive2_devel)

| Path | SPDX | Used for |
|------|------|----------|
| sound/soc/starfive/pwmdac.h | GPL-2.0 | STATE register at 0x08, FIFO full flag |
| sound/soc/starfive/starfive_pwmdac.c | GPL-2.0 | `get_pwmdac_fifo_state()`, PIO thread idea (push while not full) |
| sound/soc/starfive/starfive_pwmdac_pcm.c | GPL-2.0 | `sf_pwmdac_pcm_tx_16()` frame packing |

### Other facts

* JH7110 TRM, PWMDAC register page (doc-en.rvspace.org, read 2026-11-20):
  STATE bit 0 = FIFO full, bit 1 = FIFO empty (contradicts the vendor
  driver; the driver probes which bit it is). FIFO depth not documented.
* Vendor U-Boot (`refs/jh7110/u-boot-orangepi-2021.10-jh7110`):
  `jh7110.dtsi` pwmdac node (compatible `starfive,pwmdac`, status disabled),
  `board/starfive/visionfive2/spl.c` PLL2 = 1188 MHz.
