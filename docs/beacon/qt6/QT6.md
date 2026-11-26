# Qt 6.10.3 for Haiku riscv64 (Beacon cross-build)

Built 2026-11-25 on the Linux laptop as the base for a later QtWebEngine 6
browser (WebEngine itself not started). Nothing has been run on the board yet.
Layout is parallel to ~/beacon/qt5 (dl, src, build, host, stage*, pkg, logs,
notes, testapp, tools).

## HaikuPorts Qt 6 (read-only reference)

Sparse clone of haikuports (commit 5354c053c4ea, dev-qt + dev-qt/qtwebengine)
in ~/beacon/refs/haikuports-qt6-2026-11-25/hp.

* Qt version: **6.10.3** for every qt6-* recipe (qt6_base rev 3, the others rev 2).
* qt6_base-6.10.3.recipe: CMake + Ninja, 23-patch patchset
  (qt6_base-6.10.3.patchset). The Haiku platform plugin is **not** a separate
  package in Qt 6: the recipe downloads threedeyes/qt6-haikuplugins
  (commit 1d255f2f, SOURCE_URI_2, 1-patch source2 patchset), builds it with
  qmake6 after installing qtbase and ships libqhaiku, libqhvif,
  libqhaikustyle and Qt6Configurator inside qt6_base.
* qt6_declarative: 2 patches (Haiku fix, build fix). qt6_shadertools,
  qt6_webchannel, qt6_positioning: no patches.
* **No qt6_webengine recipe.** HaikuPorts only has qtwebengine-5.15.18
  (ARCHITECTURES "!x86_gcc2 ?x86_64", i.e. x86_64 untested-only) with
  qtwebengine-5.15.18.patchset (2 patches: Haiku patchset for qtwebengine,
  Arch/gentoo python3 patches) and qtwebengine-chromium-5.15.18.patchset
  (9 patches: Haiku patchset for Chromium 87, CloseSuperfluousFds, fstat,
  FD access mode check, python3, x86 build fix, internal ffmpeg, discardable
  memory, Haiku x86_64 build fix). Plus qtwebengine_bin and pyqtwebengine5.
  A Qt 6 WebEngine port would have to start from these 5.15 patches.

## Sources (checksums verified against the recipes)

dl/, list in notes/SHA256SUMS.expected (all OK):
qtbase, qtshadertools, qtdeclarative, qtwebchannel 6.10.3
(ftp.fau.de/qtproject mirror, as in the recipes) and
qt6-haikuplugins-1d255f2f.tar.gz. Each tree in src/ is a git repo: commit
"orig", then the HaikuPorts patchsets via `git am` (all applied cleanly),
Beacon changes left uncommitted (saved as patches, below).

Injection scan (recipes, patchsets, all five source trees): no agent files
(AGENTS.md, CLAUDE.md, .claude, .cursorrules, copilot-instructions, GEMINI.md),
no AI-directed text. Hidden-Unicode hits are all legitimate data: test suites
(test262, XML conformance, emoji/IDNA tables), ZWNJ in Persian text
(harfbuzz README, Android fa strings), ZWJ in freetype's Mongolian blue zones
(afblue.dat/.c), emoji tag sequences in Unicode data/test files.

## Host tools (Linux, 6.10.3)

* build/host_qtbase -> host/: `configure -prefix ~/beacon/qt6/host -release
  -nomake examples -nomake tests -no-opengl -no-dbus -no-glib -qt-pcre
  -qt-harfbuzz -qt-doubleconversion -no-icu -no-openssl -no-feature-sql`
  (built from the same patched tree as the target).
* notes/build-host-module.sh qtshadertools, qtdeclarative (qt-configure-module).
  Gives qsb, qmlcachegen, qmltyperegistrar, qmlimportscanner etc.

## Target (Haiku riscv64)

* Toolchain file notes/haiku-riscv64-toolchain.cmake: CMAKE_SYSTEM_NAME Haiku,
  gcc 13 cross tools, sysroot ~/beacon/qt5/sysroot (same SHA-checked sysroot
  as Qt 5, with our libglvnd/Mesa), develop/lib + develop/headers search
  paths, `-lgcc_s` as standard library, pkg-config env, and
  -ffile-prefix-map (see problem 6).
* notes/configure-qtbase.sh <build-dir> (env QT_STAGE, QT_HOST): install
  layout from the HaikuPorts recipe (bin lib/Qt6, plugins add-ons/Qt6, headers
  develop/headers/Qt6, data data/Qt6, ...), CMAKE_STAGING_PREFIX = stage,
  QT_HOST_PATH = host. Features: desktop OpenGL + EGL (libglvnd), no Vulkan,
  OpenSSL 3 linked, system SQLite, ICU, fontconfig, system freetype/zlib/png/
  jpeg, bundled harfbuzz/pcre2/double-conversion (not in our sysroot),
  no D-Bus/glib/xcb, no rpath, zstd, syslog, backtrace (libexecinfo).
  Result: Core Gui Widgets Network Sql Xml Concurrent PrintSupport Test
  OpenGL OpenGLWidgets EglFSDeviceIntegration + platforms eglfs/minimal/
  minimalegl/offscreen/vnc, sqlite driver, openssl + certonly TLS backends.
