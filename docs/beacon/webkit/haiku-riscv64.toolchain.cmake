# SPDX-License-Identifier: MIT
# Beacon: CMake toolchain for cross-compiling to Haiku riscv64 with Haiku's
# cross-tools, against the sysroot made by make-sysroot.py.
set(CMAKE_SYSTEM_NAME Haiku)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(BEACON_CROSS $ENV{HOME}/beacon/haiku/generated.riscv64/cross-tools-riscv64/bin)
set(CMAKE_C_COMPILER   ${BEACON_CROSS}/riscv64-unknown-haiku-gcc)
set(CMAKE_CXX_COMPILER ${BEACON_CROSS}/riscv64-unknown-haiku-g++)
set(CMAKE_AR           ${BEACON_CROSS}/riscv64-unknown-haiku-ar)
set(CMAKE_RANLIB       ${BEACON_CROSS}/riscv64-unknown-haiku-ranlib)
set(CMAKE_STRIP        ${BEACON_CROSS}/riscv64-unknown-haiku-strip)

set(CMAKE_SYSROOT $ENV{HOME}/beacon/webkit/sysroot)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Haiku keeps link-time libraries and headers under develop/.
list(APPEND CMAKE_LIBRARY_PATH /boot/system/develop/lib)
list(APPEND CMAKE_INCLUDE_PATH /boot/system/develop/headers)
list(APPEND CMAKE_PREFIX_PATH /boot/system/develop /boot/system)

set(ENV{PKG_CONFIG_SYSROOT_DIR} ${CMAKE_SYSROOT})
set(ENV{PKG_CONFIG_LIBDIR} ${CMAKE_SYSROOT}/boot/system/develop/lib/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
