add_library(icom_audio STATIC
    src/null_audio_engine.cpp
    src/alsa_audio_engine.cpp
)
add_library(icom::audio ALIAS icom_audio)

target_include_directories(icom_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Required, not optional like libgpiod: every Linux dev host can install
# libasound2-dev, and a cross build already needs ICOM_PI_SYSROOT for
# libsystemd -- see docs/CROSS_COMPILE.md.
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

target_compile_features(icom_audio PUBLIC cxx_std_20)
target_link_libraries(icom_audio PUBLIC icom::core icom::config PRIVATE icom::warnings PkgConfig::ALSA)
