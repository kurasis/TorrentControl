find_package(Catch2 3 CONFIG REQUIRED)
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
include(Catch)

add_executable(tc_unit_tests
    unit/app_bridge_tests.cpp
    unit/async_bridge_tests.cpp
    unit/bencode_json_tests.cpp
    unit/bencode_tests.cpp
    unit/bridge_tests.cpp
    unit/consistency_tests.cpp
    unit/diagnostics_tests.cpp
    unit/engine_tests.cpp
    unit/field_registry_tests.cpp
    unit/job_paging_tests.cpp
    unit/manifest_policy_tests.cpp
    unit/manifest_tests.cpp
    unit/metainfo_tests.cpp
    unit/metainfo_validation_tests.cpp
    unit/output_tests.cpp
    unit/outgoing_bridge_tests.cpp
    unit/service_tests.cpp
    unit/verify_tests.cpp
    unit/test_support.cpp
)
target_link_libraries(tc_unit_tests PRIVATE TorrentControl::core TorrentControl::service TorrentControl::bridge Catch2::Catch2WithMain)
tc_set_warnings(tc_unit_tests)
if(WIN32)
    target_sources(tc_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app/windows/long_path_aware.manifest)
endif()
# Catch2 exits with 4 when a test case is skipped (for example a POSIX-only
# permission test running as root); report that as skipped, not failed.
catch_discover_tests(tc_unit_tests PROPERTIES SKIP_RETURN_CODE 4)

# Headless integration proof (specification section 3.1): drives tc-proof and
# checks every output with the independent Python reference verifier.
if(TC_BUILD_TOOLS)
    add_test(NAME integration_proof
        COMMAND ${Python3_EXECUTABLE} -m unittest -v test_proof
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/integration)
    set_tests_properties(integration_proof PROPERTIES
        ENVIRONMENT "TC_PROOF=$<TARGET_FILE:tc-proof>;TC_VERIFIER=${PROJECT_SOURCE_DIR}/tools/reference/verify_torrent.py"
        TIMEOUT 600)
endif()

if(TC_BUILD_TOOLS)
    add_test(NAME network_diagnostics_proof
        COMMAND ${Python3_EXECUTABLE} -m unittest -v test_network
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/integration)
    set_tests_properties(network_diagnostics_proof PROPERTIES
        ENVIRONMENT "TC_NETWORK_PROOF=$<TARGET_FILE:tc-network-proof>"
        TIMEOUT 120)
endif()
