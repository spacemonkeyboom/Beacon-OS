# Beacon patches to HaikuWebKit 1.10.0 for Haiku riscv64

Cross-compiling HaikuWebKit (WebPositive's engine) for Haiku riscv64 from Linux.
These changes stay under HaikuWebKit's own licences (LGPL-2.1 / BSD), the same
as the files they touch. Every change is marked in the source with a `Beacon:`
comment. Originals are kept next to this file as `*.orig`.

Build configuration (scripts/configure-webkit.sh): ENABLE_C_LOOP=ON (portable
C++ interpreter), JIT/DFG/FTL/WebAssembly OFF, USE_MIMALLOC=OFF, AVIF/LCMS/
JPEGXL/WebGL OFF (no such libraries or Mesa for riscv64 in HaikuPorts yet).

| # | File (under Source/) | Change | Why |
|---|---|---|---|
| 1 | JavaScriptCore/runtime/MachineContext.h | Added `CPU(RISCV64)` cases for Haiku: stack pointer `x[1]` (x2/sp), frame pointer `x[7]` (x8/s0), instruction pointer `pc`, `argumentPointer<1>` `x[10]` (x11/a1), LLInt PC `x[8]` (x9), wasm instance `x[13]` (x14); same registers the Linux riscv64 cases use (`__gregs[n]` = xn there) | Haiku's riscv64 `struct vregs` is `x[0..30]` = registers x1..x31, then `pc`. The file only knew x86_64 on Haiku and hit `#error Unknown Architecture`. |
| 2 | cmake/OptionsHaiku.cmake | `find_package(OpenGLES2)` no longer `REQUIRED` | GL is only needed for WebGL; riscv64 has no Mesa package. |
| 3 | WebCore/CMakeLists.txt | Link `OpenGL::GLES` only `elseif (TARGET OpenGL::GLES)` | Follows from 2: no GL target exists without Mesa. |
| 4 | JavaScriptCore/llint/InPlaceInterpreter.asm | Guarded one `nextIPIntInstruction()` call with `if JSVALUE64 and (ARM64 or ARM64E or X86_64)` (else `break`) | That macro is only defined where InPlaceInterpreter64.asm is included; other CPUs failed the offset extractor. Only matters for the asm LLInt (not used now; harmless). |
| 5 | JavaScriptCore/offlineasm/riscv64.rb | `emitCall` accepts `LocalLabelReference`; `t9`/`t10` map to x6/x7 (same as ws0/ws1) | Generic LLInt code uses these on riscv64. Only matters for the asm LLInt (not used now; harmless). |
| 6 | WTF/wtf/PlatformEnable.h | The block forcing `ENABLE_WEBASSEMBLY 1` on `CPU(RISCV64)` now also requires `!ENABLE(C_LOOP)` | It overrode `-DENABLE_WEBASSEMBLY=OFF`; with C_LOOP the Wasm headers (which need JIT register types like `JSValueRegs`, `RegisterSet`) then failed to compile, e.g. in LLIntSettingsExtractor.cpp. C_LOOP has no Wasm support. |

Note: LowLevelInterpreter.asm.orig exists from an experiment, but that file is
currently identical to the original (the change was reverted).

| # | File (under Source/) | Change | Why |
|---|---|---|---|
| 7 | WebCore/PlatformHaiku.cmake, WebKitLegacy/PlatformHaiku.cmake | Private include dir `/system/develop/headers/private/netservices` is now `${CMAKE_SYSROOT}/boot/system/develop/headers/private/netservices` | Absolute host path did not exist when cross-compiling (gcc's `--sysroot` does not rewrite plain `-I` paths), so `UrlProtocolDispatchingListener.h` was not found (MediaPlayerPrivateHaiku.cpp, a unified source). Natively `CMAKE_SYSROOT` is empty and `/boot/system` is what `/system` points to, so this is unchanged there. (WTF/wtf/PlatformHaiku.cmake has the same pattern for private/system but needed no change so far; left alone to avoid a full rebuild.) |
| 8 | WebCore/PlatformHaiku.cmake | `platform/graphics/PlatformDisplay.cpp` only added `if (TARGET OpenGL::GLES)` | It is EGL-only (needs `EGL/egl.h`) and nothing in the Haiku port calls it; riscv64 has no Mesa/EGL. |
| 9 | WebKitLegacy/PlatformHaiku.cmake | `GL` removed from the fixed library list and appended only `if (TARGET OpenGL::GLES)` | riscv64 Haiku has no libGL (no Mesa); link failed with `cannot find -lGL`. |

## Result (2026-11-25)

`ninja -C build` completes (logs build7..build11.log, install1.log). Staged with
`DESTDIR=stage ninja -C build install` into stage/boot/system:
bin/jsc, lib/libJavaScriptCore.so.18.7.4, lib/libWebKitLegacy.so.1.10.0 (WebCore,
PAL, WTF and bmalloc are static and linked into libWebKitLegacy), headers and
data/WebKit. Binaries are not stripped (they carry debug info). Not yet run on
hardware.
