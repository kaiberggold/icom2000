add_library(icom_config STATIC
    src/config_file.cpp
    src/station_registry.cpp
)
add_library(icom::config ALIAS icom_config)

target_include_directories(icom_config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(icom_config PUBLIC cxx_std_20)
target_link_libraries(icom_config PRIVATE icom::warnings)
