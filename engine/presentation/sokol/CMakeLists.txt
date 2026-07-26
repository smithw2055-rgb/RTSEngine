if(NOT RTSENGINE_SOKOL_INCLUDE_DIR)
    message(FATAL_ERROR
        "RTSENGINE_ENABLE_SOKOL_RENDERER requires RTSENGINE_SOKOL_INCLUDE_DIR")
endif()

add_library(rts_presentation_sokol STATIC
    src/SokolFixed2DShader.cpp
)
add_library(RTSEngine::PresentationSokol ALIAS rts_presentation_sokol)

target_include_directories(rts_presentation_sokol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${RTSENGINE_SOKOL_INCLUDE_DIR}
)

target_link_libraries(rts_presentation_sokol PUBLIC
    RTSEngine::Presentation
    RTSEngine::RenderSokol
)
target_compile_features(rts_presentation_sokol PUBLIC cxx_std_17)
target_compile_definitions(rts_presentation_sokol PUBLIC
    RTSENGINE_HAS_SOKOL_PRESENTATION=1
)
