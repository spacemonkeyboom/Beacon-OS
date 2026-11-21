<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# HDMI audio on the JH7110 (Orange Pi RV): plan

Status: research and plan only (2026-11-20). Nothing here is implemented.
Phase 1 (3.5 mm jack, `src/add-ons/kernel/drivers/audio/pwmdac_jh7110`) comes
first. The display is currently brought up by the EFI loader replaying
U-Boot's display state (`src/system/boot/platform/efi/arch/riscv64/arch_start.cpp`).
This plan does not touch that code; it only adds audio registers on top of
the HDMI block the loader left running.

Sources (local copies in `~/beacon/refs/audio-2026-11-20`, screened clean):
* StarFive vendor kernel `JH7110_VisionFive2_devel`:
  `drivers/gpu/drm/verisilicon/starfive_hdmi_audio.c`, `inno_hdmi.c/.h`,
  `arch/riscv/boot/dts/starfive/jh7110.dtsi`, `codecs/sf_hdmi.dtsi`,
  `sound/soc/dwc/dwc-i2s.c`.
* Linux v6.12: `sound/soc/dwc/dwc-i2s.c`, `local.h` (JH7110 support for
  `starfive,jh7110-i2stx0`), `jh7110.dtsi`, clock/reset bindings.
  Mainline v6.12 has no JH7110 HDMI driver, so the HDMI side comes from the
  vendor tree only.
* Vendor Linux boot log on this board (serial-logs/haiku-boot23-rv21-*):
  `innohdmi-starfive 29590000.hdmi: HDMI&AUDIO register done.` and
  `#1: Starfive-HDMI-Sound-Card`, so the path works in hardware.

## Signal path

    multi_audio buffers --(PIO or DMA ch 47)--> DW I2S TX "i2stx_4ch0" @ 0x120b0000
         (I2S master, standard I2S, 2 ch, mclk 256 fs, bclk 64 fs)
       --> internal I2S0 input of the Innosilicon HDMI TX @ 0x29590000
         (audio packets, N/CTS) --> HDMI sink

Vendor DT (`codecs/sf_hdmi.dtsi`): simple-audio-card dai-link, format
"i2s", CPU (`i2stx_4ch0`) is bit clock and frame master, `mclk-fs = <256>`,
codec = `&hdmi`. There are no external pins: the I2S TX0 is wired to the HDMI
TX inside the SoC.

## Blocks, registers, clocks

### 1. I2S TX0 (Synopsys DesignWare I2S), 0x120b0000, size 0x1000

Compatible: `starfive,jh7110-i2stx0` (Linux), `starfive,jh7110-i2stx-4ch0`,
`snps,designware-i2s` (vendor). DMA request 47 on dw-axi-dmac. No
`interrupts` property in either DT.

Registers (Linux `sound/soc/dwc/local.h`):

| Offset | Name | Use |
|---|---|---|
| 0x000 | IER | bit 0 IEN: block enable |
| 0x008 | ITER | 1 = transmitter enable |
| 0x00c | CER | 1 = clock enable (master mode: drives bclk/lrck) |
| 0x010 | CCR | clock config: word select cycles (bits 4:3; 0x10 = 32 cycles) / gating |
| 0x018 | TXFFR | 1 = flush TX FIFO |
| 0x020 + 0x40n | LTHR(n) | left sample of channel pair n (write) |
| 0x024 + 0x40n | RTHR(n) | right sample |
| 0x02c + 0x40n | TER(n) | bit 0 TXCHEN |
| 0x034 + 0x40n | TCR(n) | word length (2 = 16 bit, 5 = 32 bit; Linux `xfer_resolution`) |
| 0x038 + 0x40n | ISR(n) | bit 4 TXFE (FIFO empty to threshold), bit 5 TXFO (overrun) |
| 0x03c + 0x40n | IMR(n) | bits 5:4 mask TXFO/TXFE (Linux clears them on JH7110 even for DMA) |
| 0x044 + 0x40n | TOR(n) | read clears overrun |
| 0x04c + 0x40n | TFCR(n) | TX FIFO trigger level (Linux: fifo_th - 1, fifo_th = depth / 2) |
| 0x054 + 0x40n | TFF(n) | 1 = flush channel FIFO |
| 0x1c8 | TXDMA | DMA data port (Linux `I2S_TXDMA`, 32-bit L/R pairs) |
| 0x1f0 / 0x1f4 | COMP_PARAM_2 / _1 | config: FIFO depth = 2 << COMP1_FIFO_DEPTH_GLOBAL, word sizes, TX/RX present |

Linux JH7110 quirk (`dwc-i2s.c` `i2s_start()`): "I2S needs to enable IRQ to
make a handshake with DMAC on the JH7110 SoC" -> unmask TXFE/TXFO in IMR even
when DMA is used.

