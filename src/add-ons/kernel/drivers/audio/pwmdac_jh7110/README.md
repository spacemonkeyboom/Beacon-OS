<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# pwmdac_jh7110: StarFive JH7110 PWM-DAC (3.5 mm headphone jack)

The JH7110 PWM-DAC drives two PWM pins (GPIO33 = left, GPIO34 = right) that
the board filters into the 3.5 mm jack (Orange Pi RV, VisionFive 2). The
vendor Linux image on the Orange Pi RV registers "Starfive-PWMDAC-Sound-Card"
for it, so the jack is wired this way on our board.

    fdt node pwmdac@100b0000 (starfive,pwmdac | starfive,jh7110-pwmdac)
      -> drivers/audio/hmulti/pwmdac_jh7110/driver_v1   (this driver)
        -> /dev/audio/hmulti/pwmdac/1                    (multi_audio API)
          -> media_addon_server: hmulti_audio.media_addon -> mixer -> apps

Status: compile- and jam-build-tested for riscv64 (-Werror). **Not run on
hardware yet.**

## What it does

1. Attach: matches the FDT node, also when the node says `status =
   "disabled"` (the vendor U-Boot DT that U-Boot hands to Beacon never
   enables it; the driver logs this).
2. Logs what the firmware left in SYSCRG (apb0, audio_root, pwmdac_apb,
   pwmdac_core, reset) and in the SYS IOMUX for GPIO33/34.
3. Clocks: sets `audio_root` = PLL2 / 8 = 148.5 MHz (PLL2 assumed 1188 MHz,
   as the vendor SPL sets it), gates on `pwmdac_apb` and `pwmdac_core`,
   pulses the APB reset (SYSRST 96).
4. Pinmux: GPIO33/34 -> GPOUT_SYS_PWMDAC_LEFT/RIGHT, output enabled. Pad
   drive/bias stay as the firmware left them.
5. FIFO probe: enables the DAC at silence and writes as fast as it can
   until a STATE bit rises. That gives the FIFO depth and which bit means
   "full" (the TRM and the vendor driver disagree). If no bit rises in 1024
   writes, the driver falls back to time-paced writes with an assumed depth
   of 16.
6. Playback: a periodic kernel timer tops up the FIFO from the multi_audio
   buffers (one 32-bit write per stereo frame, left in 15:0, right in
   31:16), every depth/2 frames.

Formats: S16 stereo only. Rates: 48000 (real 48339.8 Hz, pwmdac_core =
148.5 MHz / 12) and 44100 (real 44621.4 Hz, / 13). The rates are off by
+0.71% / +1.18% because audio_root is an integer divider of PLL2 (Linux has the
same limitation with this clock tree); the media node follows the driver's
clock through played_real_time, so this is a pitch offset, not a drift.
8-bit PWM, centre-aligned, as in Linux. A 10-bit mode exists (`10bit true`
in the driver settings file `pwmdac_jh7110`; core clock 49.5 MHz, 48 kHz
only); it is untested.

No hardware volume control: use the media mixer (Media preferences / Deskbar
volume).

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

## Expected serial lines (prefix `pwmdac:`)

