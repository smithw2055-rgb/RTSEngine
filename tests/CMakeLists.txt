add_executable(rts_foundation_tests foundation_tests.cpp)
target_link_libraries(rts_foundation_tests PRIVATE RTSEngine::Sim)
add_test(NAME rts_foundation_tests COMMAND rts_foundation_tests)

add_executable(rts_ecs_tests ecs_tests.cpp)
target_link_libraries(rts_ecs_tests PRIVATE RTSEngine::Ecs)
add_test(NAME rts_ecs_tests COMMAND rts_ecs_tests)