Clocks (SYSCRG 0x13020000, register = index x 4; bit 31 gate, 23:0 divider,
27:24 mux; `clk-starfive-jh7110-sys.c`):

| Index | Name | Kind | Offset | Value to set |
|---|---|---|---|---|
| 16 | audio_root | div (max 8) of PLL2 1188 MHz | 0x040 | 8 -> 148.5 MHz (same as the PWM-DAC driver: must agree) |
| 17 | mclk_inner | div (max 64) of audio_root | 0x044 | 12 -> 12.375 MHz (256 x 48.34 kHz) |
| 18 | mclk | mux: mclk_inner / mclk_ext | 0x048 | mclk_inner (0). The VF2 has a 12.288 MHz `mclk_ext` oscillator; the Orange Pi RV may not: check the schematic |
| 63 | vout_top_hdmitx0_mclk | gate of mclk | 0x0fc | enable |
| 161 | i2stx0_apb | gate of apb0 | 0x284 | enable |
| 162 | i2stx0_bclk_mst | gate + div (max 32) of mclk | 0x288 | div 4 -> bclk = 64 fs (Linux `jh7110_i2stx0_clk_cfg`: rate = fs x 64) |
| 164 | i2stx0_lrck_mst | mux + div (max 64) of bclk_mst(_inv) | 0x290 | div 64, parent bclk_mst |
| 165 | i2stx0_bclk | mux: bclk_mst / ext | 0x294 | bclk_mst (0) |
| 167 | i2stx0_lrck | mux: lrck_mst / ext | 0x29c | lrck_mst (0) |

Resets (SYS assert 0x2f8, status 0x308): 101 I2STX0_APB and 102 I2STX0_BCLK
-> word 3 bits 5 and 6 (assert 0x304, status 0x314).
Linux `jh7110_i2s_crg_master_init()` order: apb clock on, mclk parent =
mclk_inner, bclk_mst on, deassert resets, then mclk parent = mclk_ext (VF2).
We would stay on mclk_inner unless the board has mclk_ext.

### 2. Innosilicon HDMI TX, 0x29590000, size 0x4000, PLIC 99

Byte registers at a 4-byte stride (`hdmi_writeb(off)` = `writel(base + off * 4)`).
Audio registers (vendor `starfive_hdmi_audio.c`):

| Reg | Address | Use | Value (2 ch, 48 kHz, 1080p60) |
|---|---|---|---|
| 0x05 DEV_MUTE | +0x014 | bit 1 audio mute | 0 to play, 1 to stop |
| 0x35 AUDIO_CFG | +0x0d4 | bits 1:0 MCLK ratio (1 = 256 fs), bits 4:3 source (0 = I2S), bit 7 CTS source (1 = external) | 0x81 |
| 0x37 SAMPLE_FRE | +0x0dc | 0x0 44.1k, 0x2 48k, 0x3 32k, 0x8/0xa/0xc/0xe 88.2/96/176.4/192k | 0x2 |
| 0x38 PINS_ENA | +0x0e0 | bits 1:0 I2S format (0 standard), bits 5:2 I2S0..3 enable | 0x04 |
| 0x39 CHANNEL_INPUT | +0x0e4 | channel to I2S line map | 0x00 (ch0=I2S0, ch1=I2S1, ch2=I2S2, ch3=I2S3: each field 0) |
| 0x3f/0x40/0x41 N | +0x0fc/+0x100/+0x104 | N[19:0] (3f bits 3:0) | N = 128 x fs / 1000 = 6144 |
| 0x45/0x46/0x47 CTS | +0x114/+0x118/+0x11c | CTS[19:0] | CTS = TMDS / 1000 = 148500 (148.5 MHz) |

Init once (vendor `starfive_hdmi_audio_init()`): CTS external, I2S source,
MCLK 256 fs, standard I2S, channel map, mute. Per stream (`hw_params`): rate
code, I2S0 enable, N, CTS. Trigger: unmute / mute.

