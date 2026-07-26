add_library(rts_assets STATIC
    src/AssetManager.cpp
    src/CookedAsset.cpp
    src/CookedContent.cpp
    src/Vfs.cpp
)
add_library(RTSEngine::Assets ALIAS rts_assets)

target_include_directories(rts_assets PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts_assets PUBLIC RTSEngine::Foundation)
target_compile_features(rts_assets PUBLIC cxx_std_17)
