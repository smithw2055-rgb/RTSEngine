add_library(rts_engine_roguelite STATIC
    src/RunSimulation.cpp
)
add_library(RTSEngine::Roguelite ALIAS rts_engine_roguelite)

target_include_directories(rts_engine_roguelite PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(rts_engine_roguelite PUBLIC
    RTSEngine::TowerDefense
)
target_compile_features(rts_engine_roguelite PUBLIC cxx_std_17)

if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(rts_engine_roguelite PRIVATE /bigobj)
endif()
