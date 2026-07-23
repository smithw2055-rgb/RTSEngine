add_library(rts_rts_presentation INTERFACE)
add_library(RTSEngine::RtsPresentation ALIAS rts_rts_presentation)

target_include_directories(rts_rts_presentation INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_rts_presentation INTERFACE
    RTSEngine::Rts
    RTSEngine::Presentation
)
target_compile_features(rts_rts_presentation INTERFACE cxx_std_17)
