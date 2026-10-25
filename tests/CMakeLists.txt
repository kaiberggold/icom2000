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

add_executable(icom_tcm1171_tests
    tcm1171_tests.cpp
)
target_link_libraries(icom_tcm1171_tests PRIVATE icom::core icom::gpio icom::hw icom::warnings)

add_test(NAME icom_tcm1171_tests COMMAND icom_tcm1171_tests)

add_executable(icom_audio_tests
    alsa_engine_tests.cpp
)
target_link_libraries(icom_audio_tests PRIVATE icom::audio icom::config icom::warnings)

add_test(NAME icom_audio_tests COMMAND icom_audio_tests)

# The real intercomd + intercomctl, end to end, with stand-in audio
# devices. Python only drives them; skipped (with a note) without it.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME daemon_integration
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/daemon_integration_test.py"
                $<TARGET_FILE:intercomd> $<TARGET_FILE:intercomctl> "${CMAKE_SOURCE_DIR}/config/icom2000.conf"
    )
else()
    message(STATUS "python3 not found -- skipping the daemon_integration test")
endif()

# Enforces the "future-proofing for the network extension" architecture
# invariants (docs/ARCHITECTURE.md "Architecture invariants") on every
# `ctest` run, not just when someone remembers to run the script by hand.
add_test(
    NAME architecture_invariants
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/check_architecture_invariants.sh"
)
