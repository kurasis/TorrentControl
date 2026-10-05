#include "webview_host.hpp"

#include <wrl.h>

namespace tc::app {

using Microsoft::WRL::Callback;

namespace {

constexpr wchar_t virtual_host[] = L"torrentcontrol.example";

} // namespace

std::wstring to_wide(std::string_view utf8)
{
    if (utf8.empty()) return {};
    int const n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string to_utf8(std::wstring_view wide)
{
    if (wide.empty()) return {};
    int const n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

WebViewHost::WebViewHost(HWND window, Options options, bridge::Dispatcher const& dispatcher)
    : window_(window), options_(std::move(options)), dispatcher_(dispatcher)
{
}

HRESULT WebViewHost::start()
{
    return CreateCoreWebView2EnvironmentWithOptions(nullptr, options_.user_data_dir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) { return on_environment_created(result, env); })
            .Get());
}

HRESULT WebViewHost::on_environment_created(HRESULT result, ICoreWebView2Environment* env)
{
    if (FAILED(result)) {
        fail(result, L"The WebView2 environment could not be created.");
        return S_OK;
    }
    environment_ = env;
    HRESULT const hr = env->CreateCoreWebView2Controller(window_,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this](HRESULT r, ICoreWebView2Controller* c) { return on_controller_created(r, c); })
            .Get());
    if (FAILED(hr)) fail(hr, L"The WebView2 controller could not be created.");
    return S_OK;
}

HRESULT WebViewHost::on_controller_created(HRESULT result, ICoreWebView2Controller* controller)
{
    if (FAILED(result) || controller == nullptr) {
        fail(result, L"The WebView2 controller could not be created.");
        return S_OK;
    }
    controller_ = controller;
    controller_->get_CoreWebView2(&webview_);

    configure_settings();
    register_handlers();
    resize();

    // Serve only the bundled asset directory under a fixed origin. The source
    // tree and arbitrary local paths are never exposed.
    auto webview3 = webview_.try_query<ICoreWebView2_3>();
    if (!webview3) {
        fail(E_NOINTERFACE, L"The installed WebView2 Runtime is too old.");
        return S_OK;
    }
    HRESULT hr = webview3->SetVirtualHostNameToFolderMapping(
        virtual_host, options_.asset_dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
    if (FAILED(hr)) {
        fail(hr, L"The application files could not be mapped.");
        return S_OK;
    }
    std::wstring const url = std::wstring(L"https://") + virtual_host + L"/index.html" + options_.start_query;
    hr = webview_->Navigate(url.c_str());
    if (FAILED(hr)) fail(hr, L"The application page could not be opened.");
    return S_OK;
}

void WebViewHost::configure_settings()
{
    wil::com_ptr<ICoreWebView2Settings> settings;
    webview_->get_Settings(&settings);
    settings->put_IsScriptEnabled(TRUE);
    settings->put_IsWebMessageEnabled(TRUE);
    settings->put_AreHostObjectsAllowed(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
#ifdef NDEBUG
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    if (auto s3 = settings.try_query<ICoreWebView2Settings3>()) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
#endif
}

void WebViewHost::register_handlers()
{
    EventRegistrationToken token{};

    // Only bundled application content may be shown in the view.
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string uri;
                args->get_Uri(&uri);
                if (!bridge::is_allowed_source(to_utf8(uri.get()))) args->put_Cancel(TRUE);
                else {
                    ++page_generation_;
                    pending_.clear();
                }
                return S_OK;
            })
            .Get(),
        &token);

    webview_->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE); // block unsolicited windows
                return S_OK;
            })
            .Get(),
        &token);

    webview_->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                return S_OK;
            })
            .Get(),
        &token);

    if (auto webview4 = webview_.try_query<ICoreWebView2_4>()) {
        webview4->add_DownloadStarting(
            Callback<ICoreWebView2DownloadStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                    args->put_Cancel(TRUE);
                    return S_OK;
                })
                .Get(),
            &token);
    }

    // The bridge: string messages only, validated by the dispatcher (origin,
    // schema, operation) and answered with a serialized JSON string. Dropped
    // files arrive as native objects; only their paths are taken from them.
    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string source;
                wil::unique_cotaskmem_string message;
                if (FAILED(args->get_Source(&source)) || FAILED(args->TryGetWebMessageAsString(&message)))
                    return S_OK;
                PendingRequest request{to_utf8(message.get()), to_utf8(source.get()), {}, page_generation_};
                if (auto args2 = wil::com_ptr<ICoreWebView2WebMessageReceivedEventArgs>(args)
                                     .try_query<ICoreWebView2WebMessageReceivedEventArgs2>()) {
                    wil::com_ptr<ICoreWebView2ObjectCollectionView> objects;
                    UINT32 count = 0;
                    if (SUCCEEDED(args2->get_AdditionalObjects(&objects)) && objects && SUCCEEDED(objects->get_Count(&count))) {
                        for (UINT32 i = 0; i < count && i < 10000; ++i) {
                            wil::com_ptr<IUnknown> object;
                            if (FAILED(objects->GetValueAtIndex(i, &object)) || !object) continue;
                            auto file = object.try_query<ICoreWebView2File>();
                            if (!file) continue;
                            wil::unique_cotaskmem_string path;
                            if (SUCCEEDED(file->get_Path(&path)) && path) request.attached.emplace_back(path.get());
                        }
                    }
                }
                pending_.push_back(std::move(request));
                PostMessageW(window_, wm_bridge_request, 0, 0);
                return S_OK;
            })
            .Get(),
        &token);

    // Renderer failures: the native state is authoritative, so reloading the
    // page is safe; it requests a fresh snapshot and never replays commands.
    webview_->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                args->get_ProcessFailedKind(&kind);
                if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                    fail(E_FAIL, L"The WebView2 browser process exited.");
                } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED
                    || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                    ++page_generation_;
                    pending_.clear();
                    if (options_.on_renderer_recovery) options_.on_renderer_recovery();
                    if (FAILED(sender->Reload())) fail(E_FAIL, L"The application page could not be recovered.");
                }
                return S_OK;
            })
            .Get(),
        &token);
}

