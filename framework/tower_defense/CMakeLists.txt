add_library(rts_engine_tower_defense STATIC
    src/Simulation.cpp
)
add_library(RTSEngine::TowerDefense ALIAS rts_engine_tower_defense)

target_include_directories(rts_engine_tower_defense PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(rts_engine_tower_defense PUBLIC
    RTSEngine::Rts
)
target_compile_features(rts_engine_tower_defense PUBLIC cxx_std_17)
