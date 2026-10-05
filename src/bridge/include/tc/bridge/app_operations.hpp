#pragma once

// The application's bridge operations (specification section 13.1) mapped
// onto the native service. Paths for privileged operations never come from
// the page: files are chosen in native dialogs or attached natively by
// drag and drop, and later commands refer to native-owned IDs.

#include "tc/bridge/protocol.hpp"
#include "tc/service/app_service.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tc::bridge {

// Native services the Windows host provides (dialogs and shell actions).
class HostServices {
public:
    virtual ~HostServices() = default;

    enum class OpenKind {
        Files,         // payload files (multi-select)
        Folder,        // one payload folder
        Torrent,       // a .torrent to open
        Project,       // a .tcproject to open
        PayloadFolder, // the folder holding a torrent's data, for verification
        OutputFolder,  // destination folder for a batch
    };
    enum class SaveKind { Torrent, Project, Magnet, Profile };

    // Empty when the user cancels.
    virtual std::vector<std::filesystem::path> pick_open(OpenKind kind) = 0;
    virtual std::optional<std::filesystem::path> pick_save(
        SaveKind kind, std::string const& suggested_name, std::filesystem::path const& folder) = 0;
    // Opens Explorer with the file selected.
    virtual void show_in_folder(std::filesystem::path const& file) = 0;
    // Opens the file through its Windows file association, with the path as a
    // separate argument (never a constructed command line).
    virtual bool open_with_default_app(std::filesystem::path const& file) = 0;
    // Opens an http(s) URL in the system browser.
    virtual bool open_url(std::string const& url) = 0;
};

void register_app_operations(Dispatcher& d, service::AppService& app, HostServices& host);

} // namespace tc::bridge
