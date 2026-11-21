<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# Third-party sources: JH7110 audio driver (pwmdac_jh7110: jack + HDMI)

## License of this add-on

| File | License | Why |
|------|---------|-----|
| `PwmdacRegs.h` | GPL-2.0-only | PWMDAC register map, CTRL bit fields and default configuration from Linux `jh7110_pwmdac.c` (GPL-2.0) and the StarFive vendor `pwmdac.h` (GPL-2.0); STATE register notes from the JH7110 TRM. |
| `Pwmdac.cpp` | GPL-2.0-only | Jack sink. CTRL programming order, sample-rate table (N per rate, core clock = 256 x fs) and clock/reset bring-up order adapted from Linux `jh7110_pwmdac.c`; PIO push (one stereo frame per WDATA write while the FIFO is not full, frame packing L = 15:0, R = 31:16) adapted from vendor `starfive_pwmdac.c` / `starfive_pwmdac_pcm.c`. FIFO probe and pacing are Beacon code. |
| `I2sHdmiRegs.h` | GPL-2.0-only | DW I2S register offsets, ISR bits, COMP_PARAM_1 fields, S16 CCR/TCR values from Linux `sound/soc/dwc/local.h` + `dwc-i2s.c` (GPL-2.0, ST Microelectronics / Rajeev Kumar); HDMI audio register numbers and bit fields from vendor `starfive_hdmi_audio.c` (GPL-2.0, StarFive, Xingyu Wu). |
| `I2sHdmi.cpp` | GPL-2.0-only | HDMI sink. I2S config/start/stop order, FIFO depth from COMP_PARAM_1, JH7110 IMR quirk and `jh7110_i2s_crg_master_init()` clock order from Linux `dwc-i2s.c`; HDMI audio init, per-stream N/CTS/SAMPLE_FRE/PINS_ENA and mute trigger from vendor `starfive_hdmi_audio.c`; bclk = rate x width x 2 from vendor `dwc-i2s.c`. Clock/reset offsets are facts from the files below. The TXFE budget feeder and the display-domain checks are Beacon code. |
| `Jh7110Audio.h`, `Engine.cpp` | MIT (Beacon) | New: sink interface, timer engine, buffers, stream ownership, output switch, settings persistence, stats; CRG/reset helpers (register offsets are hardware facts). |
| `multi.cpp` | MIT (Beacon) | New hmulti glue incl. the Output MUX control; structure follows Haiku's `null_audio` / `hda` drivers (MIT, Haiku Inc.). |
| `kernel_interface.cpp` | MIT (Beacon) | New device manager / FDT glue, same pattern as Beacon's dwmac / dw_mmc drivers and Haiku's `virtio_sound` (MIT). |
| `Jamfile`, `README.md`, `THIRD_PARTY.md` | MIT (Beacon) | Build files and docs. |

The add-on as a whole is **GPL-2.0-only** (the Linux and vendor sources are
GPL-2.0 without "or later"). It is a separate kernel add-on and is not linked
into the core kernel. `src/bin/hmulti_tone` and `src/bin/audio_output` (MIT,
Beacon, new) are separate programs and contain no third-party code.

## Sources

Local copies with SHA256SUMS and SOURCE.txt: `~/beacon/refs/audio-2026-11-20`
and `~/beacon/refs/vout-clk-2026-11-20` (screened for hidden instructions on
2026-11-20: none found).

### Linux v6.12 (tag v6.12, github.com/torvalds/linux)

