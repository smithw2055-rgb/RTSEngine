add_library(rts_assets INTERFACE)
add_library(RTSEngine::Assets ALIAS rts_assets)

target_include_directories(rts_assets INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_assets INTERFACE RTSEngine::Foundation)
target_compile_features(rts_assets INTERFACE cxx_std_17)
