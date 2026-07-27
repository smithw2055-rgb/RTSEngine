add_library(rts_navigation INTERFACE)
add_library(RTSEngine::Navigation ALIAS rts_navigation)

target_include_directories(rts_navigation INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_navigation INTERFACE
    RTSEngine::Foundation
)

target_compile_features(rts_navigation INTERFACE cxx_std_17)
