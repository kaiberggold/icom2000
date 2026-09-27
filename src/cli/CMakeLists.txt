add_executable(intercomctl ctl_main.cpp)
target_compile_features(intercomctl PRIVATE cxx_std_20)
target_link_libraries(intercomctl PRIVATE icom::warnings)

add_executable(icom-audiotest audiotest_main.cpp)
target_compile_features(icom-audiotest PRIVATE cxx_std_20)
target_link_libraries(icom-audiotest PRIVATE icom::audio icom::config icom::core icom::warnings)

install(TARGETS intercomctl icom-audiotest RUNTIME DESTINATION bin)
