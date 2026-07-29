add_executable(rts_rts_scripting_tests
    ../rts_scripting_adapter_tests.cpp
)
target_link_libraries(rts_rts_scripting_tests PRIVATE
    RTSEngine::RtsScripting
    RealScript::GameCompiler
)
add_test(NAME rts_rts_scripting_tests COMMAND rts_rts_scripting_tests)
