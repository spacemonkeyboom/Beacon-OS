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

### SDIO host interrupt disabled by the kernel under load — confirmed
* **Seen:** during a large transfer the link stays "up" but traffic nearly
  stops (pings 260-940 ms, SSH stalls); serial shows
  `Disabling unhandled io interrupt 74` (74 = sdio0, the Wi-Fi). Only the
  bus thread's 10 ms poll keeps the driver alive.
* **Why:** Haiku disables a non-shared vector when >99% of 10000
  interrupts go unclaimed. Our dw_mmc handler masked events in INTMASK and
  left RINTSTS for the waiting thread. On the JH7110 the line stayed up
  while a masked raw bit was set (logged: `RINTSTS 0x8` data-over, not in
  `INTMASK 0x1be84`, interrupts back to back), so it fired unclaimed.
* **Fix:** as Linux `dw_mci_interrupt()`: the handler copies the event
  bits into a software status and clears them in hardware; the driver
  reads `RINTSTS | copy` and acks both. 64 MB copied with no loss after.
  `src/add-ons/kernel/busses/mmc/dw_mmc/DwMmc.cpp` (`_Status()`, `_Ack()`).
  Unclaimed interrupts are now logged with the controller state.

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

## Images, packages and services

### OpenSSH needs a `sshd` user; first-boot keys may not appear — confirmed
* The nightly/release profiles create the `party` group naming `sshd` but
  never the user, so sshd exits with "Privilege separation user sshd does
  not exist". Add it with `AddUserToHaikuImage` (`build/jam/DefaultBuildProfiles`).
* OpenSSH's post-install key script did not run on one first boot; our
  start script (`data/beacon/beacon-sshd`) now runs `ssh-keygen -A` itself.

### gcc in the nightly image was never installed — confirmed
The nightly image only copies gcc to the Installer's optional folder
(`_packages_`). To build on the board, add `binutils gcc make` and
`gmp mpfr mpc` (the image builder requires dependencies listed) as system
packages, and put `haiku_devel` in `system packages` instead of
`_packages_` (`build/jam/OptionalPackages`; adding it twice fails with
"would need to uninstall package haiku_devel"). `haiku_devel` names
`haiku` as its base package, so it only activates in the system folder.

### mmc_disk serial trace slows large writes — confirmed
Upstream builds `mmc_disk` with `TRACE_MMC_DISK` on: two synchronous
115200-baud lines per block request. Turned off; the empty `TRACE` must
be a statement (`do { } while (0)`) or an `if/else` breaks.

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

## Web browser

### HaikuWebKit 1.10.0 + WebPositive on riscv64 — confirmed
WebPositive runs on the Orange Pi RV (C_LOOP JavaScript, no JIT/Wasm/WebGL).
Full write-up, patch and package definitions: [WEBKIT.md](WEBKIT.md). Key
points for other ports: Haiku riscv64 `MachineContext.h` mapping (`vregs
x[n-1]` = xn, then `pc`); `PlatformEnable.h` forces WebAssembly on riscv64
even with C_LOOP; plain `-I/system/...` paths are not sysroot-relative when
cross-compiling; GL/EGL must be optional without Mesa. Haiku's riscv64
build-packages snapshot is too old for WebKit's dependencies; the image
builder needs a locally generated HaikuPorts index (`RepositoryRules`) and
rejects packages whose vendor is not "Haiku Project" in that index.

## Clock wrong after every boot (no battery-backed RTC)

