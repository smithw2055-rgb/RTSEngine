add_library(rts_scripting STATIC
    src/ScriptBundle.cpp
    src/RealScriptHost.cpp
)
add_library(RTSEngine::Scripting ALIAS rts_scripting)

set_target_properties(rts_scripting PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(rts_scripting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_scripting PUBLIC
    RTSEngine::Foundation
    RTSEngine::Assets
    RealScript::GameSdk
)

target_compile_features(rts_scripting PUBLIC cxx_std_17)
target_compile_definitions(rts_scripting PUBLIC
    RTSENGINE_HAS_REALSCRIPT=1
    RTSENGINE_REALSCRIPT_GIT_TAG="${RTSENGINE_REALSCRIPT_GIT_TAG}"
)

if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(rts_scripting PRIVATE /bigobj)
endif()
