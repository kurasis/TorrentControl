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
#include "../install_identity.hpp"
#include "../runtime_policy.hpp"
#include "../../../tools/tc-proof/workflow_fixture.hpp"
#include "../../../tools/tc-proof/workflow_probe.hpp"
#include "../../../tools/tc-proof/session_fixture.hpp"

#include "tc/bridge/app_operations.hpp"
#include "tc/bridge/protocol.hpp"
#include "tc/core/torrent_engine.hpp"
#include "tc/service/app_service.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"TorrentControl.MainWindow";
constexpr wchar_t runtime_download_url[] = L"https://developer.microsoft.com/microsoft-edge/webview2/";
constexpr UINT_PTR self_test_timer = 1;
constexpr UINT self_test_timeout_ms = 120'000;
constexpr UINT wm_service_events = WM_APP + 2;
constexpr UINT wm_self_test_crash = WM_APP + 3;
constexpr UINT wm_ui_tasks = WM_APP + 4;
constexpr UINT_PTR performance_timer = 3;
constexpr UINT wm_smb_self_test_control = WM_APP + 5;
constexpr UINT_PTR smb_self_test_timer = 4;

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
    std::unique_ptr<tc::bridge::UiTasks> ui;
    // Destroyed in reverse order: the service (which joins its workers) goes
    // before the queue its event sink writes to.
    std::unique_ptr<EventQueue> events;
    tc::bridge::EventChannel channel;
    std::unique_ptr<tc::app::WindowsHostServices> shell;
    std::unique_ptr<tc::app::NativeSelfTest> native_test;
    int renderer_recoveries = 0;
    // Outlives the service and every payload reader that references it.
    std::unique_ptr<tc::proof::WorkflowProbe> smb_probe;
    std::uint64_t smb_sequence = 0;
    std::unique_ptr<tc::service::AppService> service;
    tc::bridge::Dispatcher dispatcher;
    std::unique_ptr<tc::app::WebViewHost> host;
    std::optional<std::wstring> self_test_log;
    int exit_code = 0;
    ULONGLONG performance_last_tick = 0;
    ULONGLONG performance_max_gap = 0;
    std::size_t performance_ticks = 0;
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