* qt6-haikuplugins: notes/build-haikuplugins.sh <build-dir> (staged qmake6,
  then copies into the stage like the recipe, plus the Deskbar Preferences link).
* Other modules: notes/build-cross-module.sh <module> [cmake args]
  (cmake with the stage's lib/cmake/Qt6/qt.toolchain.cmake).
* Runs actually packaged: build/qtbase_v2 -> stage_v2, build/qt6haikuplugins_v2b.
  build/qtbase + stage (first run) and build/qt6haikuplugins were before the
  file-prefix-map fix and are kept but not used.

## Problems and fixes

1. **Host build: `ambiguating new declaration of memrchr`.** HaikuPorts patch
   "Fix* missing function for memrchr" declares `extern void *memrchr(...)` in
   qbytearray.cpp unconditionally; glibc has a C++ const overload. Fix:
   `#ifdef Q_OS_HAIKU` around it.
2. **Host build: libQt6Gui link `relocation R_X86_64_PC32 ... recompile with
   -fPIC`** (bundled harfbuzz). HaikuPorts "Haiku build setup" sets
   CMAKE_POSITION_INDEPENDENT_CODE OFF for everyone (the recipe passes -fPIC
   in CFLAGS). Fix in cmake/QtBuildHelpers.cmake: OFF only when
   CMAKE_SYSTEM_NAME is Haiku (our target flags have -fPIC as in the recipe).
3. **Cross qtbase: qlogging.cpp `binding reference of type 'void*&' to 'void*
   const' discards qualifiers`.** Upstream Qt 6.10 bug in the execinfo
   backtrace path (used because libexecinfo is in the sysroot; Linux glibc
   builds take another path): lambda `decodeFrame(void *&)` is called with
   `void *const &` from a range-for. Fix: lambda takes `void *const &`
   (backtrace_symbols takes `void *const *`).
4. **qt6-haikuplugins post-link: `rc: not found`, `xres`, `mimeset`.** The
   Qt6Configurator .pro runs makeres.sh. Fix: use the Linux builds of rc and
   xres from the Haiku tree (generated.riscv64/objects/linux/x86_64/release/
   tools, LD_LIBRARY_PATH=objects/linux/lib); mimeset fails with "Operation
   not supported" (no BFS attributes on Linux), so tools/noattr/mimeset is a
   no-op wrapper (Haiku sets the MIME type on install).
5. **Staged `qt-configure-module` fails: `../lib/Qt6/qt-cmake-private: not
   found`.** With the Haiku layout (bindir = libexecdir = lib/Qt6) the script
   computes lib/lib/Qt6. Fix: call cmake directly with
   -DCMAKE_TOOLCHAIN_FILE=<stage>/lib/cmake/Qt6/qt.toolchain.cmake.
6. **Build-machine paths inside every binary** (e.g. `/home/.../cross-tools-
   riscv64/.../include/c++/13.3.0/bits/stl_vector.h`): Qt 6 builds with
   `_GLIBCXX_ASSERTIONS`, whose messages contain `__FILE__` of libstdc++
   headers. Fix: `-ffile-prefix-map` for the cross-tools c++ dir (->
   /boot/system/develop/headers/c++), src/ and build/ (in the toolchain file
   as *_FLAGS_INIT and in configure-qtbase.sh), then a full rebuild
   (build/qtbase_v2, stage_v2). Note: `grep` on this laptop is ugrep; plain
   `grep -rl` missed the matches in ELF files, `grep -rla` finds them. The
   packaging check uses -a.
7. **qmake6 wrapper splits quoted arguments** (`$*` unquoted in the generated
   wrapper), so `"QMAKE_CXXFLAGS+=-a -b"` broke qmake. Fix: one space-free
   assignment per flag.
8. **package create: `License 'GNU FDL v1' isn't contained in package`.**
   Haiku does not ship that licence. Fix: notes/licenses/GNU FDL v1 (copied
   from the HaikuPorts recipe licenses dir) goes into data/licenses of every
   package.
9. Test apps: Qt's CMake adds a build RUNPATH pointing into the stage. Fix:
   -DCMAKE_SKIP_BUILD_RPATH=ON (notes/build-testapp.sh refuses host paths).

## OSMesa bridge (GL_BACK fix)

qt6-haikuplugins has the same osmesa_bgl.h as Qt 5's qthaikuplugins (OSMesa
API emulated over BGLView). Same two Beacon changes:
`glReadBuffer(GL_BACK)` instead of GL_FRONT (the frame just rendered; no swap
has happened yet), and `<GL/glu.h>` only `#if __has_include` plus `-lGL`
without `-lGLU` in platform.pro (no GLU for riscv64; GLU is not used).
libqhaiku.so NEEDED: libroot, libtracker, libgame, libfreetype.so.6,
libQt6Widgets/Gui/Core.so.6, libbe, libGL.so.1, libgcc_s, libstdc++.

