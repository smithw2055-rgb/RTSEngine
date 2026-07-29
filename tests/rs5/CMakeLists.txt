include(RTSEngineRealScript)

rtsengine_add_script_aot(
    rts_rs5_aot_smoke
    PROGRAM_NAME RtsRs5AotSmoke
    CPP_NAMESPACE rts_rs5_aot_smoke
    OPT_LEVEL 2
    SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/aot_smoke.rs"
)

add_executable(rts_script_mod_tests
    ../rts_script_mod_tests.cpp
)
target_link_libraries(rts_script_mod_tests PRIVATE
    RTSEngine::RtsScripting
    RealScript::GameCompiler
)
add_test(NAME rts_script_mod_tests COMMAND rts_script_mod_tests)
