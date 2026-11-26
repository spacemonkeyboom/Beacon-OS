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

*Pending (rv65).* On the Orange Pi RV the Wi-Fi chip sometimes (a) stops
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

*Pending (vf2lite-03).* The VisionFive 2 and the Orange Pi RV get their USB-A
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
on some boots, clean on others with identical display/clock logs); fix in
vf2lite-04, pending.* With the uncached DDR alias (RenkuOS #490),
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
the glue ran twice; fix in vf2lite-05, pending.* The glue published its
"xhci_platform" child with register_node(..., NULL module, ...), which the
device manager rejects; the failed child registration also made the glue
node get initialized again (the second power-up sequence). Fix: the glue
node now carries xhci/phys, xhci/size, xhci/irq and names
"busses/usb/xhci/platform/driver_v1" as its B_DEVICE_FIXED_CHILD; the xhci
platform module is attached only that way (its supports_device returns 0).
Also measured: PLL0 on the Lite is 1.0 GHz (not the 1.25 GHz in Linux's
DT), so usb_125m needs div 8; the driver computes it from the PLL registers.
