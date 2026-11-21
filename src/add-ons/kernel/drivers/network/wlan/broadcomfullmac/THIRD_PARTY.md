# Third-party code and data in broadcomfullmac

## OpenBSD bwfm(4) (ISC licence)

Fetched from the OpenBSD source mirror https://github.com/openbsd/src.git,
commit `5e59bc2ab7139115734c8216da419175d1f2be54` (2026-11-20, master). Each
local copy was verified against the git blob of that commit
(`git hash-object` == `git rev-parse HEAD:<path>`) before it was modified.

| Local file | Upstream file | Upstream RCS id | Blob | Changed |
|---|---|---|---|---|
| dev/ic/bwfm.c | sys/dev/ic/bwfm.c | bwfm.c,v 1.118 2026/10/20 | 66f0b5fd9b25 | yes, `#ifdef __HAIKU__` |
| dev/ic/bwfmvar.h | sys/dev/ic/bwfmvar.h | bwfmvar.h,v 1.32 2026/10/20 | 0dcf0ee3893a | yes, `#ifdef __HAIKU__` |
| dev/ic/bwfmreg.h | sys/dev/ic/bwfmreg.h | bwfmreg.h,v 1.28 2026/10/20 | 0fcc2cf4a6f5 | no |
| dev/sdmmc/if_bwfm_sdio.c | sys/dev/sdmmc/if_bwfm_sdio.c | if_bwfm_sdio.c,v 1.45 2026/10/20 | acb42b7dd12f | yes, `#ifdef __HAIKU__` + Haiku attach at the end |
| dev/sdmmc/if_bwfm_sdio.h | sys/dev/sdmmc/if_bwfm_sdio.h | if_bwfm_sdio.h,v 1.3 2020/03/06 | 4cd3e63a002c | no |
| dev/sdmmc/sdmmcdevs.h | sys/dev/sdmmc/sdmmcdevs.h | (generated from sdmmcdevs) | see tree | no |

Copyright (c) 2010-2016 Broadcom Corporation, Copyright (c) 2016,2017
Patrick Wildt; ISC licence, kept verbatim at the top of each file:

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

All Haiku changes are inside `#ifdef __HAIKU__` / `#ifndef __HAIKU__`, so
`diff -u` against the upstream files shows exactly what changed.

## Beacon code (MIT)

Copyright 2026, The Beacon Project, MIT licence:

- `glue.c` (modelled on Haiku's iaxwifi200 glue, MIT, Haiku, Inc.)
- `bwfm_haiku.h` (small OpenBSD API shims)
- `dev/sdmmc/sdmmcvar.h`, `dev/sdmmc/sdmmc_io_haiku.c` (OpenBSD sdmmc(4)
  API subset on the Beacon SDIO bus; new code, OpenBSD names)
- `src/libs/compat/freebsd_network/sdio.cpp`,
  `src/libs/compat/freebsd_network/compat/dev/sdio/haiku_sdio.h`, and the
  small BUS_sdio additions to `device.h`, `driver.c`, `haiku-module.h` and
  the freebsd_network `Jamfile` (Haiku files, MIT).

`headers/private/drivers/sdio_bus.h` is an identical copy of the SDIO bus
agent's header (~/beacon/shared/sdio_bus.h, MIT); drop it when the SDIO
branch is merged.

## Firmware (binary, not open source)

`data/system/data/firmware/broadcomfullmac/brcmfmac43456-sdio-beacon-1.tgz`
contains unmodified files; full provenance, hashes and licences are in
`FIRMWARE-SOURCES.txt` inside the archive and in README.md here:

| File | Source | Licence |
|---|---|---|
| brcmfmac43456-sdio.bin | RPi-Distro/firmware-nonfree 5d2295b633d9, debian/added-firmware/brcm/ | Synaptics binary redistribution EULA (LICENCE.synaptics, from that repo's debian/copyright) |
| brcmfmac43456-sdio.clm_blob | same | same |
| brcmfmac43456-sdio.txt (AP6256 NVRAM V1.1) | orangepi-xunlong/firmware dc163b0ed2e0, brcm/ | **unclear**: no licence stated anywhere |
