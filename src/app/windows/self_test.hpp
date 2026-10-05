#pragma once

#include "host_services.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace tc::app {

// Only constructed for --self-test-flow. Fixtures and dialog destinations are
// native-owned; the page selects named steps, never arbitrary filesystem paths.
class NativeSelfTest {
public:
    NativeSelfTest(HWND owner, std::filesystem::path root);
    nlohmann::json step(nlohmann::json const& payload);
    HRESULT show_dialog(IFileDialog* dialog);
    nlohmann::json checkpoint = nullptr;
    std::filesystem::path const& root() const { return root_; }

private:
    static void CALLBACK tick(HWND, UINT, UINT_PTR id, DWORD);
    HWND owner_;
    std::filesystem::path root_;
    std::filesystem::path selection_;
    bool armed_ = false;
    bool cancel_ = false;
    bool clicked_ = false;
    bool shown_ = false;
    IFileDialog* dialog_ = nullptr;
    ULONGLONG deadline_ = 0;
};

} // namespace tc::app
