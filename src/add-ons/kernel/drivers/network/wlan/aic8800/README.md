# aic8800: AICSemi AIC8800D80 Wi-Fi over SDIO

Target: StarFive VisionFive 2 Lite onboard Wi-Fi, LB-LINK BL-M8800DS2-40
module = AIC8800D80 (SDIO ids 0xc8a1:0x0082), on JH7110 sdio1
(dw_mmc @0x16020000, 4-bit, WL_REG_ON = GPIO33). Licence: GPL-2.0 (port of
the Linux aic8800 driver, see THIRD_PARTY.md).

Status (2026-11-26): **milestone 1 ("fw up") written, compiles, not yet run
on hardware.** Scan (milestone 2) and WPA2 join (milestone 3) are not
written yet.

## Firmware (not shipped)

Copy the AIC firmware to `/boot/system/non-packaged/data/firmware/aic8800/`:
`fmacfw_8800d80_u02.bin` (or `fmacfw_8800d80_h_u02.bin` for the H variant).
Without it the driver logs where it looked and does not attach. Beacon never
puts these files in the repo or in public images (no licence).

## How it works

```
 net_server / Network prefs (/dev/net/aic8800/0)
 FreeBSD compat + OpenBSD net80211 (openbsd_wlan.a)
 if_aic_sdio.c   chip bring-up, LMAC message transport, net80211 glue
 aic_sdmmc_io.c  = broadcomfullmac's sdmmc_io_haiku.c (OpenBSD sdmmc API shim)
 freebsd_sdio.a  -> MMC bus manager SDIO function node -> dw_mmc
```

Bring-up, as the Linux BSP does it for the D80:

1. Function 1: block size 512, enable; fn0 vendor regs 0xF2=0x7F,
   0xF0=0x40 (SDIO clock free running), 0xF8=0, 0xF1=0; byte mode off
   (fn1 0x07=1); chip interrupt enable (fn1 0x00=0x07).
2. Messages: 4-byte SDIO header (length+4, type 0x11, CRC-8 poly 0x107),
   4-byte dummy word, LMAC header (id, dest task, src task 100, param
   length), params; padded to 4, +4 tail, rounded up to 512-byte blocks.
   Before each write, fn1 0x03 must report enough free 1536-byte buffers.
   Written to fn1 FIFO 0x10 (fixed address CMD53).
3. Replies: fn1 0x04 = number of 512-byte blocks waiting (120 = byte mode,
   length in 0x05 x4; bit 7 = "other" interrupt, cleared in fn1 0x01).
   Read from FIFO 0x0F. Each config frame: 4-byte header, then
   `ipc_e2a_msg` (id, dest, src, param_len, pattern, params).
4. DBG_MEM_READ 0x40500000: bits 21:16 = revision (must be U02 = 3).
5. Upload `fmacfw_8800d80_u02.bin` to 0x120000 in 1 KB DBG_MEM_BLOCK_WRITEs.
6. Patch config (magic "PTCH"/"HCTP", pair table, block sizes 0).
7. DBG_START_APP 0x120000, HOST_START_APP_AUTO.
8. MM_GET_MAC_ADDR, MM_RESET, MM_VERSION.

Milestone 1 polls fn1 0x04 instead of using the SDIO card interrupt. If the
boot ROM does not answer at all, it also sets CCCR 0x04 = 0x07 (what Linux
does for `need_func0_intr`) and retries once.

Log lines all start with `aic8800:`; `firmware up: lmac ...` is the
milestone 1 success line.
