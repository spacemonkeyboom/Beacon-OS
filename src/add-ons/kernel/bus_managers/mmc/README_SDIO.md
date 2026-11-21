<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright 2026, The Beacon Project. -->

# SDIO in the MMC bus manager (Beacon)

SDIO support for Haiku's MMC stack, first for the AP6256 Wi-Fi module
(Broadcom BCM43456) on the Orange Pi RV's `mmc0` (dw_mmc at 0x16010000,
PLIC 74). Status: compile- and link-tested for riscv64 with `-Werror`
(`~/beacon/tools/sdio-compile-check.sh`). It has not run on hardware yet.

## Design

SDIO is an extra card type inside `bus_managers/mmc`. It is not a separate
bus. The reasons:

* SDIO cards share CMD0/CMD3/CMD7, the bus lock, the RCA handling and the
  host interface (`mmc_bus_interface`) with SD cards. A second bus manager
  would duplicate all of that and fight over the same host.
* The scan thread has to try CMD5 before the SD memory init anyway. The
  memory init (ACMD41) loops forever on an SDIO-only card.
* Combo cards (SDIO + memory) need both on the same bus later.

The layers:

    dw_mmc (busses/mmc/dw_mmc)          host: CMD5/52/53 reply types, PIO
      | mmc_bus_interface               data to virtual buffers, any length,
      |  + execute_buffer_command       SDIO IRQ (INTMASK bit 16), Wi-Fi
      |  + set_sdio_irq_semaphore       reset GPIO, low-power clock off
      |  + enable_sdio_irq
    bus_managers/mmc                    mmc_bus.cpp: CMD5 probe in the scan
      |  sdio.cpp  (SdioCard)           sdio.cpp: OCR, CMD3, CMD7, CCCR, CIS,
      |  sdio_module.cpp                FBR, high speed, 4-bit, IRQ thread
      v
    "SDIO function N" nodes             one per function 1..7, module
      (B_DEVICE_BUS "sdio")             bus_managers/mmc/sdio_function/driver_v1
      v                                 = sdio_bus_interface
    function driver (bwfm)              headers/private/drivers/sdio_bus.h

The new `mmc_bus_interface` hooks are at the end of the struct. Hosts that do
not set them (sdhci) behave as before: the bus manager skips the CMD5 probe.

## Enumeration (scan thread, bus lock held)

1. CMD0, CMD8 (as before), then **CMD5 arg 0**. No answer: SD/MMC path as
   before.
2. CMD5 with the card's OCR limited to 3.2-3.4 V, repeated every 10 ms
   (up to 100 times) until the C (ready) bit is set.
3. `set_card_type(CARD_TYPE_SDIO)`. dw_mmc turns off the low-power clock
   gate (the card needs the clock to signal interrupts).
4. CMD3 (R6) gives the RCA. CMD7 selects the card.
5. CMD52 reads CCCR 0x00-0x1f, then the common CIS (MANFID, FUNCE, VERS_1).
6. The fn0 block size is set (FUNCE value, else 64). A **CMD53 data check**
   reads CCCR 0..7 through the DAT lines and compares it with the CMD52
   values.
7. For each function: FBR interface code, FBR CIS pointer, function CIS
   (MANFID, FUNCE max block size). A function with no MANFID takes the card's
   ids. The block size is set to min(512, max).
8. If CCCR SHS: set EHS, 50 MHz, data check. On failure: back to 25 MHz.
   Then 4-bit (unless LSC without 4BLS), data check, 1-bit again on failure.
9. Publish one node per function, with the bus lock released.

A rescan keeps the enumerated card. The card is not removable.

## Card interrupt

`install_interrupt_handler()` sets CCCR IEN (master + function bit), starts
the `sdio irq` thread (real-time display priority) and unmasks the host's SDIO
interrupt. The dw_mmc ISR acks and masks the bit, then releases a semaphore.
The thread reads CCCR INT_PENDING and calls the handlers of the pending
functions (or the only handler if the bit has already dropped, as Linux
does). Then it unmasks again. Handlers run in thread context and may use
CMD52/CMD53. If the semaphore stays quiet for 100 ms while a handler is
installed, the thread polls INT_PENDING itself. A broken PLIC route then makes
Wi-Fi slow, not dead.

## Expected serial log (Orange Pi RV, vendor U-Boot DT)

