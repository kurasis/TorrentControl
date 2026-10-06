// TorrentControl native host entry point.
//
// Usage:
//   TorrentControl.exe                      normal start
//   TorrentControl.exe --self-test LOGFILE  check the bridge, HTML dialogs and
//                                           profile persistence; log and exit
//                                           (0 = success); used by CI
//   TorrentControl.exe --self-test-flow LOGFILE  native dialogs/jobs/recovery
//   TorrentControl.exe --self-test-settings LOGFILE --self-test-data DIR
//                                           verify settings in a fresh process

#include "host_services.hpp"
#include "self_test.hpp"
#include "webview_host.hpp"

#include "tc/bridge/app_operations.hpp"
#include "tc/bridge/protocol.hpp"
#include "tc/core/torrent_engine.hpp"
#include "tc/service/app_service.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"TorrentControl.MainWindow";
constexpr wchar_t runtime_download_url[] = L"https://developer.microsoft.com/microsoft-edge/webview2/";
constexpr UINT_PTR self_test_timer = 1;
constexpr UINT self_test_timeout_ms = 120'000;
constexpr UINT wm_service_events = WM_APP + 2;
constexpr UINT wm_self_test_crash = WM_APP + 3;

// Events raised on service worker threads, handed to the UI thread.
class EventQueue {
public:
    explicit EventQueue(HWND window) : window_(window) {}

    void push(nlohmann::json event)
    {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(std::move(event));
        }
        if (!posted_.exchange(true)) PostMessageW(window_, wm_service_events, 0, 0);
    }

    // Takes the queued events. Progress updates of one job are coalesced to
    // the newest, keeping the position of that newest update.
    std::vector<nlohmann::json> take()
    {
        posted_ = false;
        std::vector<nlohmann::json> events;
        {
            std::lock_guard lock(mutex_);
            events.swap(events_);
        }
        std::map<std::string, std::size_t> last_job_event;
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i].value("type", "") == "job") last_job_event[events[i]["job"].value("id", "")] = i;
        std::vector<nlohmann::json> out;
        out.reserve(events.size());
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].value("type", "") == "job" && last_job_event[events[i]["job"].value("id", "")] != i) continue;
            out.push_back(std::move(events[i]));
        }
        return out;
    }

private:
    HWND window_;
    std::mutex mutex_;
    std::vector<nlohmann::json> events_;
    std::atomic<bool> posted_{false};
};

struct AppState {
    // Destroyed in reverse order: the service (which joins its workers) goes
    // before the queue its event sink writes to.
    std::unique_ptr<EventQueue> events;
    tc::bridge::EventChannel channel;
    std::unique_ptr<tc::app::WindowsHostServices> shell;
    std::unique_ptr<tc::app::NativeSelfTest> native_test;
    int renderer_recoveries = 0;
    std::unique_ptr<tc::service::AppService> service;
    std::unique_ptr<tc::app::WebViewHost> host;
    tc::bridge::Dispatcher dispatcher;
    std::optional<std::wstring> self_test_log;
    int exit_code = 0;
};

AppState* state_of(HWND hwnd)
{
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void write_self_test_log(AppState const& app, std::string const& line)
{
    if (!app.self_test_log) return;
    std::ofstream out(std::filesystem::path(*app.self_test_log), std::ios::app);
    out << line << "\n";
}

std::wstring executable_dir()
{
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        DWORD const n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    return path.substr(0, path.find_last_of(L"\\/"));
}

// %LOCALAPPDATA%\TorrentControl, or empty when it is unavailable.
std::wstring app_data_dir()
{
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) {
        dir = std::wstring(local) + L"\\TorrentControl";
    }
    CoTaskMemFree(local);
    return dir;
}

// The Runtime is detected before any web UI is constructed (section 16).
std::wstring configured_runtime_folder()
{
    constexpr wchar_t variable[] = L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER";
    DWORD const size = GetEnvironmentVariableW(variable, nullptr, 0);
    if (size == 0) return {};
    std::wstring folder(size, L'\0');
    DWORD const length = GetEnvironmentVariableW(variable, folder.data(), size);
    if (length == 0 || length >= size) return {};
    folder.resize(length);
    return folder;
}

