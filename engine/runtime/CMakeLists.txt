add_library(rts_runtime STATIC
    src/DesktopFrameLoop.cpp
)
add_library(RTSEngine::Runtime ALIAS rts_runtime)

target_include_directories(rts_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_runtime PUBLIC
    RTSEngine::Foundation
    RTSEngine::Platform
    RTSEngine::Sim
)
target_compile_features(rts_runtime PUBLIC cxx_std_17)
