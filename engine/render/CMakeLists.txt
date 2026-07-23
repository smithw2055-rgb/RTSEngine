add_library(rts_render INTERFACE)
add_library(RTSEngine::Render ALIAS rts_render)

target_include_directories(rts_render INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_render INTERFACE
    RTSEngine::Foundation
    RTSEngine::Platform
)
target_compile_features(rts_render INTERFACE cxx_std_17)

if(RTSENGINE_ENABLE_SOKOL_RENDERER)
    add_subdirectory(sokol)
endif()