HDMI clocks (VOUTCRG 0x295c0000): `hdmi_tx_mclk` (VOUTCLK 15), `hdmi_tx_bclk`
(16), `hdmi_tx_sys` (17); reset VOUTRST 9 (`HDMI_TX_HDMI`). The loader's
display restore already leaves sys + pixel clock running and the reset
deasserted (U-Boot's sf_hdmi turned them on); mclk/bclk may be gated, because
U-Boot has no audio. Offsets must be confirmed from Linux
`clk-starfive-jh7110-vout.c` (not fetched yet; fetch into a new refs dir) or
from the snapshot dump the loader already prints (`disp:` lines, voutcrg
0x295c0000 + 0x100).

Open point: the vendor code never writes an Audio InfoFrame. Some sinks
stay silent without one; check whether the Inno block sends one by itself
(inno_hdmi.h packet registers) once audio is running.

### 3. TMDS clock

CTS depends on the TMDS clock. Today the mode is fixed: 1920x1080@60, pixel
clock 148.5 MHz (U-Boot replay). The audio driver reads the DC8200/HDMI
state rather than assuming it, or takes 148.5 MHz with a log line until the
real display driver exists and can tell it.

## Data feeding: same choice as the PWM-DAC

I2S TX0 has no interrupt line in the DT. At 48 kHz stereo a PIO feeder writes
LTHR + RTHR = 2 writes per frame = 96,680 writes/s (2x the PWM-DAC). The FIFO
depth comes from COMP_PARAM_1 (typically 16 words/channel on DW I2S); with 16
frames the same timer engine as pwmdac_jh7110 needs ~6,000 ticks/s, about
5-6% of one core. Workable, but DMA is the better long-term answer for both
outputs:

* dw-axi-dmac @ 0x16050000 (`snps,axi-dma-1.01a` family, compatible
  `starfive,jh7110-axi-dma`), 4 channels, PLIC 73, clocks STG DMA1P_AXI /
  DMA1P_AHB, resets STGRST DMA1P_AXI / AHB, handshake 47 (I2S TX0) and 22
  (PWM-DAC). Linux driver `drivers/dma/dw-axi-dmac/` (fetched, v6.12).
* One channel in cyclic mode over the multi_audio ring (2 buffers), linked
  list items in a small coherent area, IRQ per block. Non-coherent: flush
  each buffer with ccache Flush64 (0x2010200, per 64-byte line, as dw_mmc and
  dwmac do) before the DMA reads it. 1024 frames x 4 B = 64 lines per buffer.

## Steps

1. Generalise the pwmdac_jh7110 engine (timer PIO, FIFO probe, stats) into a
   shared "JH7110 audio" helper so both drivers use one tested feeder and one
   `audio_root` owner (148.5 MHz, refcounted).
2. Read-only first: a debug build that only logs I2S COMP_PARAM_1/2, the
   SYSCRG audio clocks, VOUTCRG HDMI clocks and HDMI audio registers 0x05,
   0x35-0x47 after the loader display replay. No writes. Confirms offsets.
3. New add-on `drivers/audio/hmulti/jh7110_hdmi_audio` (GPL-2.0-only: adapted
   from vendor `starfive_hdmi_audio.c` and Linux `dwc-i2s.c`): attach to the
   `i2stx_4ch0@120b0000` FDT node (vendor DT; probably status disabled, attach
   anyway as pwmdac does), map HDMI 0x29590000 read-mostly.
4. I2S bring-up: clocks per the table, resets 101/102, IER=0, TXFFR=1, TCR(0)=2
   (16-bit), TFCR(0)=depth/2-1, TER(0)=1, CCR for 32-cycle word select,
   IER=1, ITER=1, CER=1.
5. HDMI audio init (once), then per stream: SAMPLE_FRE, PINS_ENA, N=6144,
   CTS=148500; unmute on start, mute on stop. Only audio registers; never
   the video, PHY or PLL registers the loader set.
6. Feeder: timer PIO writing LTHR(0)/RTHR(0) while ISR(0).TXFE says space
   (or by FIFO level), with the same stats line; publish
   `/dev/audio/hmulti/hdmi/1`.
7. Test: `hmulti_tone -d /dev/audio/hmulti/hdmi/1` with a TV/monitor that has
   speakers; check the sink's audio info (most TVs show "PCM 48 kHz").
8. Later: dw-axi-dmac driver (shared by both audio drivers, and by SPI/UART
   later), then 44.1 kHz (needs a different audio_root/mclk split or
   mclk_ext), multichannel (I2S1-3 lines), and an Audio InfoFrame if sinks
   need it. When the real DC8200/HDMI display driver replaces the loader
   replay, it takes over the TMDS rate and hot-plug, and tells audio.

## Risks

* `audio_root` is shared: pwmdac, I2S, mclk all hang off it. One owner.
* 48 kHz is really 48.34 kHz with mclk_inner (148.5 MHz / 12 / 256); HDMI
  sinks lock to N/CTS. With CTS external (measured by the HDMI block) this
  should be fine; if a sink drops out, set N for the real rate
  (N = 128 x 48340 / 1000 = 6187) or use mclk_ext if present.
* The HDMI block's state comes from U-Boot's video setup replay. Writing
  audio registers must not disturb it (DEV_MUTE 0x05 also holds video mute
  bits: use read-modify-write, as the vendor `hdmi_modb()` does).
