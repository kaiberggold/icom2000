add_library(icom_gpio STATIC
    src/backend_factory.cpp
    src/mock/mock_backend.cpp
)
add_library(icom::gpio ALIAS icom_gpio)

target_include_directories(icom_gpio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(icom_gpio PUBLIC cxx_std_20)
target_link_libraries(icom_gpio PUBLIC icom::core PRIVATE icom::warnings)

if(ICOM_WITH_LIBGPIOD)
    target_sources(icom_gpio PRIVATE src/gpiod/gpiod_backend.cpp)
    target_compile_definitions(icom_gpio PRIVATE ICOM_WITH_LIBGPIOD)

    # Two ways to get libgpiod for the target, see docs/CROSS_COMPILE.md:
    #   ON  (default): cross-compile it from source ourselves, via meson,
    #       using the exact same compiler this project is being built
    #       with. No prebuilt armv6 libgpiod .deb exists anywhere -- Debian/
    #       Ubuntu only ship it for their own (ARMv7) armhf baseline -- so
    #       this is the only way to get it without a full Raspberry Pi OS
    #       sysroot that already has it built.
    #   OFF: look for an already-installed libgpiod via pkg-config against
    #       ICOM_PI_SYSROOT (a real Raspberry Pi OS filesystem you rsynced
    #       or extracted, which already has libgpiod-dev installed on it).
    option(ICOM_LIBGPIOD_BUILD_FROM_SOURCE
        "Cross-compile libgpiod from source instead of requiring it in ICOM_PI_SYSROOT" ON)

    if(ICOM_LIBGPIOD_BUILD_FROM_SOURCE)
        include(cmake/BuildLibgpiodFromSource.cmake)
        icom_build_libgpiod_from_source()
        target_link_libraries(icom_gpio PRIVATE icom::libgpiod_cxx)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod>=2.0)
        # The C library and its C++ bindings are two separate pkg-config
        # modules (libgpiodcxx.pc, not part of libgpiod.pc) -- easy to
        # miss since <gpiod.hpp> lives in the same source tree, but it's
        # a genuinely separate library your sysroot needs to have too.
        pkg_check_modules(GPIODCXX REQUIRED IMPORTED_TARGET libgpiodcxx>=2.0)
        target_link_libraries(icom_gpio PRIVATE PkgConfig::GPIODCXX PkgConfig::GPIOD)
    endif()
endif()
