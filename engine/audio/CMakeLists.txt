add_library(rts_audio INTERFACE)
add_library(RTSEngine::Audio ALIAS rts_audio)

target_include_directories(rts_audio INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_audio INTERFACE RTSEngine::Foundation)
target_compile_features(rts_audio INTERFACE cxx_std_17)
