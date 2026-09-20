add_library(icom_audio STATIC
    src/null_audio_engine.cpp
)
add_library(icom::audio ALIAS icom_audio)

target_include_directories(icom_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(icom_audio PUBLIC cxx_std_20)
target_link_libraries(icom_audio PUBLIC icom::core icom::config PRIVATE icom::warnings)
