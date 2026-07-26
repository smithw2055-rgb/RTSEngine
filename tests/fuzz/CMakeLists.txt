if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "RTSENGINE_BUILD_FUZZERS requires Clang/libFuzzer")
endif()

add_executable(rts_run_save_envelope_fuzz run_save_envelope_fuzz.cpp)
target_link_libraries(rts_run_save_envelope_fuzz PRIVATE RTSEngine::Roguelite)
target_compile_options(rts_run_save_envelope_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
)
target_link_options(rts_run_save_envelope_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
)
