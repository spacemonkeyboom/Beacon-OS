<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# pwmdac_jh7110: StarFive JH7110 audio (3.5 mm jack + HDMI)

One multi_audio device with two outputs and an on-the-fly selector:

    fdt node pwmdac@100b0000 (starfive,pwmdac | starfive,jh7110-pwmdac)
      -> drivers/audio/hmulti/pwmdac_jh7110/driver_v1   (this driver)
        -> /dev/audio/hmulti/pwmdac/1                    (multi_audio API)
          -> media_addon_server: hmulti_audio.media_addon -> mixer -> apps
       engine (Engine.cpp): timer PIO feeder, buffers, output switch
         -> Headphones: PWM-DAC 0x100b0000 -> GPIO33/34 -> 3.5 mm jack
            (not on the VisionFive 2 Lite: no jack, GPIO33 is the Wi-Fi
            WL_REG_ON there, so the jack output is disabled and the pinmux
            is never touched; HDMI audio still works)
         -> HDMI: DW I2S TX0 0x120b0000 (master, S16, 2 ch) -> Inno HDMI TX
                  0x29590000 internal I2S0 input -> TV / monitor

The add-on and the published path keep their old names (`pwmdac_jh7110`,
`/dev/audio/hmulti/pwmdac/1`), so the image, `hmulti_tone` and the media
server need no changes. The device is still found via the pwmdac FDT node;
the I2S/HDMI blocks are at fixed addresses.

Status: jack confirmed on hardware (tones and MediaPlayer). HDMI path and
the switch: compile- and link-checked only (2026-11-20), **not run on
hardware yet**.

## Switching the output

* Command line: `audio_output` (prints the current output and choices),
  `audio_output hdmi`, `audio_output jack` (or `headphones`). Works while
  the media server plays; `-d <device>` picks another device.
* Media preferences: the device's "Output" group has an "Output" pop-up
  (Headphones / HDMI): a `B_MULTI_MIX_MUX` control (id 1025, values 1026
  Headphones, 1027 HDMI), the same mechanism hda uses for its selectors.
* Persistence: every switch rewrites the `output` line of
  `~/config/settings/kernel/drivers/pwmdac_jh7110` (`output headphones` or
  `output hdmi`; other lines are kept). Default: headphones. If HDMI is
  chosen but unavailable at boot, the driver falls back to headphones.
  The media add-on also saves/restores the pop-up value itself.

Switching while playing: the new sink is started (clocks, FIFO flushed,
unmuted), then the timer hook is pointed at it under the engine spinlock,
then the old sink is stopped/muted. Same buffers, same position, same
timer (its period is computed from the smaller of the two FIFOs), no app
restart. Expect a gap of at most one FIFO (~0.3 ms) and a click.

## Openers

Any number of programs may open the device. The first one that sends a
streaming ioctl (`SET_GLOBAL_FORMAT`, `GET_BUFFERS`, `BUFFER_EXCHANGE`,
`BUFFER_FORCE_STOP`) owns the stream until it closes; others get `B_BUSY`
for those ioctls but can use `GET_DESCRIPTION`, `LIST_MIX_CONTROLS`,
`GET_MIX`, `SET_MIX` and `GET_GLOBAL_FORMAT`. That is how `audio_output`
works beside the media server. `hmulti_tone` still needs the media server
stopped (it streams).

## Driver settings (`~/config/settings/kernel/drivers/pwmdac_jh7110`)

