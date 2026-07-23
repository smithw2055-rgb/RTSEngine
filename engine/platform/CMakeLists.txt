add_library(rts_platform INTERFACE)
add_library(RTSEngine::Platform ALIAS rts_platform)

target_include_directories(rts_platform INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_platform INTERFACE RTSEngine::Foundation)
target_compile_features(rts_platform INTERFACE cxx_std_17)
