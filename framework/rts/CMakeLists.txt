add_library(rts_engine_rts STATIC
    src/Simulation.cpp
)
add_library(RTSEngine::Rts ALIAS rts_engine_rts)

target_include_directories(rts_engine_rts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(rts_engine_rts PUBLIC
    RTSEngine::Foundation
    RTSEngine::Sim
    RTSEngine::Ecs
    RTSEngine::Navigation
)
target_compile_features(rts_engine_rts PUBLIC cxx_std_17)

if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(rts_engine_rts PRIVATE /bigobj)
endif()
