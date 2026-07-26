add_library(rts_rts_presentation STATIC
    src/RtsPresentationExtractor.cpp
    src/RtsPresentationEvents.cpp
    src/RtsPresentationRuntime.cpp
    src/DesktopController.cpp
)
add_library(RTSEngine::RtsPresentation ALIAS rts_rts_presentation)

target_include_directories(rts_rts_presentation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_rts_presentation PUBLIC
    RTSEngine::Rts
    RTSEngine::Presentation
    RTSEngine::Platform
)
target_compile_features(rts_rts_presentation PUBLIC cxx_std_17)
