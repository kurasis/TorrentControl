# CommandBridge: versioned message contract between the WebView2 frontend and
# the native host. Platform independent so validation is unit-tested headlessly.
add_library(tc_bridge
    src/async_dispatcher.cpp
    src/app_operations.cpp
    src/bencode_json.cpp
    src/protocol.cpp
)
add_library(TorrentControl::bridge ALIAS tc_bridge)

target_include_directories(tc_bridge PUBLIC include)
target_link_libraries(tc_bridge
    PUBLIC nlohmann_json::nlohmann_json TorrentControl::core TorrentControl::service)
target_compile_features(tc_bridge PUBLIC cxx_std_20)
tc_set_warnings(tc_bridge)
