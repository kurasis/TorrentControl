#include "self_test.hpp"

#include "tc/core/error.hpp"
#include "tc/service/storage.hpp"

#include <wil/com.h>
#include <wil/resource.h>
#include <shlobj.h>

namespace tc::app {

NativeSelfTest::NativeSelfTest(HWND owner, std::filesystem::path root, std::function<void(std::string const&)> log)
    : owner_(owner), log_(std::move(log)), root_(std::move(root))
{
    auto const payload = root_ / L"payload" / L"набор данных";
    std::filesystem::create_directories(payload);
    std::filesystem::create_directories(root_ / L"output");
    std::string bytes(70'003, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
    service::write_file_atomic(payload / L"данные #1.bin", bytes);
    service::write_file_atomic(payload / L"second.bin", bytes.substr(0, 19'991));
}

nlohmann::json NativeSelfTest::step(nlohmann::json const& payload)
{
    if (armed_ || dialog_) throw bridge::BridgeError("SELF_TEST", "A dialog step is already pending");
    std::string const name = payload.value("name", "");
    cancel_ = name == "cancel";
    if (cancel_) selection_.clear();
    else if (name == "file") selection_ = root_ / L"payload" / L"набор данных" / L"данные #1.bin";
    else if (name == "folder") selection_ = root_ / L"payload" / L"набор данных";
    else if (name == "payload") selection_ = root_ / L"payload";
    else if (name == "project") selection_ = root_ / L"output" / L"проект.tcproject";
    else if (name == "magnet") selection_ = root_ / L"output" / L"magnet.txt";
    else if (name == "v1" || name == "v2" || name == "hybrid" || name == "reopened")
        selection_ = root_ / L"output" / core::path_from_utf8(name + ".torrent");
    else throw bridge::BridgeError("SELF_TEST", "Unknown native dialog step");
    armed_ = true;
    return nlohmann::json::object();
}

void NativeSelfTest::on_timer()
{
    if (!dialog_) return;
    if (!ticked_) { ticked_ = true; log_("DIALOG timer dispatched"); }
    if (GetTickCount64() >= deadline_) {
        log_("DIALOG timeout");
        dialog_->Close(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        return;
    }
    auto window = wil::com_ptr<IFileDialog>(dialog_).try_query<IOleWindow>();
    HWND dialog_window = nullptr;
    if (!window || FAILED(window->GetWindow(&dialog_window)) || !IsWindowVisible(dialog_window)) return;
    if (!shown_) log_("DIALOG visible");
    shown_ = true;
    if (cancel_) {
        dialog_->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    } else if (!clicked_) {
        clicked_ = true;
        // Let the dialog validate the filename and populate its real result.
        // Close(S_OK) alone would bypass that path and cannot test selection.
        PostMessageW(dialog_window, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    }
}

HRESULT NativeSelfTest::show_dialog(IFileDialog* dialog)
{
    if (!armed_) throw bridge::BridgeError("SELF_TEST", "Unexpected native dialog");
    armed_ = false;
    if (!cancel_) {
        wil::com_ptr<IShellItem> folder;
        auto const parent = selection_.parent_path();
        HRESULT hr = SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&folder));
        if (FAILED(hr)) return hr;
        hr = dialog->SetFolder(folder.get());
        if (FAILED(hr)) return hr;
        hr = dialog->SetFileName(selection_.filename().c_str());
        if (FAILED(hr)) return hr;
    }
    dialog_ = dialog;
    clicked_ = shown_ = ticked_ = false;
    deadline_ = GetTickCount64() + 15'000;
    // Common item dialogs run their own message pump. Handle WM_TIMER in the
    // owner's window procedure rather than depending on TIMERPROC dispatch.
    UINT_PTR const timer = SetTimer(owner_, timer_id, 50, nullptr);
    if (!timer) {
        dialog_ = nullptr;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    log_("DIALOG show");
    HRESULT const hr = dialog->Show(owner_);
    KillTimer(owner_, timer_id);
    log_("DIALOG result " + std::to_string(static_cast<unsigned long>(hr)));
    dialog_ = nullptr;
    if (!shown_) throw bridge::BridgeError("SELF_TEST", "The common item dialog was not shown");
    if (SUCCEEDED(hr) && !cancel_) {
        wil::com_ptr<IShellItem> item;
        wil::unique_cotaskmem_string path;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))
            || std::filesystem::path(path.get()).lexically_normal() != selection_.lexically_normal())
            throw bridge::BridgeError("SELF_TEST", "The common item dialog selected the wrong path");
    }
    return hr;
}

} // namespace tc::app
