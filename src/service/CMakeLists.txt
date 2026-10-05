# Application service: drafts, profiles, jobs, batch planning and local
# persistence. Platform independent so the whole workflow is tested headlessly.
find_package(CURL CONFIG REQUIRED)
find_package(c-ares CONFIG REQUIRED)

add_library(tc_service
    src/app_service.cpp
    src/batch.cpp
    src/diagnostics.cpp
    src/network_probe.cpp
    src/draft.cpp
    src/jobs.cpp
    src/profiles.cpp
    src/sources.cpp
    src/storage.cpp
    src/torrent_editor.cpp
)
add_library(TorrentControl::service ALIAS tc_service)

target_include_directories(tc_service PUBLIC include)
target_link_libraries(tc_service PUBLIC TorrentControl::core nlohmann_json::nlohmann_json PRIVATE CURL::libcurl c-ares::cares)
if(WIN32)
    target_link_libraries(tc_service PRIVATE crypt32)
endif()
target_compile_features(tc_service PUBLIC cxx_std_20)
tc_set_warnings(tc_service)