void write_smb_self_test_snapshot(AppState& app, LPARAM acknowledgement = 0)
{
    if (!app.smb_probe || !app.service) return;
    auto const page = app.service->jobs().bridge_page(0, 1);
    write_self_test_log(app, "SMB " + nlohmann::json{{"sequence", ++app.smb_sequence}, {"ack", acknowledgement},
        {"probe", app.smb_probe->snapshot()}, {"job", page.at("jobs").empty() ? nlohmann::json(nullptr) : page.at("jobs").at(0)}}.dump());
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

int show_runtime_unsupported(bool quiet, std::wstring const& details)
{
    if (quiet) return 3;
    int const choice = MessageBoxW(nullptr,
        (details + L"\n\nInstall or update the Microsoft Edge WebView2 Evergreen Runtime.\n"
            L"Open the official download page now?").c_str(),
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
    case wm_ui_tasks:
        if (app && app->ui) app->ui->drain();
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
    case wm_smb_self_test_control:
        // A developer-only hook on the task-owned HWND, absent in normal use.
        if (app && app->self_test_log && app->smb_probe) {
            if (wparam == 1) app->smb_probe->release();
            write_smb_self_test_snapshot(*app, lparam);
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
        if (app && app->smb_probe) write_self_test_log(*app, "SMB_CLOSE_REQUESTED");
        DestroyWindow(hwnd);
        return 0;
    case WM_TIMER:
        if (wparam == smb_self_test_timer && app && app->smb_probe) {
            write_smb_self_test_snapshot(*app);
            return 0;
        }
        if (wparam == performance_timer && app) {
            auto const now = GetTickCount64();
            app->performance_max_gap = std::max(app->performance_max_gap, now - app->performance_last_tick);
            app->performance_last_tick = now;
            ++app->performance_ticks;
            return 0;
        }
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
        if (app && app->ui) app->ui->close();
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
    // Holding a handle (without owning the mutex) lets all app instances run,
    // while Setup/Uninstall can refuse to replace files during a live job.
    wil::unique_handle install_guard(CreateMutexW(nullptr, FALSE, tc::app::install_mutex));
    if (!install_guard) return 1;
    AppState app;
    bool native_flow = false;
    bool settings_test = false;
    int diagnostics_port = 0;
    std::optional<std::wstring> self_test_data;
    std::optional<std::wstring> performance_root;
    std::optional<std::string> self_test_minimum_runtime;
    std::optional<std::wstring> memory_fixture;
    std::optional<std::wstring> smb_fixture;
    std::optional<std::wstring> session_fixture;
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
        else if (arg == L"--self-test-performance-root" && i + 1 < argc) performance_root = argv[++i];
        else if (arg == L"--self-test-minimum-runtime" && i + 1 < argc) self_test_minimum_runtime = tc::app::to_utf8(argv[++i]);
        else if (arg == L"--self-test-memory" && i + 1 < argc) memory_fixture = argv[++i];
        else if (arg == L"--self-test-smb" && i + 1 < argc) smb_fixture = argv[++i];
        else if (arg == L"--self-test-session" && i + 1 < argc) session_fixture = argv[++i];
    }
    LocalFree(argv);
    bool const self_test = app.self_test_log.has_value();
    if (self_test) std::filesystem::remove(std::filesystem::path(*app.self_test_log));

    // WebView2 requires a single-threaded apartment on the UI thread.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    std::wstring const runtime_folder = configured_runtime_folder();
    std::optional<std::wstring> const runtime = installed_runtime_version(runtime_folder);
    std::string const minimum = self_test && self_test_minimum_runtime
        ? *self_test_minimum_runtime : std::string(tc::app::minimum_webview_runtime);
    if (!runtime) {
        write_self_test_log(app, "FAIL WebView2 Runtime not found");
        int const code = show_runtime_unsupported(self_test,
            L"TorrentControl cannot find the WebView2 Runtime configured for this application.\n"
            L"Required version: " + tc::app::to_wide(minimum) + L" or later.");
        CoUninitialize();
        return code;
    }
    write_self_test_log(app, "runtime " + tc::app::to_utf8(*runtime));
    if (!tc::app::supports_webview_runtime(tc::app::to_utf8(*runtime), minimum)) {
        write_self_test_log(app, "FAIL WebView2 Runtime unsupported; required " + minimum);
        int const code = show_runtime_unsupported(self_test,
            L"The configured WebView2 Runtime is too old or its version is not recognized.\n"
            L"Installed version: " + *runtime + L"\nRequired version: " + tc::app::to_wide(minimum) + L" or later.");
        CoUninitialize();
        return code;
    }

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
    app.ui = std::make_unique<tc::bridge::UiTasks>([hwnd] { return PostMessageW(hwnd, wm_ui_tasks, 0, 0) != FALSE; });

    // The service owns the draft, the job queue and the settings; the page
    // only mirrors it. Events cross to the UI thread through the queue.
    app.events = std::make_unique<EventQueue>(hwnd);
    tc::service::AppService::Options service_options;
    if (!data_dir.empty()) service_options.settings_path = std::filesystem::path(data_dir) / L"settings.json";
    std::optional<nlohmann::json> smb_input;
    if (self_test && smb_fixture) {
        smb_input = nlohmann::json::parse(tc::service::read_small_file(std::filesystem::path(*smb_fixture)));
        app.smb_probe = std::make_unique<tc::proof::WorkflowProbe>();
        service_options.jobs.payload_factory = [&app] { return std::make_unique<tc::proof::ProbeSource>(*app.smb_probe); };
    }
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
        app.shell = std::make_unique<tc::app::WindowsHostServices>(hwnd, *app.ui,
            [&app](IFileDialog* dialog) { return app.native_test->show_dialog(dialog); });
    } else app.shell = std::make_unique<tc::app::WindowsHostServices>(hwnd, *app.ui);
    tc::bridge::register_app_operations(app.dispatcher, *app.service, *app.shell);

    if (self_test) {
        HWND const window = hwnd;
        AppState* const state = &app;
        auto register_ui_operation = [state](std::string name, tc::bridge::Dispatcher::Handler handler) {
            state->dispatcher.register_operation(std::move(name), [state, handler = std::move(handler)](nlohmann::json const& payload) {
                return state->ui->invoke([&] { return handler(payload); });
            });
        };
        if (memory_fixture) {
            auto const input = nlohmann::json::parse(tc::service::read_small_file(std::filesystem::path(*memory_fixture)));
            auto const sample_ack_directory = tc::core::path_from_utf8(input.value("sampleAckDirectory", std::string{}));
            app.dispatcher.register_operation("prepareSelfTestMemory", [state, input](nlohmann::json const&) {
                return tc::proof::prepare_fixture(*state->service, input);
            });
            app.dispatcher.register_operation("memorySelfTestCheckpoint", [state, sample_ack_directory](nlohmann::json const& p) {
                std::string const phase = p.at("phase");
                if (phase != "idle" && phase != "scan" && phase != "review" && phase != "create"
                    && phase != "completed" && phase != "cleared")
                    throw tc::bridge::BridgeError("SELF_TEST", "Invalid memory phase");
                auto const acknowledgement = sample_ack_directory / ("sampled-" + phase);
                if (!sample_ack_directory.empty() && std::filesystem::exists(acknowledgement))
                    throw tc::bridge::BridgeError("SELF_TEST", "Stale memory sample acknowledgement");
                write_self_test_log(*state, "MEMORY " + phase);
                if (!sample_ack_directory.empty()) {
                    // This operation runs on a bridge worker. Keep the phase
                    // stable while the external sampler measures the real
                    // process tree; the UI thread continues pumping messages.
                    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    while (!std::filesystem::exists(acknowledgement)) {
                        if (std::chrono::steady_clock::now() >= deadline)
                            throw tc::bridge::BridgeError("SELF_TEST", "Memory sampler did not acknowledge " + phase);
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                return nlohmann::json::object();
            });
            app.dispatcher.register_operation("joinSelfTestMemory", [state](nlohmann::json const&) {
                state->service->jobs().wait_idle();
                return nlohmann::json{{"joined", true}};
            });
        }
        if (session_fixture) {
            auto session = std::make_shared<tc::proof::SessionFixture>(nlohmann::json::parse(
                tc::service::read_small_file(std::filesystem::path(*session_fixture))));
            app.dispatcher.register_operation("sessionPrepare", [state, session](nlohmann::json const& p) {
                return session->prepare(*state->service, p.at("format"));
            });
            app.dispatcher.register_operation("sessionEnqueue", [state, session](nlohmann::json const& p) {
                return session->enqueue(*state->service, p.at("kind"), p.at("count"));
            });
            app.dispatcher.register_operation("sessionJoin", [state](nlohmann::json const&) {
                state->service->jobs().wait_idle(); return nlohmann::json{{"joined", true}};
            });
            app.dispatcher.register_operation("sessionFinish", [state, session](nlohmann::json const&) {
                try { return session->finish_cycle(*state->service); }
                catch (std::exception const& error) {
                    write_self_test_log(*state, "SESSION_FAILURE " + std::string(error.what()));
                    throw tc::bridge::BridgeError("SELF_TEST", error.what());
                }
            });
            app.dispatcher.register_operation("sessionCheckpoint", [state, window](nlohmann::json const& p) {
                auto result = state->service->retention_summary();
                result["phase"] = p.at("phase"); result["renderer"] = p.at("renderer");
                state->ui->invoke([&] {
                    if (p.at("phase") == "idle") {
                        state->performance_ticks = 0; state->performance_max_gap = 0;
                        state->performance_last_tick = GetTickCount64();
                        if (!SetTimer(window, performance_timer, 100, nullptr))
                            throw tc::bridge::BridgeError("SELF_TEST", "Could not start session heartbeat");
                    }
                    result["nativeTicks"] = state->performance_ticks;
                    result["nativeMaxGapMs"] = state->performance_max_gap;
                });
                write_self_test_log(*state, "SESSION " + result.dump()); return result;
            });
        }
        if (smb_input) {
            app.dispatcher.register_operation("prepareSelfTestSmb", [state, input = *smb_input](nlohmann::json const&) {
                auto result = tc::proof::prepare_fixture(*state->service, input);
                state->smb_probe->arm(input.at("site"));
                return result;
            });
            register_ui_operation("smbSelfTestStarted", [state, window](nlohmann::json const&) {
                if (!SetTimer(window, smb_self_test_timer, 100, nullptr))
                    throw tc::bridge::BridgeError("SELF_TEST", "Could not start SMB observation timer");
                write_smb_self_test_snapshot(*state);
                return nlohmann::json::object();
            });
        }
        if (native_flow) {
            app.dispatcher.register_operation("prepareSelfTestDiagnostics", [state, diagnostics_port](nlohmann::json const&) {
                if (diagnostics_port < 1 || diagnostics_port > 65535)
                    throw tc::bridge::BridgeError("SELF_TEST", "Local diagnostics fixture port is required");
                auto const base = "http://127.0.0.1:" + std::to_string(diagnostics_port);
                return state->service->update_draft({{"trackers", nlohmann::json::array({
                    {{"url", base + "/announce?token=fixture-secret"}, {"tier", 0}, {"enabled", true}}})},
                    {"webSeeds", nlohmann::json::array({base + "/seed/"})}}, std::nullopt);
            });
            register_ui_operation("selfTestStep", [state](nlohmann::json const& payload) {
                write_self_test_log(*state, "STEP " + payload.value("name", ""));
                return state->native_test->step(payload);
            });
            register_ui_operation("getSelfTestState", [state, settings_test, performance_root](nlohmann::json const&) {
                return nlohmann::json{{"checkpoint", state->native_test->checkpoint},
                    {"rendererRecoveries", state->renderer_recoveries}, {"settingsOnly", settings_test},
                    {"performanceFixture", performance_root.has_value()}};
            });
            register_ui_operation("crashSelfTestRenderer", [state, window](nlohmann::json const& payload) {
                if (!state->native_test->checkpoint.is_null())
                    throw tc::bridge::BridgeError("SELF_TEST", "Renderer recovery already requested");
                state->native_test->checkpoint = payload;
                write_self_test_log(*state, "STEP renderer crash");
                PostMessageW(window, wm_self_test_crash, 0, 0);
                return nlohmann::json::object();
            });
            app.dispatcher.register_operation("runSelfTestResponsiveness", [state, window, performance_root](nlohmann::json const&) {
                if (!performance_root) throw tc::bridge::BridgeError("SELF_TEST", "A native performance fixture is required");
                state->ui->invoke([state, window] {
                    state->performance_ticks = 0;
                    state->performance_max_gap = 0;
                    state->performance_last_tick = GetTickCount64();
                    if (!SetTimer(window, performance_timer, 10, nullptr))
                        throw tc::bridge::BridgeError("SELF_TEST", "Could not start the native UI heartbeat");
                });
                try {
                    state->service->new_draft();
                    state->service->add_sources({std::filesystem::path(*performance_root)});
                    state->service->wait_for_scan();
                    state->service->update_draft({{"trackers", nlohmann::json::array()}, {"format", "hybrid"}}, std::nullopt);
                    auto const validation = state->service->validate_draft();
                    auto const page = state->service->manifest_page(0, 200, "-099");
                    auto const snapshot = state->service->snapshot();
                    auto result = state->ui->invoke([state, window] {
                        KillTimer(window, performance_timer);
                        return nlohmann::json{{"nativeTicks", state->performance_ticks},
                            {"nativeMaxGapMs", std::max(state->performance_max_gap, GetTickCount64() - state->performance_last_tick)}};
                    });
                    result["files"] = snapshot.at("scan").at("summary").at("realFiles");
                    result["canCreate"] = validation.at("canCreate");
                    result["filteredFiles"] = page.at("total");
                    return result;
                } catch (...) {
                    state->ui->invoke([window] { KillTimer(window, performance_timer); });
                    throw;
                }
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
        register_ui_operation("reportSelfTest", [window, state](nlohmann::json const& payload) {
            bool const ok = payload.value("ok", false);
            write_self_test_log(*state, std::string(ok ? "PASS " : "FAIL ") + payload.dump());
            state->exit_code = ok ? 0 : 5;
            PostMessageW(window, WM_CLOSE, 0, 0);
            return nlohmann::json::object();
        });
        SetTimer(hwnd, self_test_timer, memory_fixture || smb_fixture || session_fixture ? 600'000 : self_test_timeout_ms, nullptr);
    }

    tc::app::WebViewHost::Options options;
    options.asset_dir = executable_dir() + L"\\frontend";
    options.browser_executable_folder = runtime_folder;
    options.user_data_dir = data_dir.empty() ? std::wstring() : data_dir + L"\\WebView2";
    options.start_query = self_test && session_fixture ? L"?selfTest=1&sessionFlow=1"
        : self_test && smb_fixture ? L"?selfTest=1&smbFlow=1"
        : self_test && memory_fixture ? L"?selfTest=1&memoryFlow=1"
        : native_flow ? L"?selfTest=1&nativeFlow=1" : self_test ? L"?selfTest=1" : L"";
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
    if (app.smb_probe) write_self_test_log(app, "SMB_SHUTDOWN " + app.smb_probe->snapshot().dump());
    CoUninitialize();
    return app.exit_code;
}
