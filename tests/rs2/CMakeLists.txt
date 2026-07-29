add_executable(rts_rts_scripting_tests
    ../rts_scripting_adapter_tests.cpp
)
target_link_libraries(rts_rts_scripting_tests PRIVATE
    RTSEngine::RtsScripting
    RealScript::GameCompiler
)
add_test(NAME rts_rts_scripting_tests COMMAND rts_rts_scripting_tests)

add_executable(rts_script_state_lockstep_tests
    ../rts_script_state_lockstep_tests.cpp
)
target_link_libraries(rts_script_state_lockstep_tests PRIVATE
    RTSEngine::RtsScripting
    RealScript::GameCompiler
)
add_test(NAME rts_script_state_lockstep_tests
    COMMAND rts_script_state_lockstep_tests)

add_executable(rts_entity_behavior_tests
    ../rts_entity_behavior_tests.cpp
)
target_link_libraries(rts_entity_behavior_tests PRIVATE
    RTSEngine::RtsScripting
    RealScript::GameCompiler
)
add_test(NAME rts_entity_behavior_tests
    COMMAND rts_entity_behavior_tests)
