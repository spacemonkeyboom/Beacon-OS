<!-- SPDX-License-Identifier: MIT -->
# Qt 5 on Haiku riscv64 (Beacon)

*Status, 2026-11-25:* **Qt 5.15.19 runs on Beacon** on the Orange Pi RV.
A test app opened a Qt window ("Qt5 on Beacon") and made an HTTPS request
through `QNetworkAccessManager` (HTTP 200). HaikuPorts has no Qt for riscv64,
so as far as we know this is the first Qt build for Haiku riscv64.
This is step 1 toward Falkon (Qt 5 + QtWebEngine 5.15).

## What's built

* **qtbase 5.15.19** (the KDE-maintained 5.15 branch HaikuPorts uses):
  Core, Gui, Widgets, Network (OpenSSL 3), Sql (system SQLite), Xml,
  Concurrent, PrintSupport, Test.
* **qthaikuplugins**: the Haiku platform plugin (`platforms/libqhaiku.so`),
  the Haiku style and the HVIF image plugin.
* **fontconfig 2.18.2 rebuilt with expat.** HaikuPorts' riscv64
  `fontconfig-2.18.2-2` links `libxml2.so.2`, but the riscv64 repository only
  has libxml2 2.15.3 (`libxml2.so.16`), so it cannot load. Our build uses
  expat, fontconfig's default XML parser.
* OpenGL and D-Bus are off (no Mesa for riscv64 yet).

## How it's built

Cross-compiled on Linux with Haiku's riscv64 gcc 13 cross tools, against a
sysroot of official HaikuPorts riscv64 packages (SHA-256 checked against the
repository index), the same way as [HaikuWebKit](WEBKIT.md).

* `qt5/configure-qtbase.sh`: configure line (`-xplatform
  haiku-riscv64-cross-g++`, `-sysroot`, Linux host tools).
* `qt5/beacon-qt5-5.15.19-riscv64.patch`: our changes on top of HaikuPorts'
  `qtbase-5.15.19.patchset`, covering qtbase and qthaikuplugins:
  * a `haiku-riscv64-cross-g++` mkspec;
  * link `-lgcc_s` (the cross gcc links only static libgcc, so
    `_Unwind_Resume` was undefined);
  * link `-lexecinfo` on Haiku when `execinfo.h` exists (`backtrace`);
  * qthaikuplugins: OpenGL optional (`QT_CONFIG(opengl)`).
* `qt5/configure-fontconfig.sh`: fontconfig with expat. Cross-compiling
  needs `ac_cv_va_copy=C99 fc_cv_c99_vsnprintf=yes`.
* `qt5/*.PackageInfo`: packages `qt5_base`, `qt5_base_devel` and `fontconfig`
  (vendor "Beacon"). `qt5_base` conflicts with HaikuPorts' `qt5` and
  `qthaikuplugins`, which install the same files.

## Running it

Runtime needs: `fontconfig` (ours), `freetype` 2.14, `expat` 2.10, plus
libraries Beacon already has (OpenSSL 3, ICU 74, SQLite, libpng, libjpeg,
zstd). HTTPS needs CA certificates in `/system/data/ssl/`.

## Next

QtWebEngine 5.15 (Chromium with HaikuPorts' Haiku patches), which needs a
RISC-V backend for its older V8; then Falkon. Qt 6 later.

## Credits

Qt is LGPL-3.0/GPL; qthaikuplugins (Gerasim Troeglazov) is MIT; HaikuPorts'
patches and recipes are MIT. Beacon's changes are MIT.
