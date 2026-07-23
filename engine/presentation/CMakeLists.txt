add_library(rts_presentation INTERFACE)
add_library(RTSEngine::Presentation ALIAS rts_presentation)

target_include_directories(rts_presentation INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_presentation INTERFACE RTSEngine::Foundation)
target_compile_features(rts_presentation INTERFACE cxx_std_17)