*Confirmed (Orange Pi RV, 2026-11-25).* Boards without a battery-backed clock
start at a wrong date, and HTTPS then fails ("SSL peer certificate ... not
OK"). Haiku's boot-time NTP job (`data/launch/user`, `update-time`) only runs
if `~/config/settings/networktime settings` exists with "synchronize at boot",
and a fresh install has no such file, so it never ran. The default time zone
was GMT.

Fix: the job now always runs `Time --boot-update`, which skips only if the user
has turned "synchronize at boot" off, and retries for about two minutes while
Wi-Fi and DNS come up (`src/preferences/time/Time.cpp`). The default time zone
is America/New_York when none is saved (`src/kits/locale/LocaleRosterData.cpp`,
`src/servers/launch/InitRealTimeClockJob.cpp`). The earlier Time crash in
`nsdispatch` did not reproduce from the command line once the clock was close.

## Kernel panic "Failed to acquire spinlock" (fault in the trap entry code)

*Pending (rv61, 2026-11-25).* The SiFive U74 does not set a page's
accessed/dirty bits in hardware: the first access to a page with them clear
faults. Kernel mappings were made with the bits clear, so the first write to a
new kernel stack page could fault inside `SVec`/`SVecU` while they push the
trap frame. The nested trap overwrites `sepc`/`scause`/`stval`/`sstatus` of the
trap being entered, the original trap is lost, and the kernel then treats a
kernel fault as a user fault (deadlock in `send_signal_to_thread`).
Fix taken from **RenkuOS** (PR #523 by atomozero, MIT): kernel mappings get the
accessed/dirty bits up front (`RISCV64VMTranslationMap::Map()`), and a fault in
the trap entry code now panics with a clear message (`arch_int.cpp`). Beacon
also drops stale TLB entries for the incoming thread's kernel stack at every
context switch (`arch_thread.cpp`).

## Boot screens noisy, desktop white at first (JH7110 display not cache coherent)

*Pending (rv61).* The JH7110 display controller reads DDR behind the L2 cache,
so pixels written through the cached memory port show up late or as noise.
The same DDR is mapped uncached at 0x4_4000_0000 (JH7110 TRM table 2-4). Fix
taken from **RenkuOS** (PR #490 by atomozero, MIT): the EFI loader hands the
kernel the frame buffer through that alias (`gFrameBufferAliasOffset`,
`arch_dtb.cpp`, `video.cpp`). Beacon's periodic ccache flusher is skipped when
the frame buffer is uncached (`arch_platform.cpp`), and the display-replay code
programs the controller with the real DDR address (`arch_start.cpp`).

## Wi-Fi (BCM43456 via bwfm) drops after idling and never reconnects

*Pending (rv61).* With fast power saving, the link dropped after the board sat
idle, and the driver logged no LINK/DEAUTH event; idle pings were 200 ms to
1 s. Fix (`broadcomfullmac/dev/ic/bwfm.c`, `dev/sdmmc/if_bwfm_sdio.c`): power
saving off (CAM), plus a link watchdog that rescans and rejoins when net80211
is in RUN but nothing has been received for 3 minutes.

## Restart hangs ("pmic_ops: cannot read pmic power register")

*Open.* `shutdown -r` stops everything, then fails to reset the board through
the PMIC, so the board hangs until power-cycled.

## HaikuPorts riscv64 fontconfig can't load

*Confirmed (2026-11-25).* `fontconfig-2.18.2-2` (riscv64) links
`libxml2.so.2`, but the riscv64 repository only has libxml2 2.15.3
(`libxml2.so.16`), so anything using fontconfig fails to load ("Missing
library") and `pkgman install fontconfig` can hang. Beacon rebuilds fontconfig
with expat, its default XML parser.

## Qt 5.15.19 on Haiku riscv64

*Confirmed (Orange Pi RV, 2026-11-25).* A Qt Widgets window and a
QNetworkAccessManager HTTPS request work. Cross-build notes, patch and
configure scripts: [QT5.md](QT5.md).

## "network_available" jobs never ran (time sync, time zone prompt)

*Confirmed fixed on the Orange Pi RV (rv63, 2026-11-25): update-time ran by
itself and set the clock.* On rv62, `launch_roster info update-time` showed the job
never launched. The launch daemon's NetworkWatcher only re-evaluates on
network-monitor messages; the Wi-Fi link came up while DHCP was still
running (`IFF_CONFIGURING`), and no later message made the network count as
available. Also, a listener registered after the network was up was never
told. Fix (`src/servers/launch/NetworkWatcher.cpp`): re-check every 5 s and
notify late listeners at once.

## NSPR 4.39 on Haiku riscv64

*Built (2026-11-25), not yet run.* HaikuPorts' NSPR patches have no riscv64
case: add riscv64 to the two Haiku headers and restrict the x86 atomics
assembly in `configure.in` to x86 (riscv64 uses the C atomics). Patch:
`beacon-nspr-4.39-riscv64.patch` (in docs/beacon/qt5). NSS cross-builds with a host `nsinstall` and `CROSS_COMPILE=1`
(no shlibsign/FIPS .chk files, as distros do).

## SSH dead after first boot: zero-filled host keys

*Confirmed cause (rv62) and fix (rv63, 2026-11-25): keys valid, SSH works
after a fresh flash.* `sshd` refused to
start ("unable to load host key: error in libcrypto"). All six key files had
their right sizes but held only zero bytes, dated 1970. They were made on the
first boot, before the clock was set; power was then cut (restart hangs on
this board, see the PMIC entry) before their data reached the disk, while BFS
had already journaled the sizes. `ssh-keygen -A` only makes missing keys, so
SSH stayed broken. Fix (`data/beacon/beacon-sshd`): test every host key with
`ssh-keygen -y`, move unreadable ones aside as `*.bad`, regenerate, `sync`.
Lesson for other first-boot writers: `sync` after writing anything vital.

## Static over the boot splash (JH7110, uncached frame buffer)

*Confirmed fixed on the Orange Pi RV (rv63, 2026-11-25).* Since we draw the
splash through the uncached DDR alias (0x4_4000_0000), U-Boot's own logo,
drawn through the cached port, left dirty L2 lines over the frame buffer.
Their later eviction wrote old pixels over the splash (static while the boot
icons light up). Fix: flush the frame buffer range (by its cached address,
through the ccache Flush64 register) before drawing the RISC-V screen,
around the splash, and before ExitBootServices
(`src/system/boot/platform/efi/video.cpp`, `arch/riscv64/arch_dtb.cpp`,
`arch/riscv64/arch_start.cpp`).

## OpenGL on Haiku riscv64: Mesa softpipe through libglvnd

*Confirmed on the Orange Pi RV (2026-11-25).* Mesa 25.3.6 (softpipe, no
LLVM) with libglvnd 1.7.0, cross-built: a BGLView window and a Qt
QOpenGLWidget both draw (GL 3.3 compatibility / 2.1). Two traps:

* **Don't render inside `BGLView::Draw()`.** With libglvnd's GL kit,
  `SwapBuffers()` hands the frame to the view as a bitmap and invalidates it;
  `BGLView::Draw()` paints that bitmap. An app that overrides `Draw()` to
  render hides every frame (white window) and re-renders on every swap.
  Render from `AttachedToWindow()`/`FrameResized()`/a timer instead.
* **qthaikuplugins' OSMesa-on-BGLView bridge read `GL_FRONT`** in
  `OSMesaSwapBuffers()` before any swap, so Qt windows were black. Read
  `GL_BACK`. Patch: `docs/beacon/qt5/beacon-qthaikuplugins-osmesa-backbuffer.patch`.

Also: libglvnd's HGL `meson.build` hard-codes a host header path (prefix it
with the sysroot); our headers have only `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP`.
Patches in `docs/beacon/qt5/`.

## QML (Qt Quick) on Haiku riscv64

*Confirmed (2026-11-25).* qtdeclarative 5.15.19 with the QML JIT disabled
(5.15 has no riscv64 JIT backend; the V4 interpreter runs it). A
QQuickWidget renders through the OpenGL path above.

## Boot time sync hung for half an hour on a lossy Wi-Fi link

*Pending (next image).* `Time --boot-update` sat asleep in one blocking call
(the NTP server-name lookup) while the Wi-Fi link was dropping most packets,
so its retry loop never ran again. Fix (`src/preferences/time/Time.cpp`):
each boot attempt runs in its own thread with a 15 s limit (a hung attempt is
abandoned), retries every 2 s at first, then every 10 s, up to 20 tries.

## Clock re-syncs on every network reconnect

*Confirmed on the Orange Pi RV (rv63, 2026-11-25).* The `update-time` job
(`on network_available`) is relaunched each time the network comes back: the
launch daemon resets the sticky event when the network goes away and
relaunches a finished job when it fires again (launch_roster log: Wi-Fi
disabled and re-enabled, job relaunched in the same second). No extra code
needed beyond the NetworkWatcher re-check above.

## Wi-Fi chip recovery without a reboot (AP6256 / BCM43456 over SDIO)

*Pending (rv65; restart path fixed in rv70, untested).* On the Orange Pi RV the Wi-Fi chip sometimes (a) stops
answering on SDIO altogether: every CMD52/CMD53, even CCCR reads, times out;
or (b) sends a stream of IF events (code 54) with its scans never finishing.
Only a power cycle helped. Recovery now does what a power cycle does:

* `dw_mmc`: new host hook `power_cycle` (mmc_bus_interface): pulse the
  reset GPIO (mmc-pwrseq-simple, GPIO 62), reset the controller, back to
  400 kHz, 1-bit.
* SDIO bus manager: `reset_card` (sdio_bus_interface): power-cycle, enumerate
  again (CMD5, CMD3, CMD7, CCCR, CIS, bus width/clock), restore CCCR
  INT_ENABLE for functions that still have handlers.
* `broadcomfullmac`: count consecutive SDIO timeouts (30 = dead) and IF
  events (200 in 10 s = wedged); a restart thread takes net80211 to INIT,
  resets the card, redoes the chip setup (split out of attach as
  `bwfm_sdio_chip_setup()`), clears `sc_initialized` and calls `bwfm_init()`,
  which reloads the firmware and rejoins. At most once a minute, gives up
  after 5 failures in a row. rv66 test: the wedge seen was a trickle (~18 IF
  events and 3-5 aborted scans a minute), far under the storm threshold, so
  rv67 also restarts after 8 aborted scans (SCAN -> INIT) in a row with the
  interface up. Heavy packet loss without errors is not yet
  detected.

**rv69 test (2026-11-26): the restart itself killed the chip.** After 8
aborted scans the restart ran. The card was power-cycled and enumerated again
("card ready: RCA 0x0001 ..."), and then nothing worked: every txctl timed
out, "could not write MAC address" repeated, and no networks showed until a
cold power-off. The serial log never shows "sdio: card reset and enumerated
again" or any "chip restart ..." result line, so the restart thread never
returned. Three bugs, all fixed in rv70:

1. **Deadlock in `SdioCard::Reset()`.** It took the bus and then
   `fHandlerLock`. The SDIO IRQ thread (`_DispatchInterrupts`) takes
   `fHandlerLock` and then the bus, and it polls INT_PENDING all the time.
   During the long re-enumeration the two threads locked each other out. The
   bus stayed held for good, so every later txctl timed out. Fix: snapshot
   the installed handlers under `fHandlerLock` *before* taking the bus.
2. **Firmware was never downloaded again.** The restart cleared only the
   net80211-level `sc_sc.sc_initialized`. The SDIO-level `sc_initialized`
   stayed set, so `bwfm_sdio_preinit()` returned at once, and the freshly
   powered chip got no firmware or NVRAM. Fix: clear it, plus the cached
   backplane window (`sc_bar0`, otherwise chip setup would skip programming
   the window on a reset chip), the save/restore and clock state, the console
   address and the TX credit window, all before chip setup runs again. On
   the second run preinit skips establishing the interrupt hook and bus
   thread again (they still exist), and goes straight to enabling the card
   interrupt. With the interface down, the firmware is now still reloaded.
3. **Power-off too short for a real restart.** The recovery pulse on
   WL_REG_ON (GPIO 62) is now 1 s low and then 200 ms before CMD5 (was
   50 ms / 10 ms), so the chip's internal supplies drain and its boot ROM is
   up. The boot-time fallback keeps the short pulse. The user saw the
   wedged module survive a ~30 s power-off and recover only after a longer
   one. The RV cannot switch the module's supply: in the Linux DT, vmmc is
   `vcc3v3-pcie`, "always on by hardware", and vqmmc is the shared
   `vcc_3v3`. So WL_REG_ON is the only software reset. The AXP15060 is left
   alone. A serial dongle can also back-feed the board through its UART
   pins during a "power-off".

A warm reset (reset button) after the deadlock left the chip with the old
firmware wedged. A cold power-off cleared it. Whether a clean warm reset
still brings the chip up is part of the rv70 test.

## Boot splash shown twice (JH7110)

*Confirmed on rv68 (one splash).* The loader drew the splash, then ExitBootServices let the
firmware power the display (VOUT) off; arch_start turns it back on, so the
screen went black for ~2 s and the splash came back: it looked like two
splashes. On riscv64 the loader now only prepares the splash images, keeps
the RISC-V screen up while it works, and draws the splash once, right after
the display is restored (video_set_splash_deferred() /
video_draw_deferred_splash()).

## Wi-Fi does not rejoin after being switched off and on

*Confirmed on rv68 (rejoined after off/on).* After the interface is closed and reopened, AUTH timed out
with no firmware events at all. Suspect: bwfm_connect() sends "join" only if
the desired SSID is set, and it can be empty after the reopen. It now falls
back to the SSID of the node net80211 chose and logs every join ("connect:
join sent (N)") so the next log shows which step fails. rv63 shows the same
bug, so it predates the chip-recovery work. A warm reboot did not clear a
chip that got no ACKs from the AP; unplugging power did.

## Boot time sync never succeeded after one hung attempt

*Confirmed on rv69 (synced on its own at connect, then the time zone tab opened).* The b46bed6f70 fix ran each NTP attempt in a
thread with a 15 s limit. Once one attempt hung in the resolver, every later
attempt in the same process hung too (the stuck thread kept the resolver's
lock), so the clock stayed at 1970 while `Time --update` from a shell synced
at once. Each boot attempt now runs as a child process (`Time --update`,
killed after 15 s), and `--update` exits 1 on failure. Run from ~ on the
board: boot-style sync done in 1.4 s.

## Network preferences crashed when Wi-Fi was switched off

*Pending (rv69).* The crash came with the network menu open. Pulse() rebuilds
the wireless network menu every few pulses, removing and re-adding items. When
the network list changed while the menu was open, BMenu kept tracking a
removed item and crashed in BMenu::_SelectItem() -> BView::Invalidate(). The
rebuild now waits until the menu is closed. On the same rv68 test the Wi-Fi
rejoined after off/on (the join fallback works).

## Qt 6.10.3 runs on Beacon (riscv64)

*Confirmed on rv69.* The Qt 6 widget test app opened its window on the
Orange Pi RV: qtbase 6.10.3 cross-built with the HaikuPorts patchset plus
three fixes (memrchr guard, PIC only on Haiku, libexecinfo lambda), and
qt6-haikuplugins with the same OSMesa GL_BACK fix as Qt 5. Notes and patches:
the Beacon qt6 tree (QT6.md), to be copied into docs/beacon/qt6/.

## Qt Quick 6: software renderer works, OpenGL context creation crashes

*Seen on rv69.* qt6quick ("Hello from Qt Quick 6") runs with
`QT_QUICK_BACKEND=software`. With the default RHI/OpenGL path it crashes in
libEGL_mesa under BGLView::Renderer::Init(), called from qt6-haikuplugins'
OSMesaCreateContextExt() <- QHaikuGLContext <- QRhiGles2::create(). This
happens with both the threaded and the basic render loop. Qt 5 Quick worked
with the same libGL/Mesa, so the suspect is the context format Qt 6
requests (version/profile, depth/stencil) or the Qt 6 plugin's context setup.
Open.

## Rust std runs on riscv64 Haiku

*Confirmed on rv69.* With a custom target spec and `-Z build-std`, nightly
Rust's std builds for riscv64 Haiku unchanged and passes a std test on the
board (threads, TLS, files, time, networking, unwinding). Details and the
target spec: docs/beacon/rust/.

## VisionFive 2 Lite: vendor U-Boot boots only from partition 3

*Confirmed on vf2lite-01/02: autoboot from mmc 0:3 on the VF2 Lite and from
mmc 1:3 on the VisionFive 2 (8 GB).* The StarFive SDK U-Boot for the
VisionFive 2 Lite (starfive-tech/u-boot JH7110_VisionFive2_devel,
include/configs/starfive-visionfive2.h) has `bootpart=3`: `sdk_boot_env`
imports `vf2_uEnv.txt` from partition 3 and runs `${boot2}`;
`distro_boot_env` imports `uEnv_Lite.txt` from partition 3 and runs
`sysboot`. Our MMC image only had partitions 1 (FAT) and 2 (BFS), so the
board would stop at the U-Boot prompt. Fix: build/jam/images/MMCImage adds a
32 MiB FAT partition 3 (fat_shell cannot format FAT12) after the OS partition holding `vf2_uEnv.txt`, whose
`boot2` loads EFI/BOOT/BOOTRISCV64.EFI from partition 1 and runs `bootefi`
with U-Boot's device tree. Boards that read partition 1 ignore it.

## VisionFive 2 Lite: vendor DT says only "starfive,jh7110s"

*Confirmed on vf2lite-01 (VF2 Lite: Beacon splash and display replay
show).* The Lite's vendor U-Boot DT (starfive_visionfive2_lite.dts) has
root `compatible = "starfive,jh7110s"` only (mainline Linux lists
`"starfive,jh7110s", "starfive,jh7110"`). The EFI loader keyed the uncached
frame buffer alias (display engine does not snoop the L2) on
`"starfive,jh7110"`, so on the Lite nothing drawn after hand-off would show.
Fix: src/system/boot/platform/efi/arch/riscv64/arch_dtb.cpp also matches
`"starfive,jh7110s"`.

## VisionFive 2 Lite: sdio0 is the microSD slot, GPIO62 is a USB switch

*Confirmed on vf2lite-02 (VF2 Lite boots from the microSD on sdio0).*
dw_mmc fell back to pulsing GPIO62 (the Orange Pi RV Wi-Fi
WL_REG_ON) on JH7110 sdio0 when the DT had no mmc-pwrseq. On the Lite sdio0
is the 4-bit microSD slot and GPIO62 drives the USB0 switch
(jh7110-starfive-visionfive-2-lite.dtsi). Fix: the fallback now applies only
when sdio0 is described as 8-bit, which is the VisionFive 2 vendor DT the
Orange Pi RV gets. File: src/add-ons/kernel/busses/mmc/dw_mmc/kernel_interface.cpp.

## VisionFive 2 Lite: kernel store fault at the RAM base (OpenSBI region)

*Seen on the VF2 Lite with the Orange Pi RV image rv69 (serial log
vf2lite-20261126-105630). Fix confirmed on vf2lite-02: no panic on the Lite
(4 GB) and the VisionFive 2 (8 GB) still reaches the desktop.* The kernel panicked right
after the SBI version lines: storeAccessFault, tval 0xffffffff00000000
(physMapBase = physical 0x40000000), in memset() clearing a fresh page.
OpenSBI v1.2 sits at 0x40000000-0x4003ffff and is PMP-protected from
S-mode, but the Lite's SDK U-Boot reports that range as
EfiBootServicesData, so the loader gave it to the kernel as free RAM. (The
Orange Pi RV U-Boot reports 0x40000000-0x40080000 as EfiReservedMemoryType.)
Fix: the riscv64 EFI loader now removes every /memreserve/ entry and
/reserved-memory region of the device tree from RAM, and always keeps the
first 2 MiB of RAM (the SBI firmware's place below the usual kernel load
address) unless the loader itself already allocated pages there. Files:
src/system/boot/platform/efi/dtb.cpp, dtb.h, arch/riscv64/arch_mmu.cpp.
The same log showed the Lite's U-Boot looking only at mmc 0:3 (see the
partition 3 entry above) and a black screen (no "starfive,jh7110" in the
vendor DT, see the jh7110s entry above).

## riscv64 loader: physical memory ranges must stay sorted

*Seen on the VF2 Lite with vf2lite-01 (serial vf2lite-20261126-113640).
Fix confirmed on vf2lite-02 on the VF2 Lite and the VisionFive 2.* After the firmware carve-out the first RAM range
was split and its tail (0x40200000-0x47f00000) appended last. The kernel
takes physical_memory_range[0] as the RAM base (physMemBase,
gVirtFromPhysOffset in arch_vm_translation_map.cpp), so it used 0x47f11000
while the physical map started at 0x40200000: every physical-map access was
off, and the first fault died in STrap -> VMAddressSpace::GetKernel()
(NULL+0x54, no address space yet). Fix: sort the physical memory and
allocated range lists after all removals, before computing the physical
map. File: src/system/boot/platform/efi/arch/riscv64/arch_mmu.cpp.

## VisionFive 2 Lite: USB is the on-chip Cadence controller, not a VL805

*Confirmed on vf2lite-05 (VF2 Lite: keyboard and mouse work).* The
VisionFive 2 and the Orange Pi RV get their USB-A
ports from a VIA VL805 on PCIe 0. The Lite has pcie0 disabled; its ports
(1x USB 3.0 + 3x USB 2.0 through a hub) sit on the JH7110's own Cadence USB 3
block (cdns3), whose USB 3 PHY is PCIe PHY 0. vf2lite-02 had no driver for it,
so usb_hid found no devices. Fix: new GPL add-on
src/add-ons/kernel/busses/usb/cdns3_jh7110 (powers the block and switches the
cdns3 dual-role logic to host, following Linux cdns3-starfive.c / drd.c and
the StarFive PHY drivers), which publishes an "xhci_platform" node; the MIT
xhci add-on (src/add-ons/kernel/busses/usb/xhci.cpp) gained a platform
attachment for such nodes, and the device manager now searches busses/usb
for device tree nodes. Two Lite-specific details: VBUS is GPIO26 (high) and
the port switch GPIO62 (low); usb_125m is PLL0/div and PLL0 is 1.25 GHz on
the JH7110S, so the divider is computed from the real PLL0 rate (10 instead
of 12). The vendor DT marks the Lite's controller dr_mode "unknown"; the
VisionFive 2 / Orange Pi RV one is "peripheral" and is left alone.

## JH7110: frame buffer RAM was never reserved once the uncached alias was on

*Seen on the VF2 Lite (vf2lite-02, green lines over the splash and desktop
on some boots, clean on others with identical display/clock logs). The
reservation fix is confirmed in the vf2lite-04 log ("frame buffer
0xfe000000-0xfe7e9000 removed from RAM") and that boot was clean, but the
user later described the lines as 1-px horizontal runs from a coloured
pixel to the right edge, which looks like display FIFO underflow rather
than RAM reuse. Since the fix, vf2lite-04 and -05 gave 2 clean Lite boots
in a row: likely fixed, keep watching (display underflow stays the fallback
theory, see the vf2lite PLAN).* With the uncached DDR alias (RenkuOS #490),
frame_buffer.physical_buffer.start is RAM address + 0x4_0000_0000, and the
loader removed that alias range from RAM instead of the real one: the log
shows "frame buffer 0x4fe000000-0x4fe7e9000 removed from RAM" while the
physical ranges still contain 0xfe000000-0xfe7e9000. The kernel could then
hand frame buffer pages to anything, so whatever landed there showed on
screen (intermittent, depends on allocation). Affects every JH7110 board
since the alias change, the Orange Pi RV and VisionFive 2 included. Fix:
src/system/boot/platform/efi/arch/riscv64/arch_mmu.cpp subtracts
gFrameBufferAliasOffset before removing the range.

## cdns3_jh7110: the xHCI never started (child node without a module)

*Seen on the VF2 Lite with vf2lite-04: host mode on, then no xhci line, and
the glue ran twice. Fix confirmed on vf2lite-05 (xHCI starts, keyboard and
mouse work). A second probe pass still tried to register the glue again
(B_NAME_IN_USE, harmless); vf2lite-06 skips nodes that are already
attached.* The glue published its
"xhci_platform" child with register_node(..., NULL module, ...), which the
device manager rejects; the failed child registration also made the glue
node get initialized again (the second power-up sequence). Fix: the glue
node now carries xhci/phys, xhci/size, xhci/irq and names
"busses/usb/xhci/platform/driver_v1" as its B_DEVICE_FIXED_CHILD; the xhci
platform module is attached only that way (its supports_device returns 0).
Also measured: PLL0 on the Lite is 1.0 GHz (not the 1.25 GHz in Linux's
DT), so usb_125m needs div 8; the driver computes it from the PLL registers.

## VisionFive 2: USB "over current" on every port after the partition-3 autoboot

**Status:** CONFIRMED on the VisionFive 2 with vf2lite-07 (2026-11-26). Autoboot log shows "Port link up" from pci enum, 3 devices connected, 0 over-current, and keyboard and mouse work. Lite check pending.

**Symptom:** on the full VisionFive 2, vf2lite-02/05/06 reported root port 2 and hub 7
ports 0-3 "in an over current state". Only hub 2 port 0 connected, and there was no
keyboard or mouse. rv69 worked on the same hardware.

**What was ruled out:**
- The loader: vf2lite-06 with the rv69 loader failed the same way.
- The source: the kernel and add-on diff to rv69 is only wallpapers and Tracker.
- dw_mmc GPIO62: logged behaviour is identical on both.

**Cause (from the serial logs):** every working VF2 boot was a hand boot. The user
waited for U-Boot's distro boot to fail and typed load/bootefi at the prompt. Distro
boot runs PCIe enumeration first ("starfive_pcie pcie@2B000000: Port link up").
Every failing boot was the partition-3 autoboot, whose boot2 jumps straight to
bootefi, so U-Boot never brings up PCIe and the VL805 behind it. Haiku's own PCIe
bring-up alone leaves the VL805 reporting false over-current on its ports.

**Fix:** boot2 in vf2_uEnv.txt (build/jam/images/MMCImage) now runs `pci enum`
before loading the EFI loader. This is our own image's script, not a firmware or
env change. On the Lite, PCIe0 has no link and the command just returns.

**Later:** find what U-Boot's enumeration does that our starfive_pcie driver doesn't
(PERST timing, a VL805 settle delay, or bridge setup), so Beacon doesn't depend on
it. Then the image also works with boot paths that skip `pci enum`.

## VisionFive 2 Lite: the on-board Wi-Fi is an AIC8800D80 on sdio1

**Status:** identified from sources; probe in vf2lite-08, waiting for a hardware test.

**The module:** LB-LINK BL-M8800DS2-40 (AIC8800D80, Wi-Fi 6, BT 5.4), fitted only on some
boards. Sources: CNX Software's review, LB-LINK's product page, and the StarFive Linux 6.12
vendor branch (JH7110_VisionFive2_6.12.y_devel, 4cecf169f3).
- **Wi-Fi:** SDIO on mmc1 / sdio1 (0x16020000), 4-bit, non-removable, max 50 MHz.
- **Bluetooth:** UART1 with RTS/CTS on GPIO 22-25.

**Power:** WL_REG_ON is GPIO33, high = on. The vendor dw_mmc-starfive.c drives it low for
10 ms, then high, then waits 10 ms. There's no pwrseq node and no 32 kHz clock in the DT.

**The vendor U-Boot DT disables sdio1**, so U-Boot never muxes its pins. On the Lite only,
dw_mmc now:
- attaches to sdio1 anyway;
- turns its SYSCRG clocks on and deasserts its AHB reset if they were left off;
- muxes CLK GPIO10, CMD GPIO9 and DATA GPIO11/12/7/8, like Linux's mmc1_pins;
- powers the module through GPIO33 before probing.

The SDIO bus manager then prints the card's CIS vendor and device IDs.

**Driver:**
- Vendor Linux aic8800 (aic8800_fdrv + aic_load_fw): about 65k lines, RivieraWaves/AICSemi
  copyright, MODULE_LICENSE("GPL"), no per-file SPDX tags. It's a cfg80211 FullMAC driver.
- Firmware comes from /lib/firmware/aic8800_sdio: fmacfw_8800d80_u02.bin,
  fw_patch_8800d80_u02.bin, fw_adid_8800d80_u02.bin, lmacfw_rf_8800d80_u02.bin,
  calibmode_8800d80.bin. Its redistribution license is still to be found.

**vf2lite-08 result (2026-11-26):** the module answers CMD5 with OCR 0x20ffff00 (2 I/O
functions, no memory). That was the only answer it gave: after the bus manager's CMD0, CMD5
timed out, the bus manager fell back to SD/MMC init, and so the CIS was never read. The
"RCA: 1 Status: 520" line in the log belongs to sdio0, the SD card.

The cause was the clock. U-Boot leaves sdio1's card clock at the reset-default divider 2,
while U-Boot sets 8 for sdio0. With divider 2 the controller runs four times faster than the
50 MHz the driver assumes, so the "400 kHz" init clock was really about 1.6 MHz and
"25 MHz" about 100 MHz. vf2lite-09 forces divider 8 on the Lite's sdio1. Linux does the
equivalent: it sets SDIO1_SDCARD to 50 MHz through assigned-clock-rates.

**Firmware license (2026-11-26):** no license from AICSemi found.
- deepin wrote on netdev on 2025-12-03 that the firmware's redistribution license "remains
  unclear to date".
- The AIC8800 SDIO FullMAC driver series posted to linux-wireless on 2026-07-23 (it covers
  AIC8801, 8800DC and 8800D80) lists "firmware redistribution permission and the
  corresponding linux-firmware submission are not yet available".
- Radxa's Debian package marks all of src/*, including the firmware blobs, as GPL-2. That is
  the packager's label; it isn't a grant from AIC, and binary-only blobs can't meet GPL terms
  anyway.

So Beacon must not ship these blobs in images for now.
## Firefox 160.0.1 on riscv64 Beacon: two portability gaps (pending, untested on hardware)

Found while cross-building Firefox (HaikuPorts recipe base) for riscv64 Beacon, 2026-11-26.
Patch: ~/beacon/firefox/ff160/notes/beacon-firefox-160.0.1-riscv64.patch (MPL-2.0).

- **xptcall:** xpcom/reflect/xptcall/md/unix/moz.build only built the riscv64 stubs for Linux.
  Haiku riscv64 needs them too, or libxul fails to link (NS_InvokeByIndex).
- **SpiderMonkey FlushICache:** the riscv64 JIT had no Haiku branch and hit #error. It now calls
  clear_caches(addr, len, B_INVALIDATE_ICACHE | B_FLUSH_DCACHE); Beacon's kernel turns that into
  fence.i locally plus an SBI remote fence.i on the other harts.
- Also: navigator.platform/oscpu said "Haiku x86_64" regardless of CPU; now reports riscv64.
- **wasm signal handlers:** js/src/wasm/WasmSignalHandlers.cpp had no Haiku riscv64 context macros.
  Beacon's riscv64 signal frame: pc = uc_mcontext.pc, vregs x[0..30] hold x1..x31, so ra = x[0],
  sp = x[1], fp(s0) = x[7]. Checked against the kernel's arch_thread.cpp signal frame (pending, untested).
- Toolchain note: Debian's clang 19 + lld 19 handle --target=riscv64-unknown-haiku with the
  Haiku sysroot; HaikuPorts' lld >= 21 requirement is not enforced by configure (libxul link
  with lld 19 still unproven).

**vf2lite-11 result (2026-11-26): chip confirmed on hardware.** With dw_mmc's own probe skipped on the
Lite's sdio1, so that the bus manager sends the chip's first CMD5, the card enumerates:
- fn0 CIS MANFID vendor 0xc8a1, device 0x0082;
- fn2 MANFID 0xc8a1 / 0x0182, FUNCE max block 2048;
- no high-speed support; 4-bit at 25 MHz; the 4-bit CMD53 data check passes.

0xc8a1 / 0x0082 is AICSemi's AIC8800D80 SDIO ID. One "data error at end, RINTSTS 0x8088"
appears on the first 1-bit CMD53, before the bus-width switch. It's harmless so far and low
priority.

## AIC8800D80 driver (aic8800), milestone 1 "fw up" (pending, not yet run on hardware)

New driver src/add-ons/kernel/drivers/network/wlan/aic8800/ (GPL-2.0), ported from the Linux
aic8800 RFC v4 (linux-wireless, 2026-08-29), on the OpenBSD net80211 glue like broadcomfullmac.
What another port needs to know about the D80's SDIO protocol (details in the driver README):
- SDIO v3 register map on fn1: 0x00 interrupt enable, 0x01 pending, 0x03 free firmware buffers
  (1536 bytes each), 0x04 blocks waiting (120 = byte mode), 0x0F read FIFO, 0x10 write FIFO.
- Every host message is padded up to whole 512-byte blocks and carries a CRC-8 (poly 0x107) of
  its first 3 header bytes; firmware replies come back aggregated in the read FIFO.
- Boot: read 0x40500000 (revision in bits 21:16, U02 = 3), upload fmacfw to 0x120000 in 1 KB
  block writes, write the patch table, DBG_START_APP 0x120000 (auto), then MM_* requests.
- The first version polls fn1 0x04 rather than using the SDIO card interrupt.
- The shared OpenBSD sdmmc shim (broadcomfullmac/dev/sdmmc/sdmmc_io_haiku.c) now takes its log
  tag from SDMMC_TAG, so a second driver can build it (aic8800/aic_sdmmc_io.c).
Firmware is never shipped (no AIC licence): it is loaded from
/boot/system/non-packaged/data/firmware/aic8800/. Local test images add it at build time from
a git-ignored UserBuildConfig only.

**vf2lite-12 result (2026-11-26):** the AIC8800D80 enumerated at boot (CCCR, CIS, 4-bit, 25 MHz all
fine), but when aic8800 attached ~10 s later every CMD52 to the card timed out (no response:
first SDIO set_blocklen, then function_enable). Nothing logged a reset in between. The dw_mmc
per-command trace had used up its 64-line budget during enumeration, which is why the controller
showed no lines for those commands (they were sent, they just weren't logged).
(The "reg 0x200" in the shim's set_blocklen error is the block length, not an address.) The
"found" line printed twice is normal: the compat layer probes once in init_hardware and again
before attach.
Fix in vf2lite-13 (pending): like the Linux BSP, which powers the chip on and rescans the host
right before it uses it, aic8800 now checks the card with a CMD52 read of CCCR 0. If the card is
silent, it calls reset_card (WL_REG_ON power cycle plus re-enumeration, CMD5 first, no CMD0) and
tries again, up to two times. dw_mmc PowerCycle() now logs the reset GPIO, the Lite sdio1 pin mux
and the controller state before cycling, and refills the command trace budget to 24, so the next
log shows why the card went quiet and how it came back.

**vf2lite-13 result (2026-11-26): root cause found. jh7110_audio was taking over WL_REG_ON (confirmed on hardware).**
- On the VF2 Lite, GPIO33 is the AIC8800D80's WL_REG_ON. The PWM-DAC audio driver muxes GPIO33/34 to
  PWMDAC left/right for the 3.5 mm jack, as on the VF2 and the Orange Pi RV.
- The log showed it: "pinmux GPIO33/34 -> PWMDAC left/right". The new PowerCycle dump then read
  "GPIO33 (SDIO reset): DOUT 28 = peripheral signal 28", which is PWMDAC_LEFT. The PWM signal on
  the enable pin switched the chip off.
- The aic8800 power cycle took the pin back and the card answered again (CCCR 0x43). So the
  recovery works.
- Fix in the audio driver (src/add-ons/kernel/drivers/audio/pwmdac_jh7110): on the Lite (root
  compatible starfive,jh7110s / visionfive-2-lite*) it no longer brings up the jack sink and never
  writes the GPIO33/34 mux. The Lite's vendor DT disables pwmdac, pwmdac_codec and sound3, so the
  board has no PWM-DAC jack. HDMI audio is unchanged, and so are the VF2 and the RV.
  Lesson for other ports: on a board-specific pin like this, follow that board's DT, not the SoC's
  common pin groups.
- Chip revision: the Lite's D80 reports chip word 0xf3078820, revision 7 (U03; AIC uses the same id
  for U04). The upstream RFC accepts U02 only. AIC's own SDIO BSP (radxa-pkg/aic8800
  aicbsp_driver_fw_init) runs U02 and U03 the same way with the *_u02 firmware, so aic8800 now
  accepts 3 and 7 (pending).

**vf2lite-14 result (2026-11-26): MILESTONE 1 CONFIRMED ON HARDWARE (AIC8800D80 firmware up).**
- pwmdac reports "VF2 Lite: no PWM-DAC jack ... pinmux untouched". HDMI audio still works (the user
  heard the startup sound).
- The card answered at 26.8 s after boot with NO power cycle, which confirms the GPIO33 / PWM-DAC
  root cause.
- Boot ROM chip word 0xf3078820 (revision 7, U03). The 327440-byte fmacfw_8800d80_u02.bin uploaded
  in 163 ms (polled, 1 KB block writes). Firmware version word 0x06090101, boot status 0.
- MM_VERSION reports lmac 6.9.1.1, features 0x01e877d7, max 32 stations. The MAC address comes from
  the chip's efuse (xx:xx).
- /dev/net/aic8800/0 appears, net_server starts it, and Network prefs shows the Wi-Fi interface.
  Link stays down because scanning is not written yet (milestone 2).

## AIC8800D80 driver (aic8800), milestone 2 "scan" (confirmed, vf2lite-15)

File: src/add-ons/kernel/drivers/network/wlan/aic8800/if_aic_sdio.c (README has the full order).
- After firmware up the LMAC needs more set-up before it scans, in the order of Linux
  rwnx_platform_init / rwnx_open: MM_SET_STACK_START (its reply carries the 5 GHz flag), the RF init
  (MM_SET_TXPWR_IDX_LVL with the driver's built-in userconfig table, then MM_SET_RF_CALIB with the
  D80 calibration words), ME_CONFIG, ME_CHAN_CONFIG, and on interface up MM_START then MM_ADD_IF
  (station). Without a vif from MM_ADD_IF, SCANU_START has no interface to scan on.
- SCANU_START_REQ is acknowledged by SCANU_START_CFM_ADDTIONAL (sic). The real end of the scan is a
  later SCANU_START_CFM that arrives as an indication, after one SCANU_RESULT_IND per beacon or
  probe response. Each result carries the whole 802.11 frame, so it can go straight into net80211's
  ieee80211_input (no fake beacon as bwfm builds).
- Indications are read in the SDIO RX path, which holds the SDIO lock. net80211 can answer them
  with a command (end of scan, then a rescan), which needs that lock too. So indications are copied
  onto a queue and handled in a task (Giant, no SDIO lock).
- RX now comes by SDIO card interrupt (sdmmc_intr_establish/enable; the bus sets CCCR IEN). The
  chip-side enable is fn1 0x00 = 0x07, which bring-up already writes. Synchronous commands still
  poll under the same lock, so a reply is never lost between the two paths.
- Network names and addresses are not logged; the success line is "scan done, status 0, N results".

**vf2lite-15 result (2026-11-26): MILESTONE 2 CONFIRMED ON HARDWARE (scan).** Stack start (5 GHz
supported), RF calibrated, station interface up (vif 0), scan on 22 channels, "scan done, status 0,
7 results (firmware 5)", then periodic rescans; Network prefs lists the networks.

### JH7110 dw_mmc: the SDIO card interrupt is not usable on the VF2 Lite (polling instead) — confirmed (polling works, vf2lite-16)
* **Seen (vf2lite-15):** with the handler installed (INTMASK bit 16, CTRL int_enable, CCCR IEN 0x03)
  one host interrupt arrived, then none: "card interrupt found by polling INT_PENDING (0x02); the
  host SDIO interrupt did not fire". Before that, while bit 16 was masked, sdio1's handler got
  unclaimed interrupts with RINTSTS 0x10000 (raw SDIO bit only). The bit had latched during the
  1-bit enumeration (RINTSTS 0x10004 right after the 1-bit CMD53 check) and was never acked.
* **PLIC source:** 75 is right. Every JH7110 DT (mainline, StarFive 6.6 and 6.12) gives mmc1
  `interrupts = <75>`, and sdio1's handler is the one being called.
* **Why polling:** StarFive's own DTs, the Lite's included, leave `cap-sdio-irq` out of mmc1 (the
  CM board has it commented out). So their Linux runs the MMC core's polling sdio_irq_thread, never
  the host interrupt. On the JH7110 a masked raw status bit also holds the line up (see the RV
  "SDIO host interrupt disabled by the kernel" entry), which makes a mask-then-dispatch scheme storm.
* **Fix:** aic8800 no longer installs a card interrupt. A thread polls the chip's own status
  (fn1 0x04): every 10 ms when idle, halving to 1 ms while frames arrive, as Linux does without
  MMC_CAP_SDIO_IRQ. dw_mmc acks a raw SDIO bit while nobody uses the host SDIO interrupt, so a stale
  latch cannot hold the line up. The RV (bwfm, host interrupt in use) is unchanged.
  Files: aic8800/if_aic_sdio.c (aic_poll_thread), busses/mmc/dw_mmc/DwMmc.cpp (_HandleInterrupt).

### AIC8800 scan details — confirmed (vf2lite-16)
* Message 0x004f is MM_CHANNEL_SURVEY_IND: one per scanned channel (busy time, noise). Linux keeps it
  for nl80211 survey; it is now ignored without a log line.
* "7 results (firmware 5)": the firmware counts networks, but sends one SCANU_RESULT_IND per frame,
  and one network can answer with a beacon and a probe response. The log now shows frames and
  distinct BSSIDs. net80211 merges them into one node anyway.
* After a data CRC error on a 2-block FIFO read (RINTSTS 0xa8), the scan-done message was lost and
  the driver never scanned again ("scanning" stayed set, so later SCAN states sent nothing). A 15 s
  watchdog now ends a stuck scan, or a connect with no answer, and starts again.

## AIC8800D80 driver (aic8800), milestone 3 "join" (confirmed, vf2lite-16)

Same model as bwfm on OpenBSD: the firmware does 802.11 auth/assoc (SM_CONNECT_REQ, answered by
SM_CONNECT_IND), net80211 runs the WPA2-PSK 4-way handshake, and the keys go into the firmware
(MM_KEY_ADD). For WPA the connect request sets CONTROL_PORT_HOST, so the firmware passes only
EAPOL until ME_SET_CONTROL_PORT opens the port after the pairwise key. Queued frames are flushed
before a key is installed, so message 4 leaves unencrypted.
- RX data comes as the 802.11 MPDU after a 60-byte hardware header. It is already decrypted, but the
  CCMP/TKIP header is still in it, so 8 bytes are skipped before the LLC header. hw_rxhdr's
  decr_status is in the hwvect status word (byte 36, bits 4:2); is_80211_mpdu is bit 1 of the flags
  word at byte 48 (layouts computed from rwnx_rx.h with a host compiler).
- RX frames are queued to the same task as the indications, in FIFO order. So EAPOL message 1, which
  can follow SM_CONNECT_IND in the same FIFO read, is handled after the state reaches RUN.
- TX: struct hostdesc (28 bytes) after a 4-byte SDIO data header (type 0x01, CRC-8 on the D80), then
  the frame without its Ethernet header (Linux rwnx_start_xmit + aicwf_sdio_aggr).

## NSPR cross-build silently drops its Haiku code (confirmed on the board)

Cross-configuring NSPR 4.39 for riscv64-unknown-haiku sets OS_ARCH from the target triple in
lowercase ("haiku"), but HaikuPorts' Makefiles test `ifeq ($(OS_ARCH),Haiku)`. Result: prhaiku.cpp
(the BTimeZone wrapper) and -lbe are left out, and libnspr4.so fails to load with "Could not resolve
symbol '_PR_Haiku_GetLocalTimeOffset'". Fix: map `haiku*) OS_ARCH=Haiku ;;` in configure(.in).
NSS is not affected when built with OS_ARCH=Haiku OS_TARGET=Haiku. Found 2026-11-26 by the Firefox
library test on an Orange Pi RV; after the fix 15/16 checks pass (ffi, libevent, fontconfig/FreeType,
HarfBuzz, cairo/pixman, VP9, NSS, dlopen). The VP8 failure was a test bug (packets must be drained
after every vpx_codec_encode call; VP8 has no lag). With the bundled fonts.conf, bundle_v3 passes
16/16 on the RV, and sans-serif resolves to Noto Sans with HarfBuzz ligatures (17 glyphs for 19 chars).

**vf2lite-16 result (2026-11-26): MILESTONE 3 CONFIRMED ON HARDWARE (WPA2-PSK join + data).**
- connecting (WPA, channel 6), associated (aid 9, QoS), ASSOC -> RUN, four EAPOL messages, pairwise and
  group keys installed, link up. DHCP gave an address, the user pinged the internet from the Lite,
  and a laptop pinged the Lite with 0% loss. RX polling (no card interrupt) carries it.
- Two indications are now handled without a log line: 0x0044 = MM_CHANNEL_SWITCH_IND (the firmware moved
  the vif to its channel context, at connect), and 0x140b = ME_TX_CREDITS_UPDATE_IND (per-TID TX
  credits after a block-ack session; Linux resizes its queues, and we send one frame at a time).

### AIC8800 firmware as a separate package (Armbian-style) — pending
`aic8800_firmware.hpkg` carries the 15 unmodified files of StarFive's aic8800d80-sdio-wifi-firmware 2.2,
with NOTICE and PROVENANCE (md5s match the Debian package) in /boot/system/data/firmware/aic8800/.
The driver already searches there after non-packaged. The files stay out of the repository: the build
takes them from BEACON_AIC8800_FIRMWARE_DIR and skips the package without it. Boards without the chip
(Orange Pi RV, VF2) are unaffected: aic8800 does not match them.
Files: build/jam/packages/Aic8800Firmware, src/data/package_infos/any/aic8800_firmware,
data/beacon/aic8800_firmware/.

### package_daemon double free on first boot — fix pending hardware test
Every board's first boot showed a debug_server dialog: `double free` in package_daemon, stack
`free <- ~VolumeState <- ~CommitTransactionHandler <- Volume::_CommitTransaction <- Root::_JobRunner`
(also in older rv53/rv57 logs; not riscv-specific). Cause, in upstream code: the first-boot
transaction (posted by Volume::InitPackages when the administrative directory is new) fills
`fPackagesToActivate` with Package objects owned by the handler's cloned VolumeState. When any step
threw, the handler's destructor deleted those packages, then `delete fVolumeState` freed them again.
Fix: the destructor skips packages the state still owns. The thrown exception was never logged, so
`Volume::_CommitTransaction()` now logs it (error, package, paths, strings, system error, full message)
and first-boot processing logs its steps; the next first-boot syslog tells us which step fails.
The "Failed to get activated packages info" lines are normal on a fresh image (no activated-packages
file yet). QEMU note: Debian's EDK2 (`qemu-efi-riscv64`) with `-M virt,acpi=off` boots the loader
into the kernel, but the kernel is silent on QEMU's serial; U-Boot 2025.10 S-mode crashed in the loader.
Files: src/servers/package/CommitTransactionHandler.cpp, Volume.cpp.
