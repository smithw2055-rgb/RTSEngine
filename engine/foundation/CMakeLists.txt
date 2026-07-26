add_library(rts_foundation INTERFACE)
add_library(RTSEngine::Foundation ALIAS rts_foundation)

target_include_directories(rts_foundation INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(rts_foundation INTERFACE cxx_std_17)
