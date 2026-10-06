#pragma once

#include "tc/bridge/protocol.hpp"
#include "tc/bridge/async_dispatcher.hpp"

#include <windows.h>
// WIN32_LEAN_AND_MEAN (set project-wide) leaves out the COM headers that
// define `interface`, which WebView2.h needs.
#include <objbase.h>

#include <WebView2.h>
#include <wil/com.h>

#include <deque>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tc::app {

std::wstring to_wide(std::string_view utf8);
std::string to_utf8(std::wstring_view wide);

// Posted to the window when bridge requests are waiting (see process_requests).
constexpr UINT wm_bridge_request = WM_APP + 1;

// Owns the WebView2 environment, controller and view for one top-level window.
// All methods and callbacks run on the window's STA UI thread.
class WebViewHost {
public:
    struct Options {
        std::wstring asset_dir;      // bundled frontend directory
        std::wstring browser_executable_folder; // empty = installed Evergreen
        std::wstring user_data_dir;  // WebView2 profile directory
        std::wstring start_query;    // e.g. L"?selfTest=1"
        // Called once for an unrecoverable error; the host is unusable afterwards.
        std::function<void(HRESULT, std::wstring const&)> on_fatal;
        std::function<void()> on_renderer_recovery;
    };

    WebViewHost(HWND window, Options options, bridge::Dispatcher const& dispatcher);
    ~WebViewHost();
    WebViewHost(WebViewHost const&) = delete;
    WebViewHost& operator=(WebViewHost const&) = delete;

    // Starts asynchronous creation. Failures are reported to the window with
    // a native message and do not throw.
    HRESULT start();
    void resize();
    void notify_moved();
    void close();
    // Invoked only by an operation registered for --self-test-flow.
    HRESULT crash_renderer_for_self_test();

    // Delivers worker replies on the STA thread, outside WebView2 callbacks.
    void process_requests();
    // Sends a serialized event to the page; dropped while no page is loaded.
    void post_to_page(std::string const& message);

private:
    HRESULT on_environment_created(HRESULT result, ICoreWebView2Environment* env);
    HRESULT on_controller_created(HRESULT result, ICoreWebView2Controller* controller);
    void configure_settings();
    void register_handlers();
    void fail(HRESULT hr, wchar_t const* what);

    struct PendingRequest {
        std::string message;
        std::string source;
        std::vector<std::filesystem::path> attached;
        std::uint64_t page_generation = 0;
    };

    HWND window_;
    Options options_;
    bridge::Dispatcher const& dispatcher_;
    wil::com_ptr<ICoreWebView2Environment> environment_;
    wil::com_ptr<ICoreWebView2Controller> controller_;
    wil::com_ptr<ICoreWebView2> webview_;
    std::uint64_t page_generation_ = 0;
    std::unique_ptr<bridge::AsyncDispatcher> commands_;
};

} // namespace tc::app
