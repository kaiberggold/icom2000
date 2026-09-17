add_library(icom_ipc STATIC
    src/protocol.cpp
    src/control_server.cpp
)
add_library(icom::ipc ALIAS icom_ipc)

target_include_directories(icom_ipc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(icom_ipc PUBLIC cxx_std_20)
target_link_libraries(icom_ipc PUBLIC icom::core PRIVATE icom::warnings)
