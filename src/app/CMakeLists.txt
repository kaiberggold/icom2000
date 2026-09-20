add_executable(intercomd daemon_main.cpp)

target_compile_features(intercomd PRIVATE cxx_std_20)
target_link_libraries(intercomd PRIVATE
    icom::core
    icom::config
    icom::gpio
    icom::hw
    icom::audio
    icom::ipc
    icom::warnings
)

install(TARGETS intercomd RUNTIME DESTINATION bin)
