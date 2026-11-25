#!/bin/sh
# SPDX-License-Identifier: MIT
# Beacon: configure fontconfig 2.18.2 (HaikuPorts patches) for Haiku riscv64
# with expat as the XML parser (the riscv64 repo has no libxml2.so.2).
# Usage: configure-fontconfig.sh <build-dir>
set -e
Q=$HOME/beacon/qt5
. $Q/notes/env.sh
mkdir -p "$1"
cd "$1"
exec $Q/src/fontconfig-2.18.2/configure \
	--host=riscv64-unknown-haiku --build=x86_64-pc-linux-gnu \
	CC="riscv64-unknown-haiku-gcc --sysroot=$Q/sysroot" \
	--prefix=/boot/system --exec-prefix=/boot/system \
	--bindir=/boot/system/bin --libdir=/boot/system/lib \
	--includedir=/boot/system/develop/headers \
	--datadir=/boot/system/data --datarootdir=/boot/system/data \
	--sysconfdir=/boot/system/settings --localstatedir=/boot/system/var \
	--mandir=/boot/system/documentation/man \
	--docdir=/boot/system/documentation/packages/fontconfig \
	--disable-static --disable-docs --disable-nls \
	--with-default-fonts=/boot/system/data/fonts \
	--with-add-fonts=/boot/system/non-packaged/data/fonts,/boot/home/config/data/fonts,/boot/home/config/non-packaged/data/fonts \
	--with-cache-dir=/boot/home/config/cache \
	ac_cv_va_copy=C99 fc_cv_c99_vsnprintf=yes