void WebViewHost::process_requests()
{
    // A modal dialog inside a handler pumps messages; the outer call drains
    // anything that arrives meanwhile, in order.
    if (processing_) return;
    processing_ = true;
    while (!pending_.empty()) {
        PendingRequest request = std::move(pending_.front());
        pending_.pop_front();
        std::string const response = dispatcher_.handle(request.message, request.source, request.attached);
        // A native modal dialog can pump navigation/recovery callbacks while
        // dispatching. Its response belongs to the old page, whose request IDs
        // may already have been reused by the new page.
        if (request.page_generation == page_generation_) post_to_page(response);
    }
    processing_ = false;
}

void WebViewHost::post_to_page(std::string const& message)
{
    if (webview_) webview_->PostWebMessageAsString(to_wide(message).c_str());
}

void WebViewHost::resize()
{
    if (!controller_) return;
    RECT bounds{};
    GetClientRect(window_, &bounds);
    controller_->put_Bounds(bounds);
}

void WebViewHost::notify_moved()
{
    if (controller_) controller_->NotifyParentWindowPositionChanged();
}

HRESULT WebViewHost::crash_renderer_for_self_test()
{
    if (!webview_) return E_UNEXPECTED;
    return webview_->CallDevToolsProtocolMethod(L"Page.crash", L"{}",
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
}

void WebViewHost::close()
{
    if (controller_) controller_->Close();
    controller_.reset();
    webview_.reset();
    pending_.clear();
    environment_.reset();
}

void WebViewHost::fail(HRESULT hr, wchar_t const* what)
{
    if (options_.on_fatal) options_.on_fatal(hr, what);
}

} // namespace tc::app
