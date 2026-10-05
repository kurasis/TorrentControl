#pragma once

#include "tc/bridge/app_operations.hpp"

#include <windows.h>
#include <shobjidl.h>

#include <functional>
#include <utility>

namespace tc::app {

// HostServices backed by the Windows shell: common item dialogs, Explorer and
// file associations. Must be used on the window's STA UI thread.
class WindowsHostServices final : public bridge::HostServices {
public:
    // The self-test supplies a runner which automates the real common item
    // dialog. Normal starts always use IFileDialog::Show directly.
    using DialogRunner = std::function<HRESULT(IFileDialog*)>;
    explicit WindowsHostServices(HWND owner, DialogRunner runner = {})
        : owner_(owner), dialog_runner_(std::move(runner)) {}

    std::vector<std::filesystem::path> pick_open(OpenKind kind) override;
    std::optional<std::filesystem::path> pick_save(
        SaveKind kind, std::string const& suggested_name, std::filesystem::path const& folder) override;
    void show_in_folder(std::filesystem::path const& file) override;
    bool open_with_default_app(std::filesystem::path const& file) override;
    bool open_url(std::string const& url) override;

private:
    HWND owner_;
    DialogRunner dialog_runner_;
    HRESULT show_dialog(IFileDialog* dialog);
};

} // namespace tc::app
