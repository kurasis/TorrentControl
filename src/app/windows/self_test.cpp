#include "self_test.hpp"

#include "tc/core/error.hpp"
#include "tc/service/storage.hpp"

#include <wil/com.h>
#include <wil/resource.h>
#include <shlobj.h>

namespace tc::app {

NativeSelfTest::NativeSelfTest(HWND owner, std::filesystem::path root)
    : owner_(owner), root_(std::move(root))
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

void CALLBACK NativeSelfTest::tick(HWND, UINT, UINT_PTR id, DWORD)
{
    auto* test = reinterpret_cast<NativeSelfTest*>(id);
    if (GetTickCount64() >= test->deadline_) {
        test->dialog_->Close(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        return;
    }
    auto window = wil::com_ptr<IFileDialog>(test->dialog_).try_query<IOleWindow>();
    HWND dialog_window = nullptr;
    if (!window || FAILED(window->GetWindow(&dialog_window)) || !IsWindowVisible(dialog_window)) return;
    test->shown_ = true;
    if (test->cancel_) {
        test->dialog_->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    } else if (!test->clicked_) {
        test->clicked_ = true;
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
    clicked_ = shown_ = false;
    deadline_ = GetTickCount64() + 15'000;
    UINT_PTR const timer = SetTimer(owner_, reinterpret_cast<UINT_PTR>(this), 50, tick);
    if (!timer) {
        dialog_ = nullptr;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT const hr = dialog->Show(owner_);
    KillTimer(owner_, timer);
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
