add_executable(rts_g3_closure_tests
    g3_closure_tests.cpp
)
target_link_libraries(rts_g3_closure_tests PRIVATE
    RTSEngine::Rts
    RTSEngine::RtsPresentation
)
add_test(NAME rts_g3_closure_tests
    COMMAND rts_g3_closure_tests)
