add_library(rts_engine_rts INTERFACE)
add_library(RTSEngine::Rts ALIAS rts_engine_rts)

target_include_directories(rts_engine_rts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rts_engine_rts INTERFACE RTSEngine::Foundation RTSEngine::Sim RTSEngine::Ecs)
target_compile_features(rts_engine_rts INTERFACE cxx_std_17)
