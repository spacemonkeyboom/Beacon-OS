# broadcomfullmac: Broadcom FullMAC Wi-Fi over SDIO (OpenBSD bwfm)

Target: Orange Pi RV (StarFive JH7110) onboard Wi-Fi, AP6256 module =
BCM43456 (chip 0x4345 rev 9), on SDIO host mmc0 (dw_mmc @0x16010000).
Status: **compiles and links for riscv64; never run** (it needs the Beacon
SDIO bus manager from the beacon-sdio branch).

## Architecture

```
 net_server / Network prefs (/dev/net/broadcomfullmac/0)
        |
 FreeBSD compat layer (libfreebsd_network.a) + OpenBSD net80211 (openbsd_wlan.a)
        |
 bwfm.c            OpenBSD bwfm core: chip/backplane, BCDC protocol,
                   firmware commands, net80211 glue, scan/join/WPA keys
 if_bwfm_sdio.c    OpenBSD SDIO bus glue: firmware+NVRAM upload, clocks,
                   SDPCM framing, rx/tx, card interrupt service
        |  OpenBSD sdmmc_io_* / sdmmc_intr_* calls
 sdmmc_io_haiku.c  Beacon shim: maps them onto sdio_bus_interface
        |
 freebsd_sdio.a    (src/libs/compat/freebsd_network/sdio.cpp) finds the SDIO
                   function nodes in the device manager tree at init_hardware
        |
 MMC bus manager SDIO function nodes (headers/private/drivers/sdio_bus.h)
        |
 dw_mmc host driver -> AP6256
```

Attach: the compat layer is a legacy devfs driver, so `init_hardware()` walks
the device manager tree for nodes with `B_DEVICE_BUS == "sdio"` and offers
each one to `bwfm_sdio_probe()`, which reuses OpenBSD's `bwfm_sdio_match()`:
vendor 0x02d0 (Broadcom), card ids 4324/4329/4330/4334/4335/4339/4345/4354/
4356/4359/43143/43340/43341/43362/43364/43430, at least 2 functions, attach to
function 1 only. Functions 0 and 2 are used through the same cookie.
`get_driver()` on the node gives the `sdio_bus_interface` and the cookie, so
the node must be registered with `B_KEEP_DRIVER_LOADED` (request R1 in
~/beacon/shared/sdio_bus.REQUESTS).

The OpenBSD attach runs with the card lock held, then (instead of
`config_mountroot`) `bwfm_attachhook()` runs at once: firmware load and
upload, F2 enable, interrupt install, and the firmware queries (MAC, bands).
If that fails it is retried when the interface is brought up (`bwfm_init`).

Card interrupt: the handler runs in the SDIO bus manager's "sdio irq" thread
with the card interrupt masked; the shim takes Giant and calls OpenBSD's
`bwfm_sdio_intr()`, which runs the whole service routine (`bwfm_sdio_task`)
there. TX and control frames go through the same task on the compat fast
taskqueue.

## The sdmmc shim (dev/sdmmc/sdmmcvar.h, sdmmc_io_haiku.c)

| OpenBSD call | Beacon SDIO call |
|---|---|
| sdmmc_io_read_1 / write_1 | read_byte / write_byte (CMD52) |
| sdmmc_io_read_2/4, write_2/4 | rw_extended(n bytes, SDIO_RW_INCREMENT) |
| sdmmc_io_read/write_region_1 | read / write(flags SDIO_RW_INCREMENT) |
| sdmmc_io_read/write_multi_1 | read / write(flags 0, FIFO address) |
| sdmmc_io_set_blocklen | set_block_size (+ get_card_info read-back) |
| sdmmc_io_function_enable/disable/ready | enable_function(5 s) / disable_function / CCCR IO_READY |
| sdmmc_intr_establish/enable/disable | install / remove_interrupt_handler (fn 1) |

Errors map to ETIMEDOUT/EINVAL/ENODEV/EIO; the first 32 failures are logged.
Not used by bwfm and not provided: bus width/high-speed (the bus manager
chooses them), OOB GPIO interrupt (FDT `brcm,bcm4329-fmac` interrupts; the
in-band DAT1 interrupt is used).

## Haiku changes to the OpenBSD files

