add_library(rts_engine_roguelite INTERFACE)
add_library(RTSEngine::Roguelite ALIAS rts_engine_roguelite)

target_include_directories(rts_engine_roguelite
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_engine_roguelite
    INTERFACE
        RTSEngine::TowerDefense
)

target_compile_features(rts_engine_roguelite INTERFACE cxx_std_17)
