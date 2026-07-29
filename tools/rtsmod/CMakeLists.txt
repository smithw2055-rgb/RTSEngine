add_executable(rtsmod main.cpp)
target_link_libraries(rtsmod PRIVATE RTSEngine::Scripting)
target_compile_features(rtsmod PRIVATE cxx_std_17)
set_target_properties(rtsmod PROPERTIES FOLDER "RTSEngine/Tools")
