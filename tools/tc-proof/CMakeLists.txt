# Internal developer tool for the M0 headless integration proof. Not a public
# CLI (that is out of scope for release 1) and not shipped in the installer.
add_executable(tc-proof main.cpp)
target_link_libraries(tc-proof PRIVATE TorrentControl::core nlohmann_json::nlohmann_json)
tc_set_warnings(tc-proof)
if(WIN32)
    target_sources(tc-proof PRIVATE ${PROJECT_SOURCE_DIR}/src/app/windows/long_path_aware.manifest)
endif()

add_executable(tc-network-proof network_main.cpp)
target_link_libraries(tc-network-proof PRIVATE TorrentControl::service)
tc_set_warnings(tc-network-proof)

add_executable(tc-native-proof native_main.cpp)
target_link_libraries(tc-native-proof PRIVATE TorrentControl::bridge TorrentControl::service)
tc_set_warnings(tc-native-proof)

add_executable(tc-workflow-proof workflow_main.cpp)
target_link_libraries(tc-workflow-proof PRIVATE TorrentControl::service)
tc_set_warnings(tc-workflow-proof)
if(WIN32)
    target_sources(tc-workflow-proof PRIVATE ${PROJECT_SOURCE_DIR}/src/app/windows/long_path_aware.manifest)
endif()
