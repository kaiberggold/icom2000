add_library(icom_audio STATIC
    src/null_audio_engine.cpp
    src/alsa_audio_engine.cpp
)
add_library(icom::audio ALIAS icom_audio)

target_include_directories(icom_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(icom_audio PUBLIC cxx_std_20)
target_link_libraries(icom_audio PUBLIC icom::core icom::config PRIVATE icom::warnings)

# Two ways to get libasound, mirroring src/gpio's choice for libgpiod (see
# docs/CROSS_COMPILE.md):
#   ON  (default when cross-compiling): build alsa-lib from source with
#       this project's own compiler -- a self-built cross toolchain has no
#       Raspberry Pi OS libraries to link against.
#   OFF (default on host-dev): the system's libasound via pkg-config --
#       the dev host's libasound2-dev, or ICOM_PI_SYSROOT's in a cross build.
if(CMAKE_CROSSCOMPILING)
    set(_icom_alsa_from_source_default ON)
else()
    set(_icom_alsa_from_source_default OFF)
endif()
option(ICOM_ALSA_BUILD_FROM_SOURCE
    "Cross-compile alsa-lib from source instead of using an installed libasound"
    ${_icom_alsa_from_source_default})

if(ICOM_ALSA_BUILD_FROM_SOURCE)
    include(cmake/BuildAlsaLibFromSource.cmake)
    icom_build_alsa_lib_from_source()
    target_link_libraries(icom_audio PRIVATE icom::alsa_lib)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    target_link_libraries(icom_audio PRIVATE PkgConfig::ALSA)
endif()
