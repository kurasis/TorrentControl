#pragma once

#include "host_services.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace tc::app {

// Only constructed for --self-test-flow. Fixtures and dialog destinations are
// native-owned; the page selects named steps, never arbitrary filesystem paths.
class NativeSelfTest {
public:
    static constexpr UINT_PTR timer_id = 2;
    NativeSelfTest(HWND owner, std::filesystem::path root, std::function<void(std::string const&)> log);
    nlohmann::json step(nlohmann::json const& payload);
    HRESULT show_dialog(IFileDialog* dialog);
    void on_timer();
    nlohmann::json checkpoint = nullptr;
    std::filesystem::path const& root() const { return root_; }

private:
    HWND owner_;
    std::function<void(std::string const&)> log_;
    std::filesystem::path root_;
    std::filesystem::path selection_;
    bool armed_ = false;
    bool cancel_ = false;
    bool clicked_ = false;
    bool shown_ = false;
    bool ticked_ = false;
    bool timed_out_ = false;
    IFileDialog* dialog_ = nullptr;
    ULONGLONG deadline_ = 0;
};

} // namespace tc::app
