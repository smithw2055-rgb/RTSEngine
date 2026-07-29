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
option(RTSENGINE_ENABLE_REALSCRIPT
       "Build the optional RealScript scripting runtime" OFF)
set(RTSENGINE_SOKOL_INCLUDE_DIR "" CACHE PATH
    "Directory containing pinned Sokol app, gfx, glue and log headers")
set(RTSENGINE_REALSCRIPT_SOURCE_DIR "" CACHE PATH
    "Optional local RealScript source tree; when empty FetchContent uses the pinned revision")
set(RTSENGINE_REALSCRIPT_GIT_REPOSITORY
    "https://github.com/smithw2055-rgb/RealScript.git" CACHE STRING
    "RealScript repository used by FetchContent")
set(RTSENGINE_REALSCRIPT_GIT_TAG
    "e07b519acd3935def0f6d1079e3d366472fa30b5" CACHE STRING
    "Pinned RealScript revision validated for RTSEngine scripting")

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

if(RTSENGINE_ENABLE_REALSCRIPT)
    set(REALSCRIPT_BUILD_TESTS OFF CACHE BOOL
        "RTSEngine owns the integration test boundary" FORCE)
    set(REALSCRIPT_ENABLE_TOOLCHAIN_JIT_TESTS OFF CACHE BOOL
        "Toolchain JIT is not part of RTSEngine scripting" FORCE)
    set(REALSCRIPT_INSTALL_SDK OFF CACHE BOOL
        "RTSEngine does not install the nested RealScript SDK" FORCE)

    if(RTSENGINE_REALSCRIPT_SOURCE_DIR)
        if(NOT EXISTS "${RTSENGINE_REALSCRIPT_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "RTSENGINE_REALSCRIPT_SOURCE_DIR must contain RealScript/CMakeLists.txt")
        endif()
        add_subdirectory(
            "${RTSENGINE_REALSCRIPT_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/_deps/realscript-build"
            EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(
            realscript
            GIT_REPOSITORY "${RTSENGINE_REALSCRIPT_GIT_REPOSITORY}"
            GIT_TAG "${RTSENGINE_REALSCRIPT_GIT_TAG}"
            GIT_SHALLOW FALSE)
        FetchContent_MakeAvailable(realscript)
    endif()

    if(NOT TARGET RealScript::GameSdk)
        message(FATAL_ERROR
            "The pinned RealScript revision does not expose RealScript::GameSdk")
    endif()
endif()

add_subdirectory(engine/foundation)
add_subdirectory(engine/platform)
add_subdirectory(engine/assets)
if(RTSENGINE_ENABLE_REALSCRIPT)
    add_subdirectory(engine/scripting)
endif()
add_subdirectory(engine/audio)
add_subdirectory(engine/render)
add_subdirectory(engine/presentation)
add_subdirectory(engine/sim)
add_subdirectory(engine/network)
add_subdirectory(engine/runtime)
add_subdirectory(engine/ecs)
add_subdirectory(engine/navigation)
add_subdirectory(framework/rts)
if(RTSENGINE_ENABLE_REALSCRIPT)
    add_subdirectory(framework/rts_scripting)
endif()
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
    add_subdirectory(apps/multiplayer_demo)
endif()

if(RTSENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(RTSENGINE_ENABLE_REALSCRIPT)
        add_subdirectory(tests/rs2)
    endif()
endif()

if(RTSENGINE_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()
