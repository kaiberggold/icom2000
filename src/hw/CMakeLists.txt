add_library(icom_hw STATIC
    src/bell_controller.cpp
    src/tcm1171_controller.cpp
    src/status_led.cpp
)
add_library(icom::hw ALIAS icom_hw)

target_include_directories(icom_hw PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(icom_hw PUBLIC cxx_std_20)
target_link_libraries(icom_hw PUBLIC icom::core icom::gpio PRIVATE icom::warnings)
