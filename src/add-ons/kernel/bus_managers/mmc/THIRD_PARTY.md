<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# Third-party sources: MMC bus manager SDIO support (Beacon)

| File | License | Origin |
|------|---------|--------|
| `mmc_bus.cpp`, `mmc_bus.h`, `mmc_module.cpp`, `Jamfile` | MIT | Haiku. Beacon changes (CMD5 probe, `_TrySdio`, `ExecuteBufferCommand`, rescan guard, SDIO module entry) are MIT and marked "Beacon". |
| `sdio.cpp`, `sdio.h` | ISC | Adapted from OpenBSD `sys/dev/sdmmc/sdmmc_io.c` v1.42 and `sdmmc_cis.c` (Copyright (c) 2006 Uwe Stuehler, ISC). Beacon modifications are under the same ISC terms. |
| `sdio_module.cpp`, `README_SDIO.md`, this file | MIT | Beacon. |
| `headers/private/drivers/sdio_bus.h` | MIT | Beacon. Register numbers from the SDIO Simplified Specification. |
| `headers/private/drivers/mmc.h` | MIT | Haiku, with Beacon additions (SDIO commands, three host hooks, `mmc_buffer_transfer`). |

ISC and MIT are compatible. The add-on stays permissively licensed: **no
GPL code is in the bus manager.**

## Sources

* OpenBSD src, `sys/dev/sdmmc/` (`sdmmc_io.c` v1.42, `sdmmc_cis.c`,
  `sdmmc_ioreg.h` v1.11, `if_bwfm_sdio.c` read for the API that bwfm needs).
  Local copy: `~/beacon/refs/openbsd-bwfm/src/sys/dev/sdmmc/`.
  Taken from it: the CMD52/CMD53 argument encoding, the CMD5 OCR retry loop,
  function enable/ready and disable, interrupt enable and dispatch, block
  size write, bus width and high-speed switch, region/multi splitting into
  block and byte mode, and the CIS tuple walk (MANFID, FUNCID, VERS_1).
* Linux v6.12 `drivers/mmc/core/sdio.c`, `sdio_cis.c`, `sdio_ops.c`,
  `sdio_irq.c`, `core.c`, `include/linux/mmc/sdio.h` (GPL-2.0). Local copy:
  `~/beacon/refs/sdio-2026-11-20/linux/`. **Facts only, no code copied:**
  the probe order (I/O reset, CMD0, CMD8, CMD5), the FUNCE layouts (fn0
  block size at bytes 1-2, function max block size at bytes 12-13), the
  functions inheriting the card's MANFID, the byte-mode limit
  min(block size, 512), the R5 error bits checked, and calling a lone
  handler when INT_PENDING is already clear.
