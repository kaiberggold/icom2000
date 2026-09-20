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
# itself targets armv6 (e.g. built with crosstool-NG for arm1176jzf-s) --
# it verifies that assumption itself, see "Sanity-check" below, rather
# than just trusting it. libgpiod cross-builds from source as part of the
# main build (src/gpio/cmake/BuildLibgpiodFromSource.cmake) by default,
# so no sysroot is needed for that specifically; ICOM_PI_SYSROOT below
# remains available for anything else that genuinely needs to match the
# target Raspberry Pi OS build exactly (see docs/CROSS_COMPILE.md).

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
# ARM1176JZF-S core, VFPv2 FPU, hard-float ABI. `-marm` matters as much as
# the others here: without it some toolchains default to Thumb, and at
# least Ubuntu's gcc-arm-linux-gnueabihf refuses outright ("sorry,
# unimplemented: Thumb-1 'hard-float' VFP ABI") rather than silently
# picking Thumb-2 -- ARMv6 (pre-T2) doesn't have a hard-float Thumb-1
# encoding for this ABI.
set(_icom_cpu_flags "-marm -march=armv6zk -mtune=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${_icom_cpu_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_icom_cpu_flags}")

# Exposed for anything else that needs to reproduce these exact target
# flags outside CMake's own compiler invocations -- e.g. src/gpio's
# ExternalProject build of libgpiod, which drives a *separate* build
# system (meson) that has no visibility into CMAKE_C_FLAGS_INIT at all.
# CACHE INTERNAL because plain `set()` here would only be visible for the
# remainder of *this* toolchain-file inclusion, not to CMakeLists.txt code
# that runs later in subdirectories.
set(ICOM_TARGET_CPU_FLAGS "${_icom_cpu_flags}" CACHE INTERNAL "Target CPU flags, for build systems CMake doesn't drive directly")

# pkg-config must resolve .pc files from the sysroot, not the host.
set(ENV{PKG_CONFIG_LIBDIR} "${ICOM_PI_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${ICOM_PI_SYSROOT}/usr/lib/pkgconfig:${ICOM_PI_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${ICOM_PI_SYSROOT}")

# --- Sanity-check: can this compiler actually PRODUCE an ARMv6 binary? ---
#
# Passing -march=armv6zk does not guarantee ARMv6 output. Confirmed by
# direct testing: Ubuntu's (and, by all indications, Debian's)
# gcc-arm-linux-gnueabihf package correctly compiles *individual*
# translation units to genuine ARMv6 object code with these flags -- but
# every prebuilt runtime object it ships (crt1.o, and every member of
# libgcc.a: division helpers, __main.o, etc.) is hard-tagged
# `Tag_CPU_arch: v7`, unconditionally, because that's the only multilib
# variant Debian's armhf port policy builds. Those objects get pulled
# into *every* linked executable (crt1.o always; libgcc members whenever
# your code needs e.g. software integer division, which ARMv6 code
# often does), so the final binary silently ends up mixed-architecture
# and can crash with SIGILL on a real ARM1176JZF-S (Pi Zero 1.x/Pi 1) --
# no diagnostic from the compiler or linker, because everything involved
# is individually valid; it's the combination that isn't. This is not
# fixable with different flags; it needs a toolchain whose runtime
# objects were themselves built for ARMv6 (see docs/CROSS_COMPILE.md).
#
# This probe catches exactly that: compile+link a trivial program with
# the real flags (so crt1.o/libgcc.a get pulled in the same way they
# would for intercomd), then check the resulting binary's own
# Tag_CPU_arch rather than trusting the flags we asked for.
option(ICOM_SKIP_ARMV6_CHECK "Skip the ARMv6 toolchain capability probe (only for testing this CMake config itself on a non-ARMv6-capable toolchain -- a binary built this way should not be trusted on real Pi Zero 1.x hardware)" OFF)

# CMake re-includes this toolchain file for its own internal compiler-ABI
# try_compile() checks, each in a throwaway "CMakeScratch" project with
# its own cache -- one that never saw -DICOM_SKIP_ARMV6_CHECK, so the
# option would silently reset to OFF and re-run (and re-fail) the probe
# there instead of just letting that internal check proceed. None of
# those scratch builds are intercomd's actual output, so skip unconditionally
# whenever we're inside one.
if(CMAKE_BINARY_DIR MATCHES "CMakeScratch")
    set(_icom_in_try_compile TRUE)
else()
    set(_icom_in_try_compile FALSE)
endif()

if(NOT ICOM_SKIP_ARMV6_CHECK AND NOT _icom_in_try_compile)
    find_program(_icom_readelf NAMES ${_icom_toolchain_prefix}-readelf)

    if(_icom_readelf)
        set(_icom_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/icom_armv6_probe")
        file(MAKE_DIRECTORY "${_icom_probe_dir}")
        file(WRITE "${_icom_probe_dir}/probe.c" "int main(void) { return 0; }\n")

        separate_arguments(_icom_cpu_flags_list UNIX_COMMAND "${_icom_cpu_flags}")
        execute_process(
            COMMAND ${CMAKE_C_COMPILER} ${_icom_cpu_flags_list} -o "${_icom_probe_dir}/probe" "${_icom_probe_dir}/probe.c"
            RESULT_VARIABLE _icom_probe_rc
            OUTPUT_QUIET ERROR_QUIET
        )

        if(_icom_probe_rc EQUAL 0)
            execute_process(
                COMMAND ${_icom_readelf} -A "${_icom_probe_dir}/probe"
                OUTPUT_VARIABLE _icom_probe_attrs
                ERROR_QUIET
            )
            if(NOT _icom_probe_attrs MATCHES "Tag_CPU_arch: v6")
                message(FATAL_ERROR
                    "${_icom_toolchain_prefix}-gcc does not produce genuine ARMv6 "
                    "binaries. A trivial program compiled with the exact target "
                    "flags ('${_icom_cpu_flags}') still links in non-ARMv6 "
                    "runtime code -- readelf -A reports:\n\n"
                    "${_icom_probe_attrs}\n"
                    "This is a known limitation of Debian/Ubuntu's packaged "
                    "gcc-arm-linux-gnueabihf: its crt1.o and libgcc.a are only "
                    "built for ARMv7-A, regardless of -march. No combination of "
                    "flags fixes this -- see docs/CROSS_COMPILE.md for toolchains "
                    "that actually work (a QEMU-chroot build against real "
                    "Raspberry Pi OS, or a crosstool-NG toolchain built for "
                    "arm1176jzf-s). To bypass this check anyway (e.g. to test "
                    "this CMake configuration itself, NOT to produce a binary "
                    "for real hardware), reconfigure with "
                    "-DICOM_SKIP_ARMV6_CHECK=ON.")
            endif()
        else()
            message(WARNING
                "Could not run the ARMv6 toolchain probe (compiling a trivial "
                "program with '${_icom_cpu_flags}' failed) -- continuing "
                "anyway, but this toolchain may not work at all. See "
                "docs/CROSS_COMPILE.md if the build fails.")
        endif()
    else()
        message(WARNING
            "${_icom_toolchain_prefix}-readelf not found -- skipping the "
            "ARMv6 toolchain capability probe. If intercomd crashes with "
            "SIGILL on the Pi Zero, see docs/CROSS_COMPILE.md.")
    endif()
endif()
