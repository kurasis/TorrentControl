# NativeHost: Win32 window, WebView2 lifecycle, and the bundled frontend.
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)

add_executable(TorrentControl WIN32
    main.cpp
    host_services.cpp
    host_services.hpp
    webview_host.cpp
    webview_host.hpp
    torrentcontrol.manifest
)
target_link_libraries(TorrentControl PRIVATE
    TorrentControl::core
    TorrentControl::bridge
    TorrentControl::service
    unofficial::webview2::webview2
    WIL::WIL)
target_compile_definitions(TorrentControl PRIVATE TC_APP_VERSION="${PROJECT_VERSION}")
tc_set_warnings(TorrentControl)

# Frontend assets are served from <exe dir>/frontend through a WebView2
# virtual host mapping; nothing is loaded from the network.
add_custom_command(TARGET TorrentControl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/frontend $<TARGET_FILE_DIR:TorrentControl>/frontend
    COMMENT "Copying frontend assets")
