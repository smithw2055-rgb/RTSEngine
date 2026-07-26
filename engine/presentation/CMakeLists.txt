add_library(rts_presentation STATIC
    src/Fixed2DRenderer.cpp
    src/PresentationAssetCache.cpp
    src/PresentationEvents.cpp
    src/PresentationPlayback.cpp
    src/SpriteBatch.cpp
    src/ScreenUi.cpp
)
add_library(RTSEngine::Presentation ALIAS rts_presentation)

target_include_directories(rts_presentation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_presentation PUBLIC
    RTSEngine::Foundation
    RTSEngine::Assets
    RTSEngine::Audio
    RTSEngine::Render
)
target_compile_features(rts_presentation PUBLIC cxx_std_17)

if(RTSENGINE_ENABLE_SOKOL_RENDERER)
    add_subdirectory(sokol)
endif()
