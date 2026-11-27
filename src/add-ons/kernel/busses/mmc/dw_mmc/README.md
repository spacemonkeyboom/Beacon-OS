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
  `status = "okay"` (or no status). SDIO-only slots attach too (Beacon SDIO,
  see below).
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

## SDIO (Beacon, 2026-11-20)

The bus manager side is described in
`src/add-ons/kernel/bus_managers/mmc/README_SDIO.md`. Host-side changes:

* Reply types: CMD5 is R4 (no CRC check), CMD52/CMD53 are R5 (R1 shape). A
  CMD52 write to CCCR ABORT (0x06) gets the STOP_ABORT flag, like Linux.
* `execute_buffer_command`: CMD53 data to or from a kernel virtual buffer.
  BLKSIZ is the SDIO block size (1-2048) and BYTCNT is size x count. The PIO
  loop handles lengths that are not a multiple of the FIFO width: the last
  FIFO word carries the tail and is zero padded on writes. There is no CMD12,
  the bus manager aborts through CCCR ABORT.
* SDIO interrupt: RINTSTS/INTMASK bit 16 (slot 0). The ISR acks it, masks it
  and releases the bus manager's semaphore. `enable_sdio_irq` unmasks it.
  RINTSTS clears before commands leave bit 16 alone.
  While no one has the host SDIO interrupt enabled, the ISR acks a raw bit
  16 anyway: it latches during 1-bit enumeration, and on the JH7110 a
  masked raw bit holds the line up. The VF2 Lite's aic8800 polls instead of
  using this interrupt (StarFive's DTs have no `cap-sdio-irq`; see
  docs/beacon/FINDINGS.md).
* `set_card_type(CARD_TYPE_SDIO)` turns off the CLKENA low-power gate: the
  card needs a running clock to signal its interrupt.
* Probe without card detect: CMD52 I/O reset, CMD0, CMD8, **CMD5** first,
  then the SD (CMD55) and MMC (CMD1) probes as before.

### Wi-Fi reset GPIO (power sequence)

`Jh7110Gpio.{h,cpp}` is a minimal SYS GPIO helper (not a GPIO driver). It
maps 0x13040000, logs pins and drives one pin as a plain output.

* **Mainline DT**: `mmc-pwrseq` -> `mmc-pwrseq-simple` -> `reset-gpios =
  <&sysgpio 62 GPIO_ACTIVE_LOW>` (optional `post-power-on-delay-ms`). This
  runs at controller init: reset asserted 50 ms, released, then
  max(DT delay, 10 ms), then the 80 init clocks go with the next command.
  CMD5 is retried 25 times, 20 ms apart.
* **Vendor U-Boot DT** (what Haiku gets on the Orange Pi RV today: the
  VisionFive 2 DT, sdio0 described as 8-bit eMMC, no pwrseq). On the JH7110
  sdio0 only: if nothing answers CMD5, CMD55 or CMD1, the driver runs the
  same sequence on GPIO62 (active low) and probes for SDIO again. A
  VisionFive 2 eMMC answers CMD1 first, so its GPIO62 is never touched. A
  VisionFive 2 *without* eMMC would get GPIO62 driven high; disable that with
  `jh7110_sdio0_reset_gpio none` in the settings file.
* At init on the JH7110 sdio0, firmware's state of GPIO62 (WL_REG_ON),
  GPIO21 (WL_HOST_WAKE, unused) and the 11 dedicated SD0 pads is logged
  ("jh7110_gpio:" lines).

The PCIe driver (PERST# on GPIO26/28) uses the same block, but different
DOUT/DOEN words, so their read-modify-writes cannot collide.

Settings file `~/config/settings/kernel/drivers/dw_mmc`:
`max_clock_khz <kHz>`, `jh7110_sdio0_reset_gpio <pin|none>`.

## Limits

* SD default and high-capacity cards at 3.3 V, 25 MHz, 4-bit. `mmc_disk`
  rejects eMMC (an `mmc_disk` limit, not a driver limit).
* No UHS, HS200 or tuning, no 1.8 V switch, no DDR.
* 16-bit host FIFO width is rejected. 32- and 64-bit widths work.

## Boot log

All lines start with `dw_mmc: mmc@16020000:`. The first 64 commands and
the first 8 transfers are logged one by one.
