<!-- SPDX-License-Identifier: MIT -->
# Beacon findings: bugs and hardware quirks worth knowing

Things we learned bringing Haiku up on the Orange Pi RV (StarFive JH7110,
SiFive U74 cores, VIA VL805 USB, AP6256 / BCM43456 Wi-Fi). Most of them apply
beyond this board: to other Haiku ports and forks, to other RISC-V boards, and
to anyone using the same chips or the OpenBSD drivers. Each entry says what
we saw, why it happens, and the fix, with the file to look at. Use anything
here freely (the text is MIT; code keeps the licence named in its file).

Status: **confirmed** = tested on hardware; **pending** = written, not yet
confirmed.

## RISC-V / kernel

### Spurious page faults on the U74 (stale invalid TLB entries) — confirmed
* **Seen:** random `PANIC: page fault with interrupts disabled`, e.g. a store
  to a thread's own kernel stack. `dump_virt_page` shows the page valid,
  readable, writable, accessed and dirty.
* **Why:** the RISC-V privileged spec lets a core cache a translation that
  was invalid. Haiku does not `sfence.vma` when a PTE goes from invalid to
  valid, so a CPU can keep faulting on a page that is mapped.
* **Fix:** in the page-fault trap, walk the active page table; if it allows
  the access, flush that one entry and return (Linux does the same).
  `src/system/kernel/arch/riscv64/arch_int.cpp`, `beacon_spurious_page_fault()`.
  It logs the first 8 cases; we see a few per boot.

### X512's riscv64 fixes (from X547/haiku `riscv-milkv-titan`) — pending
Cherry-picked with authorship kept: same-team check on context switch,
SBI timer clear, i-cache coherency, misaligned-access fixes (kernel and
app_server), software update of PTE access flags, PTE update race barrier.
Left out for now: EFI loader memory-map changes (our loader boots fine),
PCI/xHCI MSI rework, UR-DP1000-only changes, the PLIC IRQ limit (we reserve
1024 vectors).

## USB

### VIA VL805 xHCI (on PCIe) — confirmed
* **Seen:** transfers stall; keyboards stop after hot-plug.
* **Why:** the VL805 does not cope with Haiku's transfer ring layout and
  misses some event interrupts.
* **Fix:** a Linux-style inline transfer ring for the VL805 and event
  matching fixes; ISP on data-stage IN TRBs.
  `src/add-ons/kernel/busses/usb/xhci.cpp` (`fVL805`, `_LinkDescriptorForPipeVL805`,
  `_VL805InitRing`, `_VL805HandleEvent`).

## SD / SDIO (Synopsys DesignWare `dw_mmc`)

### No 50 MHz SDIO without sample-phase tuning — confirmed
* **Seen:** BCM43456 firmware upload fails: long CMD53 block reads return
  I/O errors ("firmware verification failed"), small reads work.
* **Fix:** stay at 25 MHz until tuning exists.
  `src/add-ons/kernel/bus_managers/mmc/sdio.cpp`, `SdioCard::_SetupBus()`.
  Also: some dw_mmc error paths returned `B_IO_ERROR` silently; they log now
  (`busses/mmc/dw_mmc/DwMmc.cpp`).

## Wi-Fi (OpenBSD `bwfm` on Haiku's compatibility layers)

### Missing `wpa_supplicant` in small images — confirmed
Without the `wpa_supplicant` package there is no password dialog and WPA
networks cannot be joined. The `minimum` profile does not include it
(`build/jam/DefaultBuildProfiles`).

### Control replies time out: task-queue deadlock — confirmed
* **Seen:** scans and the WPA 4-way handshake work, but nearly every firmware
  command logs "timeout waiting for txctl response"; key install fails and
  the AP drops the station after ~10 s.
* **Why:** net80211 sends firmware commands from the shared task queue
  (`systq`) and sleeps there for the reply; the SDIO task that reads the
  reply was also queued on `systq`, so it ran only after the wait gave up.
* **Fix:** run the SDIO bus task on its own thread, woken by interrupts and
  sends, and serviced every 10 ms (Linux brcmfmac does the same watchdog).
  Credit: the idea comes from air/OS's `broadcomfmac` (MIT).
  `dev/sdmmc/if_bwfm_sdio.c`, `bwfm_sdio_poller()`.