The vendor DT has no `mmc-pwrseq`, so the GPIO62 fallback runs after the
first probe finds nothing. The values in `<>` depend on the hardware.

    dw_mmc: mmc@16010000: VERID 0x5342290a ...
    jh7110_gpio: sysgpio mapped at 0x13040000
    sdio: mmc@16010000: firmware pin state for the Wi-Fi slot:
    jh7110_gpio: GPIO62 (WL_REG_ON on Orange Pi RV): DOEN <n> = <...>, DOUT <n> = <...>, input level <0|1>, PADCFG <...>
    jh7110_gpio: GPIO21 (WL_HOST_WAKE (OOB IRQ, unused)): ...
    jh7110_gpio: pad SD0_CLK (64): PADCFG <...> ... (11 lines, SD0_CLK .. SD0_STRB)
    sdio: mmc@16010000: reset GPIO62 active low, board fallback, used only if no card answers
    dw_mmc: mmc@16010000: ready, ciu 50000000 Hz, ...
    sdio: mmc@16010000: no answer to CMD5 after 1 try (Operation timed out)
    sdio: mmc@16010000: power sequence (no card answered, board fallback): GPIO62 reset asserted (LOW) for 50 ms, then released, then 10 ms
    jh7110_gpio: GPIO62 driven LOW
    jh7110_gpio: GPIO62 driven HIGH
    jh7110_gpio: GPIO62 (Wi-Fi reset, after power sequence): DOEN 0 = output, DOUT 1 = HIGH, ...
    sdio: mmc@16010000: card answers CMD5 (try <k>): OCR <0x2xff8000-ish>, <1|2|3> I/O function(s), memory absent
    mmc_bus: ...
    sdio: card answers CMD5, OCR <...>
    sdio: card OCR <...>: <n> I/O function(s), memory absent, voltage window <...>
    sdio: card ready after <k> CMD5 (arg 0x300000, R4 0x8<...>)
    sdio: mmc@16010000: SDIO card: card clock low-power gating off
    sdio: CMD3: RCA 0x<rca> (R6 ...)
    sdio: CMD7: card selected
    sdio: CCCR: rev <..> (CCCR <n>, SDIO <n>), SD rev .., IOE .., IOR .., IEN .., bus .., caps <..> SMB, speed <..>, CIS 0x10<..>
    sdio: fn0 CIS: MANFID vendor 0x02d0 device 0x4345        <- MILESTONE (expected Broadcom 0x02d0, 0x4345 family)
    sdio: fn0 CIS: FUNCE max block <n>, max speed code <..>
    sdio: fn0 CIS: VERS_1 <..> "<product strings>"
    sdio: common CIS: vendor 0x02d0 device 0x4345 "<...>"
    sdio: fn0: block size <n>
    sdio: CMD53 data check (1-bit, 400 kHz): OK, CCCR ...
    sdio: fn1 FBR: interface <..> (ext ..), CIS 0x<...>
    sdio: fn1 CIS: ... / sdio: fn1: block size <n>
    sdio: fn2 FBR: ... (BCM43456: 2 or 3 functions)
    dw_mmc: mmc@16010000: card clock 25000000 Hz ...
    dw_mmc: mmc@16010000: card clock 50000000 Hz ...
    sdio: CMD53 data check (1-bit, 50 MHz high speed): OK, ...
    dw_mmc: mmc@16010000: 4-bit data bus
    sdio: CMD53 data check (4-bit): OK, ...
    sdio: card ready: RCA 0x<rca>, vendor 0x02d0 device 0x4345, <n> function(s), 4-bit, 50000 kHz (high speed)
    sdio: published function 1: vendor 0x02d0 device 0x4345 class <..>: No error
    sdio: published function 2: ...

With the mainline DT (`mmc-pwrseq`), the power sequence runs at controller
init ("mmc-pwrseq in DT"), before the first probe, and CMD5 is retried up to
25 times, 20 ms apart.

Failure patterns:

* `no answer to CMD5 after 25 tries` after the power sequence: the chip is
  still in reset or unpowered. Check the GPIO62 dump (DOEN must be 0 =
  output, DOUT 1 = HIGH), the SD0 pad config, and the vmmc supply.
* `CMD53 data check (1-bit, 400 kHz) failed`: the data path (FIFO PIO,
  BLKSIZ/BYTCNT) is broken. CMD52 works, so it is not the card.
* The 50 MHz or 4-bit check fails: the driver falls back and says so. Set
  `max_clock_khz 25000` in `~/config/settings/kernel/drivers/dw_mmc` to
  force 25 MHz.

## Settings (`~/config/settings/kernel/drivers/dw_mmc`)

    max_clock_khz 25000            # cap the bus clock of all dw_mmc hosts
    jh7110_sdio0_reset_gpio none   # disable the GPIO62 fallback (VisionFive 2
                                   # without eMMC), or give another pin number

## Untested assumptions

See the final report of the 2026-11-20 SDIO agent, and THIRD_PARTY.md.
