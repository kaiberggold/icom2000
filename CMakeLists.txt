cmake_minimum_required(VERSION 3.25)

project(icom2000
    DESCRIPTION "Home intercom daemon: old-telephone + TCM1171 SLIC + Codec Zero on a Raspberry Pi Zero"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Real GPIO access (libgpiod) vs. the in-process mock backend -- see
# docs/ARCHITECTURE.md "GPIO abstraction". Off by default so a plain
# `cmake -B build && cmake --build build` on a dev laptop with no GPIO chip
# and no libgpiod installed just works; the pi0-release preset turns it on.
option(ICOM_WITH_LIBGPIOD "Build the libgpiod-backed GPIO backend for target hardware" OFF)
option(ICOM_BUILD_TESTS "Build the unit test suite" ON)

# icom::core::LoopThread wraps std::jthread -- glibc's libstdc++ needs
# -lpthread linked in for the actual thread creation syscalls underneath
# std::thread/std::jthread, even though nothing here calls pthread_*
# directly.
find_package(Threads REQUIRED)

# icom::core::Logger writes straight to the systemd journal via
# sd_journal_send() (see docs/ARCHITECTURE.md "Logging") -- a deliberate,
# accepted portability tradeoff: this makes the whole project depend on
# systemd, which is fine since the only deployment target (Raspberry Pi
# OS) is itself systemd-based. Unlike libgpiod (src/gpio/CMakeLists.txt),
# there's no "build it from source for the target" option here -- see
# docs/CROSS_COMPILE.md -- so a cross build needs libsystemd-dev already
# present in ICOM_PI_SYSROOT.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# Shared warning flags as an INTERFACE target rather than sprinkled into
# every module's CMakeLists, so tightening them later is a one-line change.
add_library(icom_warnings INTERFACE)
add_library(icom::warnings ALIAS icom_warnings)
target_compile_options(icom_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
)

add_subdirectory(src/core)
add_subdirectory(src/config)
add_subdirectory(src/gpio)
add_subdirectory(src/hw)
add_subdirectory(src/audio)
add_subdirectory(src/ipc)
add_subdirectory(src/app)
add_subdirectory(src/cli)

if(ICOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
