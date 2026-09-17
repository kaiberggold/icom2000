add_executable(icom_tests
    gpio_and_event_loop_tests.cpp
)
target_link_libraries(icom_tests PRIVATE icom::core icom::gpio icom::warnings)

add_test(NAME icom_tests COMMAND icom_tests)
