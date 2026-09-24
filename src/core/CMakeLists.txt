add_library(icom_core STATIC
    src/logger.cpp
    src/journal.cpp
    src/event_loop.cpp
    src/signal_watcher.cpp
    src/loop_thread.cpp
)
add_library(icom::core ALIAS icom_core)

target_include_directories(icom_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(icom_core PUBLIC cxx_std_20)
target_link_libraries(icom_core PUBLIC Threads::Threads PRIVATE icom::warnings)
