add_library(rts_rts_desktop STATIC
    src/PlayableDesktopRuntime.cpp
)
add_library(RTSEngine::RtsDesktop ALIAS rts_rts_desktop)

target_include_directories(rts_rts_desktop PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_rts_desktop PUBLIC
    RTSEngine::Runtime
    RTSEngine::Roguelite
    RTSEngine::RtsPresentation
    RTSEngine::Assets
    RTSEngine::Audio
    RTSEngine::Render
    RTSEngine::Presentation
)
target_compile_features(rts_rts_desktop PUBLIC cxx_std_17)

if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(rts_rts_desktop PRIVATE /bigobj)
endif()
