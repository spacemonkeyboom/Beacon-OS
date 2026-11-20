<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# dw_mmc: Synopsys DesignWare MMC host controller

Kernel bus driver for the Synopsys DesignWare Mobile Storage Host
Controller (Linux "dw_mmc", Synopsys MSHC). The first target is the StarFive
JH7110 (Orange Pi RV, VisionFive 2):

| Node | Address | PLIC | On the Orange Pi RV |
|------|---------|------|---------------------|
| mmc0 / sdio0 | 0x16010000 | 74 | AP6256 SDIO Wi-Fi (eMMC on the VisionFive 2) |
| mmc1 / sdio1 | 0x16020000 | 75 | microSD slot |

The driver plugs into Haiku's MMC stack:

    fdt node (starfive,jh7110-mmc | snps,dw-mshc)
      -> busses/mmc/dw_mmc/driver_v1     (this driver, mmc_bus_interface)
        -> bus_managers/mmc               (fixed child: card scan, RCA)
          -> drivers/disk/mmc/mmc_disk    (/dev/disk/mmc/<n>/raw)

Status: build-tested only (riscv64 cross-compile with -Werror). It has not
been run on hardware yet.

## Attach

* `supports_device` takes FDT nodes with a matching compatible string and
  `status = "okay"` (or no status). It skips slots that are clearly SDIO-only:
  `mmc-pwrseq`, both `no-sd` and `no-mmc`, or a child DT node with a
  compatible string (an SDIO function such as Wi-Fi).
* `register_device` registers one node with `B_DEVICE_FIXED_CHILD` set to
  the MMC bus manager, plus DMA restrictions for `mmc_disk`'s DMAResource:
  at most 512 blocks per operation and at most 64 segments.
* `init_driver` reads from the DT `reg[0]`, `interrupts[0]`, the card clock
  (`clock-frequency`, else a single `assigned-clock-rates`, else 50 MHz),
  `max-frequency`, `fifo-depth`, `data-addr`, `bus-width` and
  `post-power-on-delay-ms`.

## Firmware assumptions

There is no clock or reset driver. U-Boot used the SD card just before
Haiku, so it is assumed to have left the controller clocked and out of
reset. On the JH7110 the driver checks that, read-only, before it touches
the controller:

* SYSCRG `sdioN_ahb` and `sdioN_sdcard` gates (bit 31). The divider is
  logged.
* SYSRST status (offset 0x308, bits 64/65).

If either check fails the driver does not attach, because an access to a
gated or reset block can stall the bus. The card clock is then programmed
only through the controller's own CLKDIV/CLKENA registers:
`card = ciu / (2 * CLKDIV)`, with 400 kHz for enumeration and 25 MHz after
it (`mmc_bus` asks for those).

## Data path: PIO through the FIFO

Data moves by PIO from the calling thread. There is no DMA, so no cache
maintenance is needed. That matters because JH7110 DMA is not cache
coherent.

* The interrupt is only a doorbell. The thread always re-reads `RINTSTS`
  and the FIFO count in `STATUS`. The ISR masks the bits that fired and
  wakes the thread.
* Every sleep is capped at 20 ms. If events keep arriving with no
  interrupt, the driver logs it and switches to polling. A broken PLIC route
  makes the driver slower, not stuck.
* Multi-block transfers send CMD12 by hand after data-over. The controller's
  auto-stop is not used.
* For CMD2/CMD9 (R2), the 136-bit reply is shifted to the SDHCI layout
  (R[127:8]), which is what `mmc_bus` and `mmc_disk` parse.

To add IDMAC later, give `DoIO` a second implementation next to
`_TransferPio`. It is called with a `DataPhase` (direction, physical vecs,
length). It would need:

* The descriptors in a physically contiguous area.
* A flush of the descriptors and TX buffers before starting (SiFive L2
  FLUSH64 at 0x2010000 + 0x200, one 64-byte line per write).
* An invalidate of the RX buffers after completion.
* `HCON.ADDR_CONFIG` (logged at boot) to choose 32- or 64-bit descriptors.

## Card detect

JH7110 SD slots route card detect to a GPIO, and there is no GPIO driver
yet. On the JH7110, and on any node with `cd-gpios`, `broken-cd` or
`non-removable`, the driver probes instead:

1. CMD0, then CMD8, then CMD55. An answer to CMD55 means an SD card.
2. If not, CMD0, then CMD1. An answer to CMD1 means MMC.

The scan semaphore is released only if something answers. This keeps
`mmc_bus` out of its endless ACMD41 loop on an empty slot or an SDIO-only
device. Hot-plug is not supported in that mode: insert the card and reboot.
On other boards, the `CDETECT` register and its interrupt are used.

## Limits

* SD default and high-capacity cards at 3.3 V, 25 MHz, 4-bit. `mmc_disk`
  rejects eMMC (an `mmc_disk` limit, not a driver limit).
* No UHS, HS200 or tuning, no 1.8 V switch, no DDR.
* 16-bit host FIFO width is rejected. 32- and 64-bit widths work.

## Boot log

All lines start with `dw_mmc: mmc@16020000:`. The first 64 commands and
the first 8 transfers are logged one by one.
