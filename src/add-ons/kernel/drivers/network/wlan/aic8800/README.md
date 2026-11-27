# aic8800: AICSemi AIC8800D80 Wi-Fi over SDIO

Target: StarFive VisionFive 2 Lite onboard Wi-Fi, LB-LINK BL-M8800DS2-40
module = AIC8800D80 (SDIO ids 0xc8a1:0x0082), on JH7110 sdio1
(dw_mmc @0x16020000, 4-bit, WL_REG_ON = GPIO33). Licence: GPL-2.0 (port of
the Linux aic8800 driver, see THIRD_PARTY.md).

Status (2026-11-26): milestones 1 ("fw up"), 2 (scan) and 3 (WPA2-PSK
join, DHCP, data) **confirmed** on the VF2 Lite (chip revisions U02 = 3 and
U03/U04 = 7; the Lite has 7).

## Firmware (separate package)

The driver loads `fmacfw_8800d80_u02.bin` (or `fmacfw_8800d80_h_u02.bin` for
the H variant) from `/boot/system/non-packaged/data/firmware/aic8800/`, then
`/boot/system/data/firmware/aic8800/`. Without it the driver logs where it
looked and does not attach. The firmware is proprietary (AICSemi) and is
never in the repository. Like Armbian, Beacon images can carry it in a
separate package, `aic8800_firmware` (files unmodified, with NOTICE and
PROVENANCE): build with `BEACON_AIC8800_FIRMWARE_DIR=<folder with the files>`
in the environment (build/jam/packages/Aic8800Firmware); without that
variable the package is not built.

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
12. RX by polling: a thread reads fn1 0x04 every 10 ms when idle, halving
    the period (to 1 ms) while frames arrive, like Linux's sdio_irq_thread
    without MMC_CAP_SDIO_IRQ. The JH7110 dw_mmc's SDIO card interrupt is
    not usable (StarFive's own DTs leave `cap-sdio-irq` out); see
    FINDINGS.md. Synchronous commands poll under the same SDIO lock.
13. Interface up: MM_START (phy_cfg 0, uapsd 300, lp clock 20 ppm), then
    MM_ADD_IF (station); the reply gives the vif index.
14. net80211 SCAN state: SCANU_START_REQ (all channels, one wildcard SSID,
    or the wanted one, broadcast BSSID), answered by
    SCANU_START_CFM_ADDTIONAL. Each SCANU_RESULT_IND carries a whole
    beacon/probe response, given to `ieee80211_input` as is (like bwfm's
    scan path). SCANU_START_CFM (as an indication) ends the scan:
    `ieee80211_end_scan`. Indications are queued from the RX path to a task,
    because net80211 may send commands back.

    MM_CHANNEL_SURVEY_IND (one per scanned channel: busy time, noise) is
    ignored. The firmware's result count is networks; we see one result
    per frame (beacon and probe response), so both are logged.
15. Join: net80211 picks the network; in AUTH state the driver sends
    SM_CONNECT_REQ (SSID, BSSID, channel, our RSN/WPA IE, for WPA
    `CONTROL_PORT_HOST | WPA_WPA2_IN_USE`, control port ethertype 0x888e).
    The firmware authenticates and associates and answers SM_CONNECT_IND
    (status, the AP's station index); net80211 goes ASSOC -> RUN.
16. WPA2-PSK: net80211 runs the 4-way handshake itself, like bwfm (the PSK
    comes from Network prefs through Haiku's HAIKU_JOIN ioctl). Keys go to
    the firmware with MM_KEY_ADD (pairwise: AP station index; group: 0xff
    and the key id); after the pairwise key, ME_SET_CONTROL_PORT opens the
    port. Queued frames (message 4) are sent before the key goes in.
17. Data TX: 4-byte SDIO header (type 0x01, CRC-8), struct hostdesc
    (length, destination, source, ethertype, AC BE, TID 0, vif, station),
    the frame without its Ethernet header; one frame per write, only with
    more than 2 free firmware buffers.
18. Data RX: 60-byte hardware header (struct hw_rxhdr + 4 alignment bytes)
    and the decrypted 802.11 MPDU (security header still in it). Converted
    to Ethernet as Linux does (A-MSDUs split); EAPOL to net80211, the rest
    to the stack. Management frames (is_80211_mpdu) are dropped: the
    firmware handles them.
19. Leaving RUN or a join: SM_DISCONNECT_REQ. SM_DISCONNECT_IND from the
    firmware starts a new scan. A scan or connect that never finishes (a
    reply lost to a bus error) is restarted after 15 s.

Log lines all start with `aic8800:`. `firmware up: lmac ...` is the
milestone 1 success line; `scan done, status 0, ...` the milestone 2 one;
`associated`, then for WPA `pairwise key installed`, the milestone 3 ones.
Network names, addresses and keys are never logged.
