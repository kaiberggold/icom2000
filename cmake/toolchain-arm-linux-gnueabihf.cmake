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
# This file assumes you already have an arm-*-gnueabihf-{gcc,g++} that
# itself targets armv6 (e.g. built with crosstool-NG for arm1176jzf-s) --
# it verifies that assumption itself (see "Sanity-check" below) rather
# than just trusting it, and picks a *working* one over a merely-present
# one if more than one arm-*-gnueabihf toolchain is on PATH at once (the
# common case once you've built your own next to a distro package you
# never uninstalled). libgpiod cross-builds from source as part of the
# main build (src/gpio/cmake/BuildLibgpiodFromSource.cmake) by default,
# so no sysroot is needed for that specifically; ICOM_PI_SYSROOT below
# remains available for anything else that genuinely needs to match the
# target Raspberry Pi OS build exactly (see docs/CROSS_COMPILE.md).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

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

# A self-built cross toolchain (crosstool-NG, Option B) is very likely a
# much newer GCC than whatever Raspberry Pi OS itself ships -- its own
# libstdc++.so.6/libgcc_s.so.1 are a completely different build than the
# ones already installed on the target, which is exactly what a
# dynamically-linked C++ binary resolves against at runtime by default.
# Statically linking our own matching copies in removes that mismatch as
# a variable entirely, the same reasoning already applied to libgpiod
# (see src/gpio/cmake/BuildLibgpiodFromSource.cmake) -- a deploy that's
# `scp` one file, not "and hope the versions on the target line up".
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Exposed for anything else that needs to reproduce these exact target
# flags outside CMake's own compiler invocations -- e.g. src/gpio's
# ExternalProject build of libgpiod, which drives a *separate* build
# system (meson) that has no visibility into CMAKE_C_FLAGS_INIT at all.
# CACHE INTERNAL because plain `set()` here would only be visible for the
# remainder of *this* toolchain-file inclusion, not to CMakeLists.txt code
# that runs later in subdirectories.
set(ICOM_TARGET_CPU_FLAGS "${_icom_cpu_flags}" CACHE INTERNAL "Target CPU flags, for build systems CMake doesn't drive directly")

# A toolchain you build yourself doesn't have to use the generic
# "arm-linux-gnueabihf" triplet -- e.g. crosstool-NG's own
# "armv6-unknown-linux-gnueabihf" sample (see docs/CROSS_COMPILE.md
# "Option B") sets CT_ARCH_SUFFIX="v6" and CT_TARGET_VENDOR="rpi",
# producing armv6-rpi-linux-gnueabihf-gcc instead -- the "v6" comes from
# the arch suffix, not the vendor field, so it's easy to miss when
# eyeballing the sample name. Both this and the plain "arm-*" name are
# tried automatically; set ICOM_TOOLCHAIN_PREFIX explicitly if yours is
# neither, or if more than one candidate on PATH actually works and
# auto-detection picks the wrong one for your purposes.
set(ICOM_TOOLCHAIN_PREFIX "" CACHE STRING
    "Cross-toolchain binary prefix (e.g. 'armv6-rpi-linux-gnueabihf'). Auto-detected if empty.")

# --- Sanity-check option, consulted by the discovery loop below ---
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
# The probe below catches exactly that: compile+link a trivial program
# with the real flags (so crt1.o/libgcc.a get pulled in the same way they
# would for intercomd), then check the resulting binary's own
# Tag_CPU_arch rather than trusting the flags asked for -- run against
# *each candidate toolchain in turn* during discovery, not just once
# against whatever happened to be found first by name.
option(ICOM_SKIP_ARMV6_CHECK "Skip the ARMv6 toolchain capability probe (only for testing this CMake config itself on a non-ARMv6-capable toolchain -- a binary built this way should not be trusted on real Pi Zero 1.x hardware)" OFF)

# CMake re-includes this toolchain file for its own internal compiler-ABI
# try_compile() checks, each in a throwaway "CMakeScratch" project with
# its own cache -- one that never saw -DICOM_SKIP_ARMV6_CHECK or
# -DICOM_TOOLCHAIN_PREFIX, so re-running the full probing dance there
# would be redundant at best (those scratch builds are never intercomd's
# actual output) and would re-fail an already-good configure at worst if
# it picked a different, unverified candidate than the outer configure
# did. Skip probing unconditionally whenever we're inside one; the
# compiler still needs to be *found* there, just not re-verified.
if(CMAKE_BINARY_DIR MATCHES "CMakeScratch")
    set(_icom_in_try_compile TRUE)
else()
    set(_icom_in_try_compile FALSE)
endif()

separate_arguments(_icom_cpu_flags_list UNIX_COMMAND "${_icom_cpu_flags}")

# --- Discover a toolchain, preferring one that actually passes the probe ---
#
# Tries each candidate prefix in turn; for each, if a *-gcc exists, it's
# compile+readelf probed before being accepted (unless probing is
# disabled below), so a working purpose-built toolchain is found even
# when a broken one (e.g. the distro package) also happens to be on PATH
# and would otherwise "win" by being searched first.
if(ICOM_TOOLCHAIN_PREFIX)
    set(_icom_candidate_prefixes "${ICOM_TOOLCHAIN_PREFIX}")
else()
    set(_icom_candidate_prefixes arm-linux-gnueabihf armv6-rpi-linux-gnueabihf arm-rpi-linux-gnueabihf)
endif()

set(_icom_probe_report "")