Patches: notes/beacon-qt6-base-6.10.3-riscv64.patch (problems 1-3 + the
haiku-riscv64-cross-g++ mkspec for qmake, a copy of the Qt 5 one) and
notes/beacon-qt6-haikuplugins-osmesa-backbuffer.patch.

## Packaging

notes/make-qt6-packages.py <name> <new-out-dir> <install_manifest.txt>...
[--extra path] (env QT_STAGE). Runtime vs devel split like the recipes
(develop/, lib/cmake, *.a, *.prl, objects-*, pkgconfig -> develop/lib/pkgconfig,
mkspecs/metatypes/modules, unversioned .so links -> develop/lib in devel).
The build-machine qmake/qt-cmake wrappers in lib/Qt6 are left out (no target
qmake/moc in a cross build; the devel packages are for cross-compiling, as
for Qt 5). Text files get build paths rewritten to /boot/system; it refuses
to package if any $HOME path is left in any file (text or binary).

Names match HaikuPorts (qt6_base, qt6_shadertools, qt6_declarative,
qt6_webchannel + _devel), vendor Beacon, revision -100 so they replace
HaikuPorts' -2/-3. Because the names are the same, they replace HaikuPorts'
packages rather than conflicting with them; no conflicts entry is needed (or
possible). HaikuPorts' qt6_base also requires haiku_svg_icon_theme, cmd:qsystray,
cmd:qnotify, libGLU, libvulkan, libharfbuzz, libpcre2_16, libdouble_conversion,
libintl. Ours does not: tray/notification helpers are optional at runtime,
and the others are bundled or not used.

## Status at hand-off (2026-11-25 evening)

Packages (pkg/, sha256):
* qt6_base_v2c/qt6_base-6.10.3-100-riscv64.hpkg 632b9720095942a82f7b755cbbbc1dcc7a1814a419c058b04ecc0f21323a1ced
* qt6_base_v2c/qt6_base_devel-6.10.3-100-riscv64.hpkg e533693a2f994e50b3776ed8fc5a944b5d59876bdc756c1aa27e7b6e432e46f5
* qt6_shadertools/qt6_shadertools-6.10.3-100-riscv64.hpkg 2fd712de277b727c50d199769a5ffa3db3d08e7407591b0756179948240c7dbe
* qt6_shadertools/qt6_shadertools_devel-6.10.3-100-riscv64.hpkg 11ca74ebcbf48f7a04e848b0ff32269274d0fc681b3c140898ece6afbfd87007
* qt6_webchannel/qt6_webchannel-6.10.3-100-riscv64.hpkg e43d0601768732251476f982d1715eaf4f6341df603ae0c7fc26ddf399c6e4df
* qt6_webchannel/qt6_webchannel_devel-6.10.3-100-riscv64.hpkg f5b782dd937a6ee15a9905ebdf15561479b994ef6c4571ce8caab00a2ac4d95a
(pkg/qt6_base, qt6_base_v2, qt6_base_v2b are failed/refused attempts, kept.)

qtdeclarative: build/qtdeclarative (v1) built fine with qml_jit off (no
riscv64 JIT backend in 6.10; V4 interpreter), but libQt6QuickTest and the
quickcontrolstestutils plugin held __FILE__ paths of the staged QtTest
headers (problem 10, fix: stage prefix map in the toolchain file). Rebuild
build/qtdeclarative_v2 was still running at hand-off (log
logs/cross_declarative_v2.out; v1 logs saved as qtdeclarative_*_v1.log).
Next: `python3 -I notes/gen-packageinfo.py qt6_declarative "<summary>" "<desc>"
build/qtdeclarative_v2/install_manifest.txt`, then make-qt6-packages.py
qt6_declarative pkg/qt6_declarative ... The static test-utils libs
(libQt6QuickTestUtils.a, libQt6QuickControlsTestUtils.a) embed
QT_TESTCASE_BUILDDIR (a -D define, not fixable by prefix maps); if the
checker refuses them, leave those two out of the devel package.
TODO: build-cross-module.sh reuses log names per module; give each build dir
its own logs.

Test apps (cross-built, not run): build/testapp_qt6widget_v2/qt6widget,
build/testapp_qt6quick/qt6quick (QQuickView; `--widget` = QQuickWidget).
Board install set: qt6_base, qt6_shadertools, qt6_declarative,
qt6_webchannel, plus libglvnd, mesa, fontconfig-2.18.2-100 from the Qt 5 work.
