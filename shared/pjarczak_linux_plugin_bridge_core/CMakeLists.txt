add_library(pjarczak_linux_plugin_bridge_core STATIC
    BridgeCoreFrame.cpp
    BridgeCoreCallbackQueue.cpp
    BridgeCoreMethodManifest.cpp
    BridgeCoreSubprocess.cpp
)

target_include_directories(pjarczak_linux_plugin_bridge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pjarczak_linux_plugin_bridge_core PUBLIC nlohmann_json::nlohmann_json)
