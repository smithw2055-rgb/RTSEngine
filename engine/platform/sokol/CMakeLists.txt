if(NOT RTSENGINE_SOKOL_INCLUDE_DIR)
    message(FATAL_ERROR
        "RTSENGINE_ENABLE_SOKOL_APP requires RTSENGINE_SOKOL_INCLUDE_DIR")
endif()

add_library(rts_platform_sokol STATIC
    src/SokolAppPlatform.cpp
)
add_library(RTSEngine::PlatformSokol ALIAS rts_platform_sokol)

target_include_directories(rts_platform_sokol
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${RTSENGINE_SOKOL_INCLUDE_DIR}
)
target_link_libraries(rts_platform_sokol PUBLIC RTSEngine::Platform)
target_compile_features(rts_platform_sokol PUBLIC cxx_std_17)
target_compile_definitions(rts_platform_sokol PUBLIC RTSENGINE_HAS_SOKOL_APP=1)
