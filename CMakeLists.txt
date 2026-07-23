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
set(RTSENGINE_SOKOL_INCLUDE_DIR "" CACHE PATH
    "Directory containing sokol_gfx.h for the optional Sokol backend")

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
add_subdirectory(engine/ecs)
add_subdirectory(framework/rts)
add_subdirectory(framework/rts_presentation)
add_subdirectory(framework/tower_defense)
add_subdirectory(framework/roguelite)

if(RTSENGINE_BUILD_EXAMPLES)
    add_subdirectory(apps/headless_demo)
    add_subdirectory(apps/tower_defense_demo)
    add_subdirectory(apps/roguelite_demo)
    add_subdirectory(apps/scale_benchmark)
endif()

if(RTSENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(RTSENGINE_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()
