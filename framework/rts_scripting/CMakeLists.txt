add_library(rts_engine_rts_scripting STATIC
    src/RtsScriptSessionV2.cpp
    src/RtsScriptSessionArchive.cpp
    src/RtsEntityBehaviorRuntime.cpp
    src/RtsScriptWorldRuntime.cpp
)
add_library(RTSEngine::RtsScripting ALIAS rts_engine_rts_scripting)

set_target_properties(rts_engine_rts_scripting PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(rts_engine_rts_scripting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_engine_rts_scripting PUBLIC
    RTSEngine::Rts
    RTSEngine::Scripting
)

target_compile_features(rts_engine_rts_scripting PUBLIC cxx_std_17)

if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(rts_engine_rts_scripting PRIVATE /bigobj)
endif()
