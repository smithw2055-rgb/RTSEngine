if(NOT RTSENGINE_ENABLE_SOKOL_APP OR NOT RTSENGINE_ENABLE_SOKOL_RENDERER)
    return()
endif()

add_library(rts_desktop_sokol_composition STATIC
    SokolComposition.cpp
)
add_library(RTSEngine::DesktopSokolComposition ALIAS
    rts_desktop_sokol_composition)
target_link_libraries(rts_desktop_sokol_composition PUBLIC
    RTSEngine::RtsDesktop
    RTSEngine::PlatformSokol
    RTSEngine::RenderSokol
    RTSEngine::PresentationSokol
    RTSEngine::Audio
)
target_include_directories(rts_desktop_sokol_composition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RTSENGINE_SOKOL_INCLUDE_DIR}
)
target_compile_features(rts_desktop_sokol_composition PUBLIC cxx_std_17)

add_executable(rts_desktop_demo SokolEntry.cpp)
target_link_libraries(rts_desktop_demo PRIVATE
    RTSEngine::DesktopSokolComposition
)
target_include_directories(rts_desktop_demo PRIVATE
    ${RTSENGINE_SOKOL_INCLUDE_DIR}
)
target_compile_features(rts_desktop_demo PRIVATE cxx_std_17)

if(WIN32)
    target_compile_definitions(rts_desktop_demo PRIVATE
        SOKOL_GLCORE
        SOKOL_WIN32_FORCE_MAIN
    )
elseif(APPLE)
    target_compile_definitions(rts_desktop_demo PRIVATE SOKOL_GLCORE)
    target_link_libraries(rts_desktop_demo PRIVATE
        "-framework AppKit"
        "-framework QuartzCore"
        "-framework OpenGL"
    )
elseif(UNIX)
    target_compile_definitions(rts_desktop_demo PRIVATE SOKOL_GLCORE)
    target_link_libraries(rts_desktop_demo PRIVATE
        X11 Xi Xcursor GL dl pthread m
    )
endif()
