add_executable(rts_engine_g1_g2_tests
    ../engine_g1_g2_tests.cpp
)
target_link_libraries(rts_engine_g1_g2_tests PRIVATE
    RTSEngine::Rts
    RTSEngine::Presentation
    RTSEngine::Audio
)
add_test(NAME rts_engine_g1_g2_tests COMMAND rts_engine_g1_g2_tests)
