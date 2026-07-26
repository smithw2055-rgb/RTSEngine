if(NOT RTSENGINE_SOKOL_INCLUDE_DIR)
    message(FATAL_ERROR
        "RTSENGINE_ENABLE_SOKOL_RENDERER requires RTSENGINE_SOKOL_INCLUDE_DIR")
endif()

add_library(rts_render_sokol STATIC
    src/SokolRenderDevice.cpp
)
add_library(RTSEngine::RenderSokol ALIAS rts_render_sokol)

target_include_directories(rts_render_sokol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${RTSENGINE_SOKOL_INCLUDE_DIR}
)

target_link_libraries(rts_render_sokol PUBLIC RTSEngine::Render)
target_compile_features(rts_render_sokol PUBLIC cxx_std_17)
target_compile_definitions(rts_render_sokol PUBLIC RTSENGINE_HAS_SOKOL=1)
