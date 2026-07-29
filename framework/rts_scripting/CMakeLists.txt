add_library(rts_engine_rts_scripting STATIC
    src/RtsScriptAdapter.cpp
)
add_library(RTSEngine::RtsScripting ALIAS rts_engine_rts_scripting)

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

if(RTSENGINE_BUILD_TESTS)
    add_executable(rts_script_adapter_tests
        ${CMAKE_SOURCE_DIR}/tests/rts_script_adapter_tests.cpp
    )
    target_link_libraries(rts_script_adapter_tests PRIVATE
        RTSEngine::RtsScripting
        RealScript::GameCompiler
    )
    add_test(NAME rts_script_adapter_tests COMMAND rts_script_adapter_tests)
endif()