### OpenBSD `bwfm_proto_bcdc_rxctl()` use-after-free — found by reading
On a reply whose length differs from the request, OpenBSD frees the pending
request but leaves it on `sc_bcdc_rxctlq`; the waiter times out and the
entry is used after free. We accept and truncate like Linux
(`brcmf_proto_bcdc_cmplt()`). `dev/ic/bwfm.c`. (Not triggered on our board
so far; worth fixing upstream.)

### Firmware roaming off
net80211 selects the BSS and runs the handshake; firmware roaming can change
the peer underneath it. We set `roam_off 1` (as air/OS does).

## Ethernet (Synopsys DesignWare EQoS `dwmac`, JH7110 gmac0)

### Link never reported up; unused gmac1 left powered; no DMA recovery — pending
* **Seen:** with `dwmac` in rv15-rv18, `/dev/net/dwmac/0: link down` once
  and never a link change; gmac1 attached as far as "DMA software reset
  stuck" and "no PHY answers on MDIO" and failed. First-boot panics in
  rv16/rv18 (both before the interface was opened) got dwmac dropped from
  rv19 on; never proven to be dwmac, and the stale-TLB and X512 MMU fixes
  (PTE A/D update, PTE race barrier) since then match those panics better.
* **Why:** (1) Haiku's ethernet module registers `ETHER_SET_LINK_STATE_SEM`
  and then only calls `ETHER_GET_LINK_STATE` when that semaphore is
  released, so a driver that reads the PHY only in `GET_LINK_STATE` never
  sees the link come up. (2) The Orange Pi RV has no PHY on gmac1 (mainline
  DT disables it; the vendor U-Boot DT enables it); a failed attach left its
  clocks open and its reset released. (3) A fatal bus error or stuck ring
  needed close/reopen; with the link down a full TX ring blocked the stack.
* **Fix:** a per-device watchdog thread (1 s): PHY poll + link semaphore,
  stall checks (DMA_DEBUG_STATUS0 RX/TX "stopped", TX OWN not cleared for
  3 s with link, fatal bus error) -> stop, DMA software reset, fresh rings;
  RX "suspended" re-arms the tail; one stats line per minute. Attach
  failure and uninit stop the DMA, assert the resets and gate the clocks the
  driver opened before the DMA memory is freed; a controller firmware left
  out of reset is held in reset before any clock is opened. TX returns
  `ENETDOWN` instead of blocking when the ring is full without link. X512's
  MIT `visionfive2` dwmac (2023) was reviewed and not used: it is a
  skeleton (`Start()` returns `B_ERROR`, no PHY setup, no cache
  maintenance, `SendData` unimplemented, RX uses the TX ring).
* **File:** `src/add-ons/kernel/drivers/network/ether/dwmac/` (README.md has
  the expected serial lines and test steps).

## Media kit

### `media_server` hang at boot: missing `libgame.so` — confirmed
* **Seen:** boot stuck at the splash (all icons lit); launch_daemon spinning;
  `QueryPort: write_port failed ... Bad port ID`.
* **Why:** `media_addon_server` needs `libgame.so`; the minimal image did not
  have it, so the add-on server never came up and media_server waited.
* **Fix:** add `libgame.so` (and `libmedia.so`) to the image.
  `build/jam/images/definitions/minimum`.

### `au_reader` sign-compare — confirmed
`au_reader.cpp` fails with `-Werror=sign-compare` on riscv64; cast fix.

## Translators

### JPEGTranslator crashes on riscv64 — open
`JPEGTranslator::Decompress` segfaults (Tracker dies when the desktop
background is a JPEG). We ship BMP wallpapers until it is fixed.

## Audio

* JH7110 PWM-DAC (3.5 mm) and I2S TX0 → HDMI audio, one device with an
  on-the-fly Output selector: see
  `src/add-ons/kernel/drivers/audio/pwmdac_jh7110/README.md` and
  `HDMI_AUDIO_PLAN.md`. Known: timer feeder sometimes runs up to 30 ms late
  (audible pops); DMA is the planned fix.