All inside `#ifdef __HAIKU__`: `sc_dev` is a `device_t`; log prefix
`DEVNAME() = "bwfm"`; no cfdriver/cfattach/activate; MAC set via
`IF_LLADDR`; multicast: always all-multicast (the compat ifnet has no
OpenBSD multicast list); FreeBSD newbus probe/attach/detach and
`DRIVER_MODULE(bwfm, sdio, ...)` at the end of if_bwfm_sdio.c; small shims in
`bwfm_haiku.h` (taskq_barrier, dma_alloc, MT_CONTROL, ...).

## Firmware

Packaged by `HAIKU_WIFI_FIRMWARE_ARCHIVE` (driver Jamfile) as
`data/system/data/firmware/broadcomfullmac/brcmfmac43456-sdio-beacon-1.tgz`,
extracted into `/system/data/firmware/broadcomfullmac/` in haiku.hpkg (the
standard Haiku wlan firmware mechanism; the compat `firmware_get()` looks in
`/system/non-packaged/data/firmware/broadcomfullmac/` first, then
`/system/data/firmware/broadcomfullmac/`). bwfm asks for these names (no
board type on Haiku, so the plain names):

| File | What | Source | Licence |
|---|---|---|---|
| brcmfmac43456-sdio.bin | firmware 7.84.17.1 (r871554), 495898 bytes | RPi-Distro/firmware-nonfree 5d2295b633d9 | Synaptics EULA: redistribution in object form for use with Synaptics chips (LICENCE.synaptics) |
| brcmfmac43456-sdio.clm_blob | regulatory data, CLM 9.14.6 | same | same |
| brcmfmac43456-sdio.txt | AP6256 NVRAM V1.1 (2017), xtal 37.4 MHz | orangepi-xunlong/firmware dc163b0ed2e0 (= Armbian rkwifi/nvram_ap6256.txt) | **UNCLEAR, no licence statement**; shipped by project-owner decision for the Orange Pi RV image |

linux-firmware (a1ebb5ad7eae, 2026-11-20) has **no** BCM43456 files (only
Cypress 43455), so the .bin/.clm_blob come from Raspberry Pi OS. A newer AP6256
NVRAM V1.4 (Orange Pi `nvram_ap6256.txt`, sets `ccode=DE`) exists; V1.1 was
chosen because Orange Pi and Armbian ship it under the brcmfmac name. A user can
override any file by putting it in `/system/non-packaged/data/firmware/broadcomfullmac/`.

The driver is added to the riscv64 image through `SYSTEM_ADD_ONS_DRIVERS_NET`
in `build/jam/images/definitions/minimum` (riscv64 only; the regular profile
includes minimum).

## Expected serial lines (boot, card present)

```
bwfm: found SDIO 02d0:4345 function 1 (2 functions)    (count may be 3)
bwfm: SDIO card 02d0:4345, 2 functions, 4-bit, 50000 kHz, high speed
bwfm: address xx:xx:xx:xx:xx:xx
bwfm: firmware up, chip 4345 rev 9
```

Failures to look for:

- `broadcomfullmac: SDIO function 1 (02d0:4345): no bus interface (...)`:
  node not kept loaded (R1).
- `bwfm: SDIO <op> fn N reg 0x... failed: <error>`: CMD52/CMD53 error from the
  bus (first 32 shown).
- `bwfm: cannot enable function 1` / `function 2`: IORx never set.
- `bwfm: cannot attach chip` / `cannot find chipcommon core`: backplane reads
  wrong (check 4-byte CMD53 and the F1 SBADDR window).
- `bwfm: failed loadfirmware of file brcmfmac43456-sdio.bin` (or `.txt`):
  firmware missing from /system/data/firmware/broadcomfullmac/.
- `bwfm: firmware verification failed`: CMD53 upload/readback mismatch.
- `bwfm: can't establish interrupt`, `bwfm: could not read mac address`
  (firmware did not start or no card interrupt; the bus polls every 100 ms).
- `bwfm: firmware not started, will retry when the interface is brought up`.

## Build check

`~/beacon/tools/wlan-compile-check.sh` builds `broadcomfullmac` with jam in a
separate output directory and checks that all imports are kernel exports.

## Not done / open

- Never run; the SDIO bus side is in progress (beacon-sdio).
- No OOB (GPIO21) host-wake interrupt; in-band only.
- No power management or suspend; detach is minimal (as in iwx on Haiku).
- Multicast filtering is replaced by all-multicast.
- Board-specific firmware names (`brcmfmac43456-sdio.<board>.txt`) are not
  used because Haiku has no FDT root `compatible` here.
- BCM43456 Bluetooth (UART) is not part of this driver.
