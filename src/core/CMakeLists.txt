# TorrentEngine, MetainfoService and ManifestService. Must not depend on
# WebView2 or any UI types so it can be tested headlessly.
add_library(tc_core
    src/bencode.cpp
    src/error.cpp
    src/field_registry.cpp
    src/file_payload_source.cpp
    src/manifest.cpp
    src/native_fs.cpp
    src/output.cpp
    src/pause.cpp
    src/metainfo.cpp
    src/torrent_engine.cpp
    src/hash_pipeline.cpp
    src/verify.cpp
)
add_library(TorrentControl::core ALIAS tc_core)

target_include_directories(tc_core PUBLIC include)
target_link_libraries(tc_core PUBLIC LibtorrentRasterbar::torrent-rasterbar)
target_compile_features(tc_core PUBLIC cxx_std_20)
if(WIN32)
    target_compile_definitions(tc_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
tc_set_warnings(tc_core)
