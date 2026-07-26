add_library(rts_engine_ecs INTERFACE)
add_library(RTSEngine::Ecs ALIAS rts_engine_ecs)

target_include_directories(rts_engine_ecs
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_engine_ecs
    INTERFACE
        RTSEngine::Foundation
)

target_compile_features(rts_engine_ecs INTERFACE cxx_std_17)