foreach(_icom_candidate_prefix ${_icom_candidate_prefixes})
    unset(_icom_candidate_gcc CACHE)
    find_program(_icom_candidate_gcc NAMES ${_icom_candidate_prefix}-gcc)
    if(NOT _icom_candidate_gcc)
        string(APPEND _icom_probe_report "  ${_icom_candidate_prefix}-gcc: not found on PATH\n")
        continue()
    endif()

    if(ICOM_SKIP_ARMV6_CHECK OR _icom_in_try_compile)
        set(_icom_toolchain_prefix "${_icom_candidate_prefix}")
        set(CMAKE_C_COMPILER "${_icom_candidate_gcc}")
        find_program(CMAKE_CXX_COMPILER NAMES ${_icom_candidate_prefix}-g++)
        break()
    endif()

    unset(_icom_candidate_readelf CACHE)
    find_program(_icom_candidate_readelf NAMES ${_icom_candidate_prefix}-readelf)
    if(NOT _icom_candidate_readelf)
        message(WARNING
            "${_icom_candidate_prefix}-readelf not found -- cannot verify this "
            "candidate's ARMv6 output, accepting it unchecked. If intercomd "
            "crashes with SIGILL on the Pi Zero, see docs/CROSS_COMPILE.md.")
        set(_icom_toolchain_prefix "${_icom_candidate_prefix}")
        set(CMAKE_C_COMPILER "${_icom_candidate_gcc}")
        find_program(CMAKE_CXX_COMPILER NAMES ${_icom_candidate_prefix}-g++)
        break()
    endif()

    set(_icom_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/icom_armv6_probe")
    file(MAKE_DIRECTORY "${_icom_probe_dir}")
    file(WRITE "${_icom_probe_dir}/probe.c" "int main(void) { return 0; }\n")

    execute_process(
        COMMAND "${_icom_candidate_gcc}" ${_icom_cpu_flags_list} -o "${_icom_probe_dir}/probe" "${_icom_probe_dir}/probe.c"
        RESULT_VARIABLE _icom_probe_rc
        OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT _icom_probe_rc EQUAL 0)
        string(APPEND _icom_probe_report
            "  ${_icom_candidate_prefix}-gcc: found, but a trivial program with the "
            "target flags ('${_icom_cpu_flags}') failed to compile/link\n")
        continue()
    endif()

    execute_process(
        COMMAND "${_icom_candidate_readelf}" -A "${_icom_probe_dir}/probe"
        OUTPUT_VARIABLE _icom_probe_attrs
        ERROR_QUIET
    )

    if(_icom_probe_attrs MATCHES "Tag_CPU_arch: v6")
        set(_icom_toolchain_prefix "${_icom_candidate_prefix}")
        set(CMAKE_C_COMPILER "${_icom_candidate_gcc}")
        find_program(CMAKE_CXX_COMPILER NAMES ${_icom_candidate_prefix}-g++)
        break()
    else()
        string(APPEND _icom_probe_report
            "  ${_icom_candidate_prefix}-gcc: found, but does NOT produce genuine "
            "ARMv6 binaries -- readelf -A on a trivial program compiled with it "
            "reports:\n"
            "${_icom_probe_attrs}\n"
            "    (This is the known Debian/Ubuntu gcc-arm-linux-gnueabihf "
            "limitation: crt1.o/libgcc.a built for ARMv7-A only, regardless of "
            "-march. See docs/CROSS_COMPILE.md.)\n")
    endif()
endforeach()

if(NOT CMAKE_C_COMPILER)
    if(ICOM_TOOLCHAIN_PREFIX)
        message(FATAL_ERROR
            "ICOM_TOOLCHAIN_PREFIX=${ICOM_TOOLCHAIN_PREFIX} did not yield a "
            "working ARMv6 toolchain:\n${_icom_probe_report}\n"
            "See docs/CROSS_COMPILE.md. To bypass verification entirely (e.g. "
            "to test this CMake configuration itself, NOT to produce a binary "
            "for real hardware), reconfigure with -DICOM_SKIP_ARMV6_CHECK=ON.")
    else()
        message(FATAL_ERROR
            "No working ARMv6 toolchain found automatically. Tried:\n"
            "${_icom_probe_report}\n"
            "If you have more than one arm-*-gnueabihf toolchain on PATH at "
            "once (e.g. a distro package installed alongside a purpose-built "
            "one), that alone shouldn't cause this -- a working one is "
            "preferred automatically. If NONE of them work, see "
            "docs/CROSS_COMPILE.md for how to get one. If your toolchain uses "
            "a prefix this file doesn't try by default, set "
            "-DICOM_TOOLCHAIN_PREFIX=<prefix>. To bypass verification entirely "
            "(e.g. to test this CMake configuration itself, NOT to produce a "
            "binary for real hardware), reconfigure with "
            "-DICOM_SKIP_ARMV6_CHECK=ON.")
    endif()
endif()

if(NOT CMAKE_CXX_COMPILER)
    message(FATAL_ERROR "${_icom_toolchain_prefix}-gcc was found and verified, but ${_icom_toolchain_prefix}-g++ was not.")
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

# pkg-config must resolve .pc files from the sysroot, not the host.
set(ENV{PKG_CONFIG_LIBDIR} "${ICOM_PI_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${ICOM_PI_SYSROOT}/usr/lib/pkgconfig:${ICOM_PI_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${ICOM_PI_SYSROOT}")