| Key | Default | Meaning |
|---|---|---|
| `output headphones\|hdmi` | headphones | selected output (written by the driver) |
| `10bit true` | off | jack: 10-bit PWM (49.5 MHz core clock, 48 kHz only; untested) |
| `period_us <n>` | from FIFOs | timer period override |
| `i2s_bclk_fs 32\|64` | 32 | HDMI: bit clock per frame. 32 = vendor kernel (bclk = rate x 16 x 2, CCR 16-cycle words); 64 = Linux mainline `jh7110_i2stx0_clk_cfg()` (CCR 32-cycle words). Try 64 if HDMI is silent or plays at the wrong pitch |
| `hdmi_tmds_khz <n>` | 148500 | TMDS clock for CTS (1080p60 from the loader's display replay) |
| `hdmi_n <n>` | 128 x fs / 1000 | force N (e.g. 6187 for the real 48.34 kHz) |

## What it does at attach

1. Logs, **before any write**: audio_root; jack clocks, reset and GPIO33/34
   mux; HDMI-side SYSCRG clocks (mclk_inner, mclk, vout_top_ahb,
   hdmitx0_mclk, i2stx0 apb/bclk_mst/lrck_mst/bclk/lrck) and resets;
   I2S COMP_PARAM_1/2 (only if the I2S APB clock already runs and its reset
   is released; otherwise right after the clock enable, still before any I2S
   write); VOUTCRG hdmi_tx_mclk/bclk/sys and the VOUT reset words (only if
   the display domain runs: vout_top_ahb on, vout_top_src out of reset);
   HDMI regs 0x05 and 0x35-0x47 (only if hdmi_tx_sys runs and the HDMI TX is
   out of reset).
2. audio_root = PLL2 / 8 = 148.5 MHz (one owner: the engine).
3. Jack: as before (clocks, reset pulse, pinmux, FIFO probe).
4. HDMI (skipped, jack only, if the display domain is not running):
   i2stx0_apb on, mclk = mclk_inner (12 -> 12.375 MHz = 256 x 48.34 kHz),
   bclk_mst on (mclk / 8 = 32 fs), lrck_mst = bclk_mst / 32, bclk/lrck muxes
   to the master clocks, hdmitx0_mclk on, I2S TX0 resets pulsed, VOUTCRG
   hdmi_tx_mclk/bclk gates on; FIFO depth from COMP_PARAM_1; I2S stopped and
   configured (TCR 16 bit, TFCR = 3/4 depth - 1, CCR); HDMI audio init as
   the vendor driver (CTS external, I2S source, MCLK 256 fs, standard I2S,
   channel map 0, audio muted). Only HDMI audio registers are written,
   DEV_MUTE by read-modify-write of bit 1. Video/PHY/PLL: never.
5. Per stream on HDMI: SAMPLE_FRE (0x2 = 48k, 0x0 = 44.1k), I2S0 enable,
   N = 6144 (48k) / 6272 (44.1k), CTS = 148500; I2S TER/IER/ITER/CER; unmute.

HDMI feeder: TXFE in ISR(0) means "FIFO at or below TFCR (3/4 depth - 1)";
each time it is seen the hook may write depth/4 frames (LTHR + RTHR per
frame). The FIFO only drains between ticks, so a left-over allowance stays
valid. The DW I2S has no TX underrun flag; the engine counts a tick gap
longer than the FIFO time as an underrun ("late > FIFO time").

Rates: 48000 (real 48339.8 Hz) and 44100 (real 44621.4 Hz) on both outputs
(integer dividers of 148.5 MHz; pitch +0.7% / +1.2%, no drift because the
media node follows the driver clock).

## Why PIO from a timer and not DMA (design choice)

The PWM-DAC has no interrupt line (none in the vendor or Linux DT, none in the
TRM register page). Linux feeds it with the dw-axi-dmac DMA engine (channel
22, burst 16); Beacon has no driver for that engine yet.

Numbers at 48 kHz stereo S16: 48,340 FIFO writes per second (193 KB/s).
With a FIFO of D frames the FIFO lasts D / 48.34 kHz, and the timer runs
every D/2 frames:

| FIFO depth D | FIFO time | timer period | ticks/s | est. cost |
|---|---|---|---|---|
| 16 (minimum implied by Linux's DMA burst of 16) | 331 us | 165 us | 6,040 | ~4% of one U74 core |
| 32 | 662 us | 331 us | 3,020 | ~2% |
| 64 | 1.3 ms | 662 us | 1,510 | ~1% |

Estimate per tick: ~2-3 us timer interrupt + SBI set_timer round trip, plus
about 0.5 us per frame (one uncached STATE read and one WDATA write over APB
at 49.5 MHz), so ~6-7 us for D = 16. One core of four, so the whole system
loses 0.25-1%. That does not "hog a core", so PIO was chosen for phase 1. The
risk is latency, not throughput: a tick delayed by more than D/2 frames
(165 us for D = 16) by a long interrupts-off section underruns (a click). The
driver counts these. If the measured depth turns out tiny (< ~10 frames) the
log says so and DMA (dw-axi-dmac, one channel, ring of the two multi_audio
buffers, ccache Flush64 on each buffer before handing it to the DMA, IRQ 73
once per buffer) is the next step.

Real numbers come from the `stats` line (below), printed every 10 s while
playing and at stop.

## Expected serial lines (prefix `jh7110_audio:`)

At boot (`<>` = unknown until run on hardware):

    jh7110_audio: pwmdac@100b0000: attaching (PWM-DAC phys 0x100b0000, DT status "disabled", attaching anyway), outputs: headphones (PWM-DAC), HDMI (I2S TX0 -> HDMI TX)
    jh7110_audio: pwmdac@100b0000: firmware left: audio_root 0x0000000?
    jh7110_audio: pwmdac@100b0000: firmware left (jack): apb0 ..., pwmdac_apb ..., pwmdac_core ..., reset assert ... status ...
    jh7110_audio: pwmdac@100b0000: firmware left (jack): GPIO32-35 dout ... doen ...
    jh7110_audio: pwmdac@100b0000: firmware left (hdmi): audio_root ... mclk_inner ... mclk ... vout_top_ahb 0x8....... hdmitx0_mclk ...
    jh7110_audio: pwmdac@100b0000: firmware left (hdmi): i2stx0_apb ... bclk_mst ... lrck_mst ... bclk ... lrck ...; SYSRST word3 assert ... status ... (i2stx0 apb/bclk = bits 5/6), word1 status ... (vout_top_src = bit 11)
    jh7110_audio: pwmdac@100b0000: i2stx0: APB clock gated or in reset: COMP_PARAM read after clock enable (before any I2S write)
    jh7110_audio: pwmdac@100b0000: firmware left (hdmi): VOUTCRG hdmi_tx_mclk ... hdmi_tx_bclk ... hdmi_tx_sys 0x8......., reset assert ... status ... (hdmi_tx = bit 9)
    jh7110_audio: pwmdac@100b0000: firmware left (hdmi): HDMI reg 0x05 DEV_MUTE 0x..; regs 0x35-0x47: .. .. (19 bytes)
    jh7110_audio: pwmdac@100b0000: audio_root divider 2 -> 8 (148500000 Hz, assuming PLL2 = 1188000000 Hz)
    jh7110_audio: pwmdac@100b0000: jack: ... / jack clocks on / pinmux / jack rate / jack FIFO probe / jack FIFO depth 17 frames ...
    jh7110_audio: pwmdac@100b0000: jack ready: rates 44100 48000, S16 stereo, FIFO depth 17 frames (full flag)
    jh7110_audio: pwmdac@100b0000: hdmi rate 48000 Hz: mclk_inner = 148500000 / 12 (256 fs), bclk = mclk / 8 (32 fs), lrck = bclk / 32 -> real 48339.843 Hz
    jh7110_audio: pwmdac@100b0000: hdmi clocks on: i2stx0_apb 0x8....... mclk ... mclk_inner 0x0000000c bclk_mst 0x80000008 lrck_mst 0x01000020 ...
    jh7110_audio: pwmdac@100b0000: i2stx0: COMP_PARAM_1 0x........ COMP_PARAM_2 0x........ (tx present, <n> channel pair(s), FIFO depth <D>, ...)
    jh7110_audio: pwmdac@100b0000: hdmi ready: I2S TX0 FIFO depth <D> frames, TXFE threshold <3D/4-1>, bclk 32 fs, AUDIO_CFG 0x81 PINS_ENA 0x.. CHANNEL_INPUT 0x00 DEV_MUTE 0x.. (audio muted until playback)
    jh7110_audio: pwmdac@100b0000: ready: output headphones (choices: headphones, hdmi), rates 44100 48000, S16 stereo
    jh7110_audio: pwmdac@100b0000: published /dev/audio/hmulti/pwmdac/1

No display: `hdmi: output unavailable: the display (VOUTCRG/HDMI TX) is not
running ...` and the choices line shows `hdmi [unavailable]`.

Playing / switching:

    jh7110_audio: ...: 2 buffers x <frames> frames at phys 0x...
    jh7110_audio: ...: hdmi audio init: fs 48000 Hz (SAMPLE_FRE 0x2), N 6144, CTS 148500 (TMDS 148500 kHz, assumed 1080p60); regs AUDIO_CFG 0x81 SAMPLE_FRE 0x02 PINS_ENA 0x04 N 00 18 00 CTS 02 44 14 DEV_MUTE 0x..
    jh7110_audio: ...: start on hdmi: 48339 Hz, 2 x <frames> frames, FIFO depth <D> frames, timer every <p> us (<n> ticks/s), latency ~<n> us
    jh7110_audio: ...: output switch: hdmi -> headphones (while playing, real rate 48339.843 Hz, switch #1)
    jh7110_audio: ...: saved "output headphones" in /boot/home/config/settings/kernel/drivers/pwmdac_jh7110
    jh7110_audio: ...: stats (hdmi): 10 s window: 483398 frames -> 48339 Hz measured, <n> ticks, hook 0.5% of one core (avg 6 us/tick), max late <n> us; total <n> ticks, underruns 0 (late > FIFO time) / 0 (empty flag); hdmi: TX overruns 0, TXFE refills <n>, last ISR 0x.., FIFO <D>/thr <t>
    jh7110_audio: ...: stop (hdmi)

What to look at for HDMI:
* `COMP_PARAM_1` / `FIFO depth`: the I2S FIFO size (Linux formula).
* `Hz measured` close to 48339: the FIFO drains, i.e. bclk/lrck run. Much
  lower or `ERROR: no buffer completed in 1 s on hdmi`: the I2S is not
  clocked (check `hdmi clocks on`), TXFE never rises.
* `TX overruns` > 0: TXFE threshold or budget wrong (writes into a full
  FIFO).
* No sound but `Hz measured` right: N/CTS or sink side; try
  `i2s_bclk_fs 64`, then `hdmi_n 6187`. The vendor code sends no Audio
  InfoFrame; some sinks need one (open point in docs/beacon/HDMI_AUDIO_PLAN.md).

## How to test

1. Boot with HDMI connected to a TV/monitor with speakers; headphones in the
   jack (low volume). Check the boot lines above (`hdmi ready`, `choices`).
2. Switch tool: `audio_output` -> `output: Headphones`, `choices:
   [Headphones], HDMI`. `audio_output hdmi` -> `output: HDMI`. Reboot and
   run `audio_output` again: still HDMI (settings file); `cat
   ~/config/settings/kernel/drivers/pwmdac_jh7110`.
3. Direct tone (no media server):

        quit application/x-vnd.Be.media-server
        audio_output hdmi
        hmulti_tone                    # 440 Hz, 3 s, both channels, 48 kHz -> TV
        hmulti_tone -c left -t 10      # left only, 10 s (stats line)
        hmulti_tone -t 20 &            # while it plays:
        audio_output jack              #   moves to the headphones
        audio_output hdmi              #   and back
        hmulti_tone -r 44100           # 44.1 kHz on HDMI (SAMPLE_FRE 0x0, N 6272)
        /system/servers/media_server &

4. Media server: `MediaPlayer /boot/system/data/sounds/beacon-test-tone.au`
   (loop it), then `audio_output jack` / `audio_output hdmi` while it
   plays, and the same from Preferences > Media > the device > Output.
   Playback must continue without restarting MediaPlayer.

Listen for: tone on the TV, correct side for `-c left`, the switch moving
the sound within a fraction of a second, no crackle (underruns 0).

## Known limitations / untested

* HDMI: everything on hardware (clock values, whether the HDMI TX takes
  bclk/ws from the CRG or from the I2S block, 32 vs 64 fs, TXFE polling with
  the interrupt unmasked but unwired, N/CTS with a 48.34 kHz source, sinks
  needing an Audio InfoFrame). CTS assumes 148.5 MHz TMDS (1080p60).
* Jack: 10-bit mode untested.
* One stream at a time; no hot-plug of HDMI or headphones; no suspend.
* No hardware volume: use the media mixer.

