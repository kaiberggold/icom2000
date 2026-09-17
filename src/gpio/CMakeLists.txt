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
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod>=2.0)

    target_sources(icom_gpio PRIVATE src/gpiod/gpiod_backend.cpp)
    target_compile_definitions(icom_gpio PRIVATE ICOM_WITH_LIBGPIOD)
    target_link_libraries(icom_gpio PRIVATE PkgConfig::GPIOD)
endif()
