cmake_minimum_required(VERSION 3.20)
project(RTSEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RTSENGINE_BUILD_TESTS "Build RTSEngine tests" ON)
option(RTSENGINE_BUILD_EXAMPLES "Build RTSEngine examples" ON)

add_subdirectory(engine/foundation)
add_subdirectory(engine/sim)

if(RTSENGINE_BUILD_EXAMPLES)
    add_subdirectory(apps/headless_demo)
endif()

if(RTSENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
