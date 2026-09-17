add_executable(intercomctl ctl_main.cpp)
target_compile_features(intercomctl PRIVATE cxx_std_20)
target_link_libraries(intercomctl PRIVATE icom::warnings)

install(TARGETS intercomctl RUNTIME DESTINATION bin)
