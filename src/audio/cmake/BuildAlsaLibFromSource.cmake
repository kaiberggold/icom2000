# Cross-compiles ALSA's alsa-lib (libasound) from source via its autotools
# build, and defines an icom::alsa_lib imported target for it -- the same
# approach, for the same reason, as src/gpio/cmake/BuildLibgpiodFromSource.cmake:
# a self-built cross toolchain ships no Raspberry Pi OS libraries at all, so
# the only way to get libasound without a Pi sysroot is to build it with the
# same compiler this project is being built with. See docs/CROSS_COMPILE.md
# "Building alsa-lib from source".
#
# Fetched as a git tag from the project's GitHub repository rather than as a
# release tarball: git checkouts don't ship a generated `configure`, hence
# the autoreconf step (and autoconf/automake/libtool on the host).
#
# --prefix=/usr on purpose: libasound compiles its config directory
# (/usr/share/alsa, where alsa.conf lives) into the library, and that has to
# be the Pi's own. The actual install goes to a staging directory via
# DESTDIR instead. Static, like libgpiod, so deploying stays "copy one
# binary".
include(ExternalProject)

function(icom_build_alsa_lib_from_source)
    find_program(ICOM_AUTORECONF_EXECUTABLE autoreconf)
    find_program(ICOM_AUTOMAKE_EXECUTABLE automake)
    find_program(ICOM_LIBTOOLIZE_EXECUTABLE libtoolize)
    find_program(ICOM_MAKE_EXECUTABLE NAMES gmake make)

    set(_icom_missing_tools "")
    foreach(_icom_tool autoreconf automake libtoolize make)
        string(TOUPPER "${_icom_tool}" _icom_tool_upper)
        if(NOT ICOM_${_icom_tool_upper}_EXECUTABLE)
            list(APPEND _icom_missing_tools "${_icom_tool}")
        endif()
    endforeach()

    if(_icom_missing_tools)
        list(JOIN _icom_missing_tools "', '" _icom_missing_tools_joined)
        message(FATAL_ERROR
            "ICOM_ALSA_BUILD_FROM_SOURCE=ON needs '${_icom_missing_tools_joined}' "
            "on the HOST (not found on PATH) to generate and drive alsa-lib's "
            "build. Install with e.g. 'apt install autoconf automake libtool make', "
            "or set -DICOM_ALSA_BUILD_FROM_SOURCE=OFF and provide libasound via "
            "ICOM_PI_SYSROOT instead.")
    endif()

    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "ICOM_ALSA_BUILD_FROM_SOURCE is for cross-compiling only -- the "
            "host-dev preset uses the dev host's own libasound2-dev.")
    endif()

    set(ICOM_ALSA_LIB_GIT_URL "https://github.com/alsa-project/alsa-lib.git" CACHE STRING
        "Repository to fetch alsa-lib from when building it from source")
    # Raspberry Pi OS Bookworm's version. Keep this matched to the Pi's own
    # libasound2 (`dpkg -s libasound2`): the library reads the Pi's
    # /usr/share/alsa/alsa.conf at runtime, so a library older than that
    # config could meet syntax it doesn't know.
    set(ICOM_ALSA_LIB_GIT_TAG "v1.2.8" CACHE STRING
        "alsa-lib tag/commit to build when building it from source")
    mark_as_advanced(ICOM_ALSA_LIB_GIT_URL ICOM_ALSA_LIB_GIT_TAG)

    set(_icom_alsa_root "${CMAKE_BINARY_DIR}/third_party/alsa-lib")
    set(_icom_alsa_stage "${_icom_alsa_root}/install")
    set(_icom_alsa_library "${_icom_alsa_stage}/usr/lib/libasound.a")

    cmake_host_system_information(RESULT _icom_jobs QUERY NUMBER_OF_LOGICAL_CORES)

    ExternalProject_Add(icom_alsa_lib_ext
        GIT_REPOSITORY "${ICOM_ALSA_LIB_GIT_URL}"
        GIT_TAG "${ICOM_ALSA_LIB_GIT_TAG}"
        GIT_SHALLOW TRUE
        # Without this, the git update step runs on every build and drags
        # autoreconf, configure, make and install along with it (~10 s, plus
        # a relink of everything using libasound). A pinned tag never needs
        # updating; changing ICOM_ALSA_LIB_GIT_TAG still re-fetches.
        UPDATE_DISCONNECTED TRUE
        PREFIX "${_icom_alsa_root}"
        INSTALL_DIR "${_icom_alsa_stage}"
        CONFIGURE_COMMAND
            "${CMAKE_COMMAND}" -E chdir <SOURCE_DIR> "${ICOM_AUTORECONF_EXECUTABLE}" -fi
        COMMAND <SOURCE_DIR>/configure
            --host=arm-linux-gnueabihf
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            "RANLIB=${CMAKE_RANLIB}"
            "NM=${CMAKE_NM}"
            "CFLAGS=${ICOM_TARGET_CPU_FLAGS} -O2"
            --prefix=/usr
            --enable-static
            --disable-shared
            --disable-python
            --disable-ucm
            --disable-topology
            --disable-rawmidi
            --disable-hwdep
            --disable-seq
        BUILD_COMMAND "${ICOM_MAKE_EXECUTABLE}" -j${_icom_jobs}
        INSTALL_COMMAND "${ICOM_MAKE_EXECUTABLE}" install DESTDIR=<INSTALL_DIR>
        BUILD_BYPRODUCTS "${_icom_alsa_library}"
        LOG_CONFIGURE TRUE
        LOG_BUILD TRUE
        LOG_INSTALL TRUE
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_INSTALL TRUE
    )

    # Must exist at generate time for INTERFACE_INCLUDE_DIRECTORIES, before
    # the install step has put the real headers there -- see the libgpiod
    # recipe for the same workaround.
    file(MAKE_DIRECTORY "${_icom_alsa_stage}/usr/include")

    add_library(icom_alsa_lib STATIC IMPORTED GLOBAL)
    set_target_properties(icom_alsa_lib PROPERTIES
        IMPORTED_LOCATION "${_icom_alsa_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_icom_alsa_stage}/usr/include"
        # alsa.pc's Libs.private -- what a static libasound needs on top.
        INTERFACE_LINK_LIBRARIES "m;dl;pthread;rt"
    )
    add_dependencies(icom_alsa_lib icom_alsa_lib_ext)
    add_library(icom::alsa_lib ALIAS icom_alsa_lib)
endfunction()
