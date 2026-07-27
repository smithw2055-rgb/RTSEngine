cmake_minimum_required(VERSION 3.20)
project(RTSEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RTSENGINE_BUILD_TESTS "Build RTSEngine tests" ON)
option(RTSENGINE_BUILD_EXAMPLES "Build RTSEngine examples" ON)
option(RTSENGINE_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(RTSENGINE_BUILD_FUZZERS
       "Build Clang libFuzzer persistence targets" OFF)
option(RTSENGINE_ENABLE_SOKOL_RENDERER
       "Build the optional Sokol render backend" OFF)
option(RTSENGINE_ENABLE_SOKOL_APP
       "Build the optional Sokol desktop platform and application" OFF)
set(RTSENGINE_SOKOL_INCLUDE_DIR "" CACHE PATH
    "Directory containing pinned Sokol app, gfx, glue and log headers")

if(RTSENGINE_ENABLE_SOKOL_APP AND NOT RTSENGINE_ENABLE_SOKOL_RENDERER)
    message(FATAL_ERROR
        "RTSENGINE_ENABLE_SOKOL_APP requires RTSENGINE_ENABLE_SOKOL_RENDERER")
endif()
if(RTSENGINE_ENABLE_SOKOL_RENDERER AND
   NOT EXISTS "${RTSENGINE_SOKOL_INCLUDE_DIR}/sokol_gfx.h")
    message(FATAL_ERROR
        "RTSENGINE_ENABLE_SOKOL_RENDERER requires sokol_gfx.h")
endif()
if(RTSENGINE_ENABLE_SOKOL_APP)
    foreach(header sokol_app.h sokol_gfx.h sokol_glue.h sokol_log.h)
        if(NOT EXISTS "${RTSENGINE_SOKOL_INCLUDE_DIR}/${header}")
            message(FATAL_ERROR
                "RTSENGINE_ENABLE_SOKOL_APP requires ${header}")
        endif()
    endforeach()
endif()

if(RTSENGINE_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(FATAL_ERROR
            "RTSENGINE_ENABLE_SANITIZERS currently requires GCC or Clang")
    endif()
endif()

add_subdirectory(engine/foundation)
add_subdirectory(engine/platform)
add_subdirectory(engine/assets)
add_subdirectory(engine/audio)
add_subdirectory(engine/render)
add_subdirectory(engine/presentation)
add_subdirectory(engine/sim)
add_subdirectory(engine/network)
add_subdirectory(engine/runtime)
add_subdirectory(engine/ecs)
add_subdirectory(engine/navigation)
add_subdirectory(framework/rts)
add_subdirectory(framework/rts_presentation)
add_subdirectory(framework/rts_desktop)
add_subdirectory(framework/tower_defense)
add_subdirectory(framework/roguelite)

if(RTSENGINE_BUILD_EXAMPLES)
    add_subdirectory(apps/headless_demo)
    add_subdirectory(apps/tower_defense_demo)
    add_subdirectory(apps/roguelite_demo)
    add_subdirectory(apps/scale_benchmark)
    add_subdirectory(apps/desktop_demo)
endif()

if(RTSENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(RTSENGINE_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()
