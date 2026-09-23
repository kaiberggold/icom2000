add_executable(icom_tests
    gpio_and_event_loop_tests.cpp
)
target_link_libraries(icom_tests PRIVATE icom::core icom::gpio icom::warnings)

add_test(NAME icom_tests COMMAND icom_tests)

add_executable(icom_logger_tests
    logger_tests.cpp
)
target_link_libraries(icom_logger_tests PRIVATE icom::core icom::warnings)

add_test(NAME icom_logger_tests COMMAND icom_logger_tests)

add_executable(icom_config_tests
    config_tests.cpp
)
target_link_libraries(icom_config_tests PRIVATE icom::config icom::warnings)

add_test(NAME icom_config_tests COMMAND icom_config_tests)

add_executable(icom_hw_tests
    status_led_tests.cpp
)
target_link_libraries(icom_hw_tests PRIVATE icom::core icom::gpio icom::hw icom::warnings)

add_test(NAME icom_hw_tests COMMAND icom_hw_tests)

add_executable(icom_pwm_tests
    pwm_tests.cpp
)
target_link_libraries(icom_pwm_tests PRIVATE icom::core icom::gpio icom::hw icom::warnings)

add_test(NAME icom_pwm_tests COMMAND icom_pwm_tests)

add_executable(icom_audio_tests
    alsa_engine_tests.cpp
)
target_link_libraries(icom_audio_tests PRIVATE icom::audio icom::config icom::warnings)

add_test(NAME icom_audio_tests COMMAND icom_audio_tests)

# Enforces the "future-proofing for the network extension" architecture
# invariants (docs/ARCHITECTURE.md "Architecture invariants") on every
# `ctest` run, not just when someone remembers to run the script by hand.
add_test(
    NAME architecture_invariants
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/check_architecture_invariants.sh"
)
