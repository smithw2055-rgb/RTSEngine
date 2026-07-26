add_library(rts_audio STATIC
    src/NullAudioDevice.cpp
)
add_library(RTSEngine::Audio ALIAS rts_audio)

target_include_directories(rts_audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_audio PUBLIC RTSEngine::Foundation)
target_compile_features(rts_audio PUBLIC cxx_std_17)
