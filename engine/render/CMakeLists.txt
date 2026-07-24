add_library(rts_render STATIC
    src/NullRenderDevice.cpp
)
add_library(RTSEngine::Render ALIAS rts_render)

target_include_directories(rts_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_render PUBLIC
    RTSEngine::Foundation
    RTSEngine::Platform
)
target_compile_features(rts_render PUBLIC cxx_std_17)

if(RTSENGINE_ENABLE_SOKOL_RENDERER)
    add_subdirectory(sokol)
endif()
