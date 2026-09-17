# Cross-compilation toolchain for the Raspberry Pi Zero 1.1.
#
# *** This board is ARMv6, not ARMv7. ***
# The Pi Zero (1.x) and original Pi 1 use a BCM2835 SoC with a single
# ARM1176JZF-S core: ARMv6 + VFPv2. Every later Pi (2/3/4/400/Zero 2) is
# ARMv7-A or ARMv8-A. Debian's and Ubuntu's "armhf" port targets ARMv7-a
# baseline by policy, so a stock `crossbuild-essential-armhf` toolchain (and
# any prebuilt armhf .deb pulled via a generic sysroot) will produce
# SIGILL on this board. This is *the* classic footgun cross-compiling for
# Zero/Pi 1 -- see docs/CROSS_COMPILE.md for the two ways around it.
#
# This file assumes you already have an arm-linux-gnueabihf-{gcc,g++} that
# itself targets armv6 (e.g. built with crosstool-NG for arm1176jzf-s), and
# optionally a sysroot rsynced from a real Raspberry Pi OS install (needed
# for libgpiod's headers/.so, which docs/CROSS_COMPILE.md covers).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(_icom_toolchain_prefix arm-linux-gnueabihf)

find_program(CMAKE_C_COMPILER NAMES ${_icom_toolchain_prefix}-gcc)
find_program(CMAKE_CXX_COMPILER NAMES ${_icom_toolchain_prefix}-g++)

if(NOT CMAKE_C_COMPILER OR NOT CMAKE_CXX_COMPILER)
    message(FATAL_ERROR
        "${_icom_toolchain_prefix}-gcc/g++ not found on PATH.\n"
        "Install an armv6 arm-linux-gnueabihf toolchain (or the Raspberry Pi OS "
        "chroot/QEMU approach) -- see docs/CROSS_COMPILE.md. A plain "
        "'apt install crossbuild-essential-armhf' targets ARMv7 and will not "
        "run on a Pi Zero 1.1.")
endif()

# Optional: a copy of the Pi's root filesystem (rsynced from a running
# device, or extracted from a Raspberry Pi OS image) so find_package() etc.
# can see target headers/libraries such as libgpiod-dev.
set(ICOM_PI_SYSROOT "" CACHE PATH "Path to a Raspberry Pi OS sysroot (target headers/libs)")
if(ICOM_PI_SYSROOT)
    set(CMAKE_SYSROOT "${ICOM_PI_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${ICOM_PI_SYSROOT}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Matches the flags Raspberry Pi OS's own toolchain uses for armv6 targets:
# ARM1176JZF-S core, VFPv2 FPU, hard-float ABI.
set(_icom_cpu_flags "-march=armv6zk -mtune=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${_icom_cpu_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_icom_cpu_flags}")

# pkg-config must resolve .pc files from the sysroot, not the host.
set(ENV{PKG_CONFIG_LIBDIR} "${ICOM_PI_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${ICOM_PI_SYSROOT}/usr/lib/pkgconfig:${ICOM_PI_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${ICOM_PI_SYSROOT}")
