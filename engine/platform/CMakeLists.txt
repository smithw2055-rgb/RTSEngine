add_library(rts_platform STATIC
    src/NullPlatform.cpp
    src/InputState.cpp
)
add_library(RTSEngine::Platform ALIAS rts_platform)

target_include_directories(rts_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_platform PUBLIC RTSEngine::Foundation)
target_compile_features(rts_platform PUBLIC cxx_std_17)

if(RTSENGINE_ENABLE_SOKOL_APP)
    add_subdirectory(sokol)
endif()
