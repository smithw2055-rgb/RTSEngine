add_library(rts_sim STATIC
    src/SimulationHost.cpp
)
add_library(RTSEngine::Sim ALIAS rts_sim)

target_include_directories(rts_sim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_sim PUBLIC RTSEngine::Foundation)
target_compile_features(rts_sim PUBLIC cxx_std_17)