| Path | SPDX | Used for |
|------|------|----------|
| sound/soc/starfive/jh7110_pwmdac.c | GPL-2.0 | Register layout (WDATA 0x00, CTRL 0x04 bits), `jh7110_pwmdac_set()` order, `init_params()` defaults (8-bit, centre duty, N = 1, MSB inverted), `hw_params()` rate table, `crg_enable()` order, trigger stop = clear ENABLE |
| drivers/clk/starfive/clk-starfive-jh7110-sys.c, clk-starfive-jh71x0.h | GPL-2.0 | Facts: clock register = index x 4, enable bit 31, divider bits 23:0, `audio_root` (max 8, from PLL2), `pwmdac_core` (gate + div max 256), `pwmdac_apb` (gate) |
| drivers/reset/starfive/reset-starfive-jh7110.c, reset-starfive-jh71x0.c | GPL-2.0(+) | Facts: SYS reset assert 0x2f8 / status 0x308, 1 = deasserted, 1 ms poll |
| include/dt-bindings/clock/starfive,jh7110-crg.h, include/dt-bindings/reset/starfive,jh7110-crg.h | GPL-2.0 OR MIT | Facts: PWMDAC_APB 157, PWMDAC_CORE 158, AUDIO_ROOT 16, APB0 12, SYSRST_PWMDAC_APB 96 |
| drivers/pinctrl/starfive/pinctrl-starfive-jh7110.c, -sys.c | GPL-2.0 | Facts: DOEN 0x000 / DOUT 0x040, 8 bits per GPIO, masks 0x3f / 0x7f |
| sound/soc/dwc/dwc-i2s.c, local.h | GPL-2.0 | DW I2S registers, `dw_i2s_config()` / `i2s_start()` / `i2s_stop()` order, S16 = CCR 0x00 / TCR 0x02, `fifo_depth = 1 << (1 + COMP1_FIFO_DEPTH_GLOBAL)`, JH7110 IMR quirk, `jh7110_i2s_crg_master_init()` |
| drivers/clk/starfive/clk-starfive-jh7110-vout.c | GPL-2.0 | Facts: VOUTCRG hdmi_tx_mclk 15, hdmi_tx_bclk 16 (parent i2stx0_bclk), hdmi_tx_sys 17 (gates) |
| drivers/clk/starfive/clk-starfive-jh71x0.h | GPL-2.0 | Facts: enable bit 31, mux bits 27:24, divider 23:0 |
| drivers/reset/starfive/reset-starfive-jh7110.c | GPL-2.0 | Facts: VOUT reset assert 0x48 / status 0x4c; VOUTRST_HDMI_TX_HDMI 9, SYSRST I2STX0_APB 101 / BCLK 102, VOUT_TOP_SRC 43 |
| arch/riscv/boot/dts/starfive/jh7110-pinfunc.h, jh7110-common.dtsi, jh7110.dtsi | GPL-2.0 OR MIT | Facts: GPIO33/34 = GPOUT_SYS_PWMDAC_LEFT/RIGHT (28/29), node at 0x100b0000, no interrupt, DMA channel 22 |

### StarFive vendor kernel (github.com/starfive-tech/linux, branch JH7110_VisionFive2_devel)

| Path | SPDX | Used for |
|------|------|----------|
| sound/soc/starfive/pwmdac.h | GPL-2.0 | STATE register at 0x08, FIFO full flag |
| sound/soc/starfive/starfive_pwmdac.c | GPL-2.0 | `get_pwmdac_fifo_state()`, PIO thread idea (push while not full) |
| sound/soc/starfive/starfive_pwmdac_pcm.c | GPL-2.0 | `sf_pwmdac_pcm_tx_16()` frame packing |
| drivers/gpu/drm/verisilicon/starfive_hdmi_audio.c | GPL-2.0 | HDMI audio registers 0x05, 0x35-0x47, init/hw_params/trigger sequence, N = 128 fs / 1000, CTS = TMDS / 1000 |
| sound/soc/dwc/dwc-i2s.c | GPL-2.0 | `dw_i2stx_4ch0_clk_init()` clock list; bit clock = rate x data width x 2 |
| arch/riscv/boot/dts/starfive/jh7110.dtsi, codecs/sf_hdmi.dtsi | GPL-2.0 OR MIT | i2stx_4ch0@120b0000 clocks/resets, hdmi@29590000 clocks, I2S master, mclk-fs 256 |

### Other facts

* JH7110 TRM, PWMDAC register page (doc-en.rvspace.org, read 2026-11-20):
  STATE bit 0 = FIFO full, bit 1 = FIFO empty (contradicts the vendor
  driver; the driver probes which bit it is). FIFO depth not documented.
* Vendor U-Boot (`refs/jh7110/u-boot-orangepi-2021.10-jh7110`):
  `jh7110.dtsi` pwmdac node (compatible `starfive,pwmdac`, status disabled),
  `board/starfive/visionfive2/spl.c` PLL2 = 1188 MHz.
