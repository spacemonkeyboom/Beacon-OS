#!/bin/sh
# SPDX-License-Identifier: MIT
# Beacon: configure Qt 5.15.19 qtbase (HaikuPorts patches + Beacon patch)
# for Haiku riscv64, cross-compiled on Linux. Usage: configure-qtbase.sh <build-dir>
set -e
Q=$HOME/beacon/qt5
B=${1:-$Q/build/qtbase}; [ $# -gt 0 ] && shift
export PATH=$HOME/beacon/haiku/generated.riscv64/cross-tools-riscv64/bin:$PATH
export PKG_CONFIG_SYSROOT_DIR=$Q/sysroot
export PKG_CONFIG_LIBDIR=$Q/sysroot/boot/system/develop/lib/pkgconfig
export PKG_CONFIG_PATH=
mkdir -p "$B"
cd "$B"
exec $Q/src/qtbase/configure \
	-xplatform haiku-riscv64-cross-g++ -sysroot $Q/sysroot \
	-extprefix $Q/stage/boot/system -hostprefix $Q/host \
	-prefix /boot/system \
	-bindir /boot/system/bin \
	-libdir /boot/system/lib \
	-libexecdir /boot/system/lib \
	-headerdir /boot/system/develop/headers \
	-docdir /boot/system/documentation/packages/qt5/Qt5 \
	-datadir /boot/system/data/Qt5 \
	-archdatadir /boot/system/data/Qt5 \
	-translationdir /boot/system/data/Qt5/translations \
	-sysconfdir /boot/system/settings \
	-testsdir /boot/system/data/Qt5/tests \
	-examplesdir /boot/system/data/Qt5/examples \
	-importdir /boot/system/data/Qt5/imports \
	-qmldir /boot/system/data/Qt5/qml \
	-plugindir /boot/system/add-ons/Qt5 \
	-release -opensource -confirm-license -no-pch \
	-nomake examples -nomake tests \
	-no-iconv -no-rpath -strip \
	-pkg-config -D _BSD_SOURCE \
	-no-opengl -no-dbus -no-glib -no-gtk \
	-system-zlib -system-libpng -system-libjpeg -system-sqlite \
	-system-freetype -fontconfig -qt-harfbuzz -qt-pcre -qt-doubleconversion \
	-icu -openssl-linked \
	-feature-ipc_posix \
	"$@"
