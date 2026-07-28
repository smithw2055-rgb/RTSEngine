add_library(rts_network STATIC
    src/UdpTransport.cpp
)
add_library(RTSEngine::Network ALIAS rts_network)

target_include_directories(rts_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_network PUBLIC RTSEngine::Foundation)
target_compile_features(rts_network PUBLIC cxx_std_17)

if(WIN32)
    target_link_libraries(rts_network PRIVATE ws2_32)
endif()