At boot (values in <> are what we expect; the firmware values are unknown):

    pwmdac: pwmdac@100b0000: DT status is not "okay"; attaching anyway (JH7110 boards route it to the 3.5 mm jack)
    pwmdac: pwmdac@100b0000: attaching (phys 0x100b0000, DT status "disabled", attaching anyway), 8-bit PWM
    pwmdac: pwmdac@100b0000: firmware left: apb0 0x80000000, audio_root 0x0000000?, pwmdac_apb 0x........, pwmdac_core 0x........, reset assert 0x........ status 0x........
    pwmdac: pwmdac@100b0000: firmware left: GPIO32-35 dout 0x........ doen 0x........ (GPIO33 dout <n> doen <n>, GPIO34 dout <n> doen <n>)
    pwmdac: pwmdac@100b0000: audio_root divider <n> -> 8 (148500000 Hz, assuming PLL2 = 1188000000 Hz)   (only if it differs)
    pwmdac: pwmdac@100b0000: clocks on: pwmdac_apb 0x80000000 pwmdac_core 0x8000000c, reset deasserted (status 0x...1)
    pwmdac: pwmdac@100b0000: pinmux GPIO33/34 -> PWMDAC left/right: dout ... -> ..., doen ... -> ... (pad drive/bias left as firmware set them)
    pwmdac: pwmdac@100b0000: rate 48000 Hz: pwmdac_core = 148500000 / 12, N = 1 -> real 48339.843 Hz
    pwmdac: pwmdac@100b0000: FIFO probe: state empty 0x2, after <D> writes 0x1 (<n> us); bit0 rose@<D> fell@0, bit1 rose@0 fell@1
    pwmdac: pwmdac@100b0000: FIFO depth <D> frames, full = STATE bit 0 (as in the JH7110 TRM), empty = bit 1
    pwmdac: pwmdac@100b0000: ready: rates 44100 48000, S16 stereo, FIFO <D> frames (full flag)
    pwmdac: pwmdac@100b0000: published /dev/audio/hmulti/pwmdac/1

When the media server opens it and something plays:

    pwmdac: pwmdac@100b0000: 2 buffers x <frames> frames at phys 0x...
    pwmdac: pwmdac@100b0000: start: 48339 Hz, 2 x <frames> frames, timer every <p> us (<n> ticks/s), latency ~<n> us
    pwmdac: pwmdac@100b0000: stats: 10 s window: 483398 frames -> 48339 Hz measured, <n> ticks, hook 0.4% of one core (avg 6 us/tick), max late <n> us; total <n> ticks, underruns 0 (empty flag) / 0 (whole-FIFO refills), state bits seen 0x3
    pwmdac: pwmdac@100b0000: stop

What to look at:
* `FIFO depth` and `full = STATE bit`: the key unknowns. "no FIFO-full flag
  seen" means time-paced mode (it still plays if the depth guess holds).
* `Hz measured` must be close to the real rate (48339). Much lower = the
  timer is late or the FIFO is smaller than probed; much higher = writes are
  being dropped (FIFO-full flag not working).
* `underruns` and `max late`: clicks. `hook ...% of one core`: CPU cost
  (time inside the hook only; add ~2-3 us per tick for interrupt entry).
* `ERROR: no buffer completed in 1 s (timer not firing?)`: the timer hook is
  not running.

## How to test

1. Plug headphones into the 3.5 mm jack (start with low volume: the first
   enable at boot may pop).
2. Without the media server (direct test of the driver):

        quit application/x-vnd.Be.media-server
        hmulti_tone                    # 440 Hz, 3 s, both channels, 48 kHz
        hmulti_tone -f 1000 -c left    # left only (checks L/R order)
        hmulti_tone -r 44100 -t 10     # 44.1 kHz, 10 s (shows a stats line)

   `hmulti_tone` prints the device description and, at the end, the rate the
   driver clock reported. Then restart the media server with
   `/system/servers/media_server &` (or reboot).
3. With the media server: open Preferences > Media, choose "JH7110 PWM-DAC"
   as the audio output, and play something through the media kit. The
   minimum image has no player or decoders yet; `hmulti_tone` is the test
   there. On a regular image: MediaPlayer with a WAV file.

Listen for: clean tone (no crackle = no underruns), correct side for `-c
left`, pitch about 0.7% sharp at 48 kHz (expected).

## Known limitations / untested

* Everything on hardware: clocks, reset, pinmux, FIFO probe, L/R order,
  signedness (MSB inverted, as Linux), the riscv64 userland access to the
  buffer pages (U-bit pages in kernel space, as every Haiku audio driver
  uses; the hook reads through a kernel-only alias).
* Single opener only (the media server, or hmulti_tone).
* `audio_root` is shared with the I2S/HDMI audio clocks; a future HDMI
  audio driver must agree on 148.5 MHz.
* No suspend/resume, no hot-unplug detection of the headphones.
