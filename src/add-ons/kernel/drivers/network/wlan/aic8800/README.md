# aic8800: AICSemi AIC8800D80 Wi-Fi over SDIO

Target: StarFive VisionFive 2 Lite onboard Wi-Fi, LB-LINK BL-M8800DS2-40
module = AIC8800D80 (SDIO ids 0xc8a1:0x0082), on JH7110 sdio1
(dw_mmc @0x16020000, 4-bit, WL_REG_ON = GPIO33). Licence: GPL-2.0 (port of
the Linux aic8800 driver, see THIRD_PARTY.md).

Status (2026-11-26): milestone 1 ("fw up") **confirmed** on the VF2 Lite
(chip revisions U02 = 3 and U03/U04 = 7; the Lite has 7). Milestone 2
(scan) written, **pending** hardware test (vf2lite-15). WPA2 join
(milestone 3) is not written yet.

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

Bring-up polls fn1 0x04 (no interrupt handler yet). If the boot ROM does
not answer at all, it also sets CCCR 0x04 = 0x07 (what Linux does for
`need_func0_intr`) and retries once.

After `firmware up`, as `rwnx_platform_init` / `rwnx_open` do:

9. MM_SET_STACK_START (is_stack_start 1, vendor info BIT(5)); the reply
   says whether 5 GHz is supported.
10. RF init: MM_SET_TXPWR_IDX_LVL (txpwr_lvl_v3, the Linux driver's
    built-in userconfig defaults; the adj and ofst2x tables are off by
    default there, so they are not sent), then MM_SET_RF_CALIB
    (cal_cfg 0x0f8f/0x0f0f, alpha 0x0c34c008, BT calib off, xtal cap 0).
11. ME_CONFIG (HT MCS 0-7, 20/40 MHz, SGI, LDPC, STBC; VHT/HE off; power
    save off) and ME_CHAN_CONFIG (2.4 GHz 1-13; non-DFS 5 GHz 36-48,
    149-165 if supported; 20 dBm).
12. Card interrupt installed (sdmmc_intr_establish/enable): the handler
    drains the chip under the SDIO lock. Synchronous commands still poll
    under the same lock, so both paths are safe.
13. Interface up: MM_START (phy_cfg 0, uapsd 300, lp clock 20 ppm), then
    MM_ADD_IF (station); the reply gives the vif index.
14. net80211 SCAN state: SCANU_START_REQ (all channels, one wildcard SSID,
    or the wanted one, broadcast BSSID), answered by
    SCANU_START_CFM_ADDTIONAL. Each SCANU_RESULT_IND carries a whole
    beacon/probe response, given to `ieee80211_input` as is (like bwfm's
    scan path). SCANU_START_CFM (as an indication) ends the scan:
    `ieee80211_end_scan`. Indications are queued from the RX path to a task,
    because net80211 may send commands back.

Log lines all start with `aic8800:`. `firmware up: lmac ...` is the
milestone 1 success line; `scan done, status 0, N results` the milestone 2
one. Network names and addresses are never logged.