std::optional<std::wstring> installed_runtime_version(std::wstring const& folder)
{
    LPWSTR version = nullptr;
    HRESULT const hr = GetAvailableCoreWebView2BrowserVersionString(folder.empty() ? nullptr : folder.c_str(), &version);
    std::optional<std::wstring> result;
    if (SUCCEEDED(hr) && version != nullptr) result = version;
    CoTaskMemFree(version);
    return result;
}

int show_runtime_missing(bool quiet)
{
    if (quiet) return 3;
    int const choice = MessageBoxW(nullptr,
        L"TorrentControl cannot find the Microsoft Edge WebView2 Runtime configured for this application.\n\n"
        L"Open the official download page now?",
        L"TorrentControl", MB_YESNO | MB_ICONINFORMATION);
    if (choice == IDYES) ShellExecuteW(nullptr, L"open", runtime_download_url, nullptr, nullptr, SW_SHOWNORMAL);
    return 3;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    AppState* app = state_of(hwnd);
    switch (msg) {
    case WM_NCCREATE: {
        auto const* cs = reinterpret_cast<CREATESTRUCTW const*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
    }
    case WM_SIZE:
        if (app && app->host) app->host->resize();
        return 0;
    case WM_MOVE:
    case WM_MOVING:
        if (app && app->host) app->host->notify_moved();
        break;
    case WM_DPICHANGED: {
        auto const* suggested = reinterpret_cast<RECT const*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case tc::app::wm_bridge_request:
        if (app && app->host) app->host->process_requests();
        return 0;
    case wm_self_test_crash:
        if (app && app->native_test && app->host) {
            HRESULT const hr = app->host->crash_renderer_for_self_test();
            if (FAILED(hr)) {
                write_self_test_log(*app, "FAIL renderer crash could not be requested");
                app->exit_code = 7;
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    case wm_service_events:
        if (app && app->events && app->host) {
            for (auto const& event : app->events->take()) app->host->post_to_page(app->channel.wrap(event));
        }
        return 0;
    case WM_CLOSE:
        // Closing with work in progress asks first (section 14.2); the
        // self-test and fatal errors close without asking.
        if (app && app->service && app->exit_code == 0 && !app->self_test_log && app->service->has_active_jobs()) {
            int const choice = MessageBoxW(hwnd,
                L"Torrents are still being created.\n\nCancel them and exit? Choose No to keep working.",
                L"TorrentControl", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (choice != IDYES) return 0;
            app->service->jobs().cancel_all();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_TIMER:
        if (wparam == tc::app::NativeSelfTest::timer_id && app && app->native_test) {
            app->native_test->on_timer();
            return 0;
        }
        if (wparam == self_test_timer && app) {
            write_self_test_log(*app, "FAIL timeout waiting for the frontend");
            app->exit_code = 4;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        if (app && app->host) app->host->close();
        PostQuitMessage(app ? app->exit_code : 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    AppState app;
    bool native_flow = false;
    bool settings_test = false;
    int diagnostics_port = 0;
    std::optional<std::wstring> self_test_data;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::wstring_view(argv[i]);
        if ((arg == L"--self-test" || arg == L"--self-test-flow" || arg == L"--self-test-settings") && i + 1 < argc) {
            settings_test = arg == L"--self-test-settings";
            native_flow = arg != L"--self-test";
            app.self_test_log = argv[++i];
        } else if (arg == L"--self-test-data" && i + 1 < argc) self_test_data = argv[++i];
        else if (arg == L"--self-test-diagnostics-port" && i + 1 < argc) diagnostics_port = _wtoi(argv[++i]);
    }
    LocalFree(argv);
    bool const self_test = app.self_test_log.has_value();
    if (self_test) std::filesystem::remove(std::filesystem::path(*app.self_test_log));

    // WebView2 requires a single-threaded apartment on the UI thread.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    std::wstring const runtime_folder = configured_runtime_folder();
    std::optional<std::wstring> const runtime = installed_runtime_version(runtime_folder);
    if (!runtime) {
        write_self_test_log(app, "FAIL WebView2 Runtime not found");
        int const code = show_runtime_missing(self_test);
        CoUninitialize();
        return code;
    }
    write_self_test_log(app, "runtime " + tc::app::to_utf8(*runtime));

    tc::bridge::register_core_operations(app.dispatcher, TC_APP_VERSION);
    // Self-tests must not alter the real user's profiles or WebView2 data.
    std::wstring const data_dir = self_test
        ? self_test_data.value_or((std::filesystem::path(*app.self_test_log).parent_path()
            / (L"TorrentControl-self-test-" + std::to_wstring(GetCurrentProcessId()))).wstring())
        : app_data_dir();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = window_class;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, window_class, L"TorrentControl", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1100, 760, nullptr, nullptr, instance, &app);
    if (hwnd == nullptr) return 1;

    // The service owns the draft, the job queue and the settings; the page
    // only mirrors it. Events cross to the UI thread through the queue.
    app.events = std::make_unique<EventQueue>(hwnd);
    tc::service::AppService::Options service_options;
    if (!data_dir.empty()) service_options.settings_path = std::filesystem::path(data_dir) / L"settings.json";
    EventQueue* const queue = app.events.get();
    try {
        app.service = std::make_unique<tc::service::AppService>(
            service_options, [queue](nlohmann::json const& event) { queue->push(event); });
    } catch (std::exception const& e) {
        // Unreadable settings must not prevent the start; keep them in memory.
        write_self_test_log(app, std::string("settings not loaded: ") + e.what());
        service_options.settings_path.clear();
        app.service = std::make_unique<tc::service::AppService>(
            service_options, [queue](nlohmann::json const& event) { queue->push(event); });
    }
    if (native_flow) {
        app.native_test = std::make_unique<tc::app::NativeSelfTest>(hwnd, std::filesystem::path(data_dir),
            [&app](std::string const& message) { write_self_test_log(app, message); });
        app.shell = std::make_unique<tc::app::WindowsHostServices>(hwnd,
            [&app](IFileDialog* dialog) { return app.native_test->show_dialog(dialog); });
    } else app.shell = std::make_unique<tc::app::WindowsHostServices>(hwnd);
    tc::bridge::register_app_operations(app.dispatcher, *app.service, *app.shell);

    if (self_test) {
        HWND const window = hwnd;
        AppState* const state = &app;
        if (native_flow) {
            app.dispatcher.register_operation("prepareSelfTestDiagnostics", [state, diagnostics_port](nlohmann::json const&) {
                if (diagnostics_port < 1 || diagnostics_port > 65535)
                    throw tc::bridge::BridgeError("SELF_TEST", "Local diagnostics fixture port is required");
                auto const base = "http://127.0.0.1:" + std::to_string(diagnostics_port);
                return state->service->update_draft({{"trackers", nlohmann::json::array({
                    {{"url", base + "/announce?token=fixture-secret"}, {"tier", 0}, {"enabled", true}}})},
                    {"webSeeds", nlohmann::json::array({base + "/seed/"})}}, std::nullopt);
            });
            app.dispatcher.register_operation("selfTestStep", [state](nlohmann::json const& payload) {
                write_self_test_log(*state, "STEP " + payload.value("name", ""));
                return state->native_test->step(payload);
            });
            app.dispatcher.register_operation("getSelfTestState", [state, settings_test](nlohmann::json const&) {
                return nlohmann::json{{"checkpoint", state->native_test->checkpoint},
                    {"rendererRecoveries", state->renderer_recoveries}, {"settingsOnly", settings_test}};
            });
            app.dispatcher.register_operation("crashSelfTestRenderer", [state, window](nlohmann::json const& payload) {
                if (!state->native_test->checkpoint.is_null())
                    throw tc::bridge::BridgeError("SELF_TEST", "Renderer recovery already requested");
                state->native_test->checkpoint = payload;
                write_self_test_log(*state, "STEP renderer crash");
                PostMessageW(window, wm_self_test_crash, 0, 0);
                return nlohmann::json::object();
            });
            app.dispatcher.register_operation("checkSelfTestOutput", [state](nlohmann::json const&) {
                auto const folder = state->native_test->root() / L"output";
                auto const first = tc::service::read_small_file(folder / L"hybrid.torrent");
                auto const second = tc::service::read_small_file(folder / L"reopened.torrent");
                auto const magnet = tc::service::read_small_file(folder / L"magnet.txt");
                return nlohmann::json{{"identical", first == second}, {"magnet", magnet}};
            });
            app.dispatcher.register_operation("checkSelfTestEdits", [state](nlohmann::json const&) {
                auto const folder = state->native_test->root() / L"output";
                auto const original = tc::core::Metainfo::parse(tc::service::read_small_file(folder / L"hybrid.torrent"));
                auto const outer = tc::core::Metainfo::parse(tc::service::read_small_file(folder / L"outer-edited.torrent"));
                auto const info = tc::core::Metainfo::parse(tc::service::read_small_file(folder / L"info-edited.torrent"));
                auto same = [](tc::core::bencode::Value const* a, tc::core::bencode::Value const* b) {
                    if (!a || !b) return a == b;
                    return tc::core::bencode::encode(*a) == tc::core::bencode::encode(*b);
                };
                bool unchanged = true;
                for (auto const* key : {"pieces", "file tree"}) {
                    unchanged = unchanged && same(original.info().find(key), info.info().find(key));
                }
                unchanged = unchanged && same(original.root().find("piece layers"), info.root().find("piece layers"));
                return nlohmann::json{{"rawInfoPreserved", original.raw_info() == outer.raw_info()},
                    {"payloadHashesPreserved", unchanged},
                    {"commentPreserved", info.root().find("comment")->text() == outer.root().find("comment")->text()},
                    {"sourceEdited", info.info().find("source")->text() == "native-m3"}};
            });
        }
        app.dispatcher.register_operation("checkSelfTestProfile", [path = service_options.settings_path](nlohmann::json const& payload) {
            auto const settings = tc::service::load_settings(path);
            bool const persisted = std::any_of(settings.custom_profiles.begin(), settings.custom_profiles.end(),
                [&](auto const& profile) { return profile.id == payload.value("profileId", "")
                    && profile.name == payload.value("name", ""); });
            return nlohmann::json{{"persisted", persisted}};
        });
        app.dispatcher.register_operation("reportSelfTest", [window, state](nlohmann::json const& payload) {
            bool const ok = payload.value("ok", false);
            write_self_test_log(*state, std::string(ok ? "PASS " : "FAIL ") + payload.dump());
            state->exit_code = ok ? 0 : 5;
            PostMessageW(window, WM_CLOSE, 0, 0);
            return nlohmann::json::object();
        });
        SetTimer(hwnd, self_test_timer, self_test_timeout_ms, nullptr);
    }

    tc::app::WebViewHost::Options options;
    options.asset_dir = executable_dir() + L"\\frontend";
    options.browser_executable_folder = runtime_folder;
    options.user_data_dir = data_dir.empty() ? std::wstring() : data_dir + L"\\WebView2";
    options.start_query = native_flow ? L"?selfTest=1&nativeFlow=1" : self_test ? L"?selfTest=1" : L"";
    options.on_renderer_recovery = [&app] {
        ++app.renderer_recoveries;
        write_self_test_log(app, "STEP renderer recovery " + std::to_string(app.renderer_recoveries));
    };
    options.on_fatal = [hwnd, &app, self_test](HRESULT hr, std::wstring const& what) {
        write_self_test_log(app, "FAIL " + tc::app::to_utf8(what) + " hr=" + std::to_string(static_cast<unsigned long>(hr)));
        if (!self_test) {
            wchar_t text[600];
            swprintf_s(text, L"%s\n\nError code: 0x%08X", what.c_str(), static_cast<unsigned>(hr));
            MessageBoxW(hwnd, text, L"TorrentControl", MB_OK | MB_ICONERROR);
        }
        app.exit_code = 6;
        // Posted, not destroyed inline: this may run inside a WebView2 callback.
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    };
    app.host = std::make_unique<tc::app::WebViewHost>(hwnd, std::move(options), app.dispatcher);

    ShowWindow(hwnd, self_test ? SW_SHOWNOACTIVATE : show);
    UpdateWindow(hwnd);

    if (HRESULT const hr = app.host->start(); FAILED(hr)) {
        write_self_test_log(app, "FAIL CreateCoreWebView2EnvironmentWithOptions hr=" + std::to_string(static_cast<unsigned long>(hr)));
        app.exit_code = 6;
        DestroyWindow(hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    app.host.reset();
    app.service.reset();
    CoUninitialize();
    return app.exit_code;
}
