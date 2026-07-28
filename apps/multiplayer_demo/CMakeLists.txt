add_executable(rts_multiplayer_demo main.cpp)
target_link_libraries(rts_multiplayer_demo PRIVATE
    RTSEngine::RtsDesktop
    RTSEngine::Rts
    RTSEngine::Network
)
target_compile_features(rts_multiplayer_demo PRIVATE cxx_std_17)

if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(rts_multiplayer_demo PRIVATE /bigobj)
endif()
