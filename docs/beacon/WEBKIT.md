<!-- SPDX-License-Identifier: MIT -->
# HaikuWebKit and WebPositive on riscv64

Beacon builds HaikuWebKit 1.10.0 (the engine behind WebPositive) for Haiku
riscv64. The packages should install on any Haiku riscv64 system, not only
Beacon. Status: **confirmed**: WebPositive runs on the Orange Pi RV (2026-11-25).

## What works so far

* `jsc` (the JavaScriptCore shell) runs correctly on the Orange Pi RV:
  loops, Promises, JSON, arrow functions, `Date`, regular expressions. A
  1-million-iteration loop takes about 1 s.
* `libJavaScriptCore.so`, `libWebKitLegacy.so` and WebPositive cross-build.

## Configuration

* JavaScript runs on **C_LOOP**, WebKit's portable C++ interpreter. JIT,
  DFG, FTL and WebAssembly are off: JavaScriptCore's riscv64 assembly
  interpreter is incomplete in 1.10.0 (the `transferp` instruction used by
  IPInt has no riscv64 lowering).
* WebGL, AVIF, LCMS, JPEG XL and mimalloc are off. HaikuPorts has no Mesa
  for riscv64 yet, so nothing links libGL or EGL.
* Cross-compiled on Linux with Haiku's riscv64 gcc 13 cross tools, against a
  sysroot of official HaikuPorts riscv64 packages (each checked against the
  repository index's SHA-256). Files in [webkit/](webkit/):
  `make-sysroot.py`, `haiku-riscv64.toolchain.cmake`, `configure-webkit.sh`.

## Source changes

[webkit/haikuwebkit-1.10.0-riscv64.patch](webkit/haikuwebkit-1.10.0-riscv64.patch)
applies to the HaikuWebKit 1.10.0 release tarball. Each change is explained
in [webkit/PATCHES.md](webkit/PATCHES.md). The patches keep HaikuWebKit's
licences (LGPL-2.1 / BSD). The most reusable ones:

* `MachineContext.h`: Haiku's riscv64 register mapping. In
  `struct vregs`, `x[n-1]` is register `xn`, followed by `pc`.
* `PlatformEnable.h`: riscv64 forced WebAssembly on even with C_LOOP,
  which breaks the build.
* `PlatformHaiku.cmake`: a plain `-I/system/...` path is not rewritten by
  `--sysroot` when cross-compiling.
* GL and EGL made optional, for platforms without Mesa.

## Packages

* `haikuwebkit-1.10.0-1-riscv64.hpkg`: the libraries, `jsc` and data.
  Vendor "Beacon": this is our build, not a HaikuPorts package.
* `haikuwebkit_devel-1.10.0-1-riscv64.hpkg`: headers and link symlinks.
* The package definitions are in [webkit/](webkit/) (`*.PackageInfo`).

The dependencies are official HaikuPorts riscv64 packages, newer than
Haiku's riscv64 build-packages snapshot:

* new: brotli, libexecinfo, libidn2, libpsl, libunistring, libxml2,
  libxslt, sqlite and woff2;
* newer versions: libiconv, ncurses6, readline and zlib.

`resolve-deps.py` works out that list from an image's package set.

## How the Beacon image includes them

* `build/jam/repositories/HaikuPorts/riscv64` lists the extra and newer
  packages. The files are fetched into `generated/download` beforehand.
* `build/jam/RepositoryRules` builds the riscv64 HaikuPorts index locally,
  because upstream has no index for a changed list. haikuwebkit is kept out
  of that index, since its vendor is not "Haiku Project".
* `build/jam/DefaultBuildProfiles` enables WebPositive for riscv64 and adds
  haikuwebkit as a package file.
