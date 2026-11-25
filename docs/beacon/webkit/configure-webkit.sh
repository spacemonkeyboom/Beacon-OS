#!/bin/sh
# SPDX-License-Identifier: MIT
# Beacon: configure HaikuWebKit for Haiku riscv64 (first pass: C_LOOP interpreter, no JIT).
set -e
W=$HOME/beacon/webkit
export BE_HOST_CPU=riscv64
exec cmake -S $W/src/haikuwebkit -B $W/build -G Ninja -DPORT=Haiku \
	-DCMAKE_TOOLCHAIN_FILE=$W/scripts/haiku-riscv64.toolchain.cmake \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/boot/system \
	-DENABLE_C_LOOP=ON -DENABLE_JIT=OFF -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF \
	-DENABLE_WEBASSEMBLY=OFF -DENABLE_SAMPLING_PROFILER=OFF \
	-DUSE_MIMALLOC=OFF -DUSE_AVIF=OFF -DUSE_LCMS=OFF -DUSE_JPEGXL=OFF -DENABLE_WEBGL=OFF \
	-DUSE_CXX_STDLIB_ASSERTIONS=OFF -DENABLE_API_TESTS=OFF \
	-DENABLE_LAYOUT_TESTS=OFF -DDEVELOPER_MODE=OFF -DSHOULD_INSTALL_JS_SHELL=ON \
	"-DCMAKE_CXX_FLAGS=-ftrack-macro-expansion=0 --param ggc-min-expand=10" \
	-DCMAKE_C_FLAGS=-ftrack-macro-expansion=0 "$@"
