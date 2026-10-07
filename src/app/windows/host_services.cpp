#include "host_services.hpp"

#include "webview_host.hpp"
#include "tc/core/manifest.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <array>

namespace tc::app {

namespace {


constexpr std::array<COMDLG_FILTERSPEC, 2> torrent_filter{{{L"Torrent files (*.torrent)", L"*.torrent"}, {L"All files", L"*.*"}}};
constexpr std::array<COMDLG_FILTERSPEC, 2> project_filter{{{L"TorrentControl projects (*.tcproject)", L"*.tcproject"}, {L"All files", L"*.*"}}};
constexpr std::array<COMDLG_FILTERSPEC, 2> magnet_filter{{{L"Text files (*.txt)", L"*.txt"}, {L"All files", L"*.*"}}};
constexpr std::array<COMDLG_FILTERSPEC, 2> profile_filter{{{L"Profiles (*.json)", L"*.json"}, {L"All files", L"*.*"}}};

std::filesystem::path item_path(IShellItem* item)
{
    wil::unique_cotaskmem_string path;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    return std::filesystem::path(path.get());
}

void set_folder(IFileDialog* dialog, std::filesystem::path const& folder)
{
    if (folder.empty()) return;
    wil::com_ptr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) dialog->SetFolder(item.get());
}

} // namespace

HRESULT WindowsHostServices::show_dialog(IFileDialog* dialog)
{
    return dialog_runner_ ? dialog_runner_(dialog) : dialog->Show(owner_);
}

std::vector<std::filesystem::path> WindowsHostServices::pick_open(OpenKind kind)
{
    if (!ui_.is_owner_thread()) return ui_.invoke([this, kind] { return pick_open(kind); });
    std::vector<std::filesystem::path> result;
    wil::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;

    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    // Only real file-system items; no shell libraries or virtual folders.
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    switch (kind) {
    case OpenKind::Files:
        options |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
        dialog->SetTitle(L"Add files");
        break;
    case OpenKind::Folder:
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Add folder");
        break;
    case OpenKind::Torrent:
        options |= FOS_FILEMUSTEXIST;
        dialog->SetFileTypes(static_cast<UINT>(torrent_filter.size()), torrent_filter.data());
        dialog->SetTitle(L"Open torrent");
        break;
    case OpenKind::Project:
        options |= FOS_FILEMUSTEXIST;
        dialog->SetFileTypes(static_cast<UINT>(project_filter.size()), project_filter.data());
        dialog->SetTitle(L"Open project");
        break;
    case OpenKind::PayloadFolder:
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Choose the folder that holds the downloaded data");
        break;
    case OpenKind::OutputFolder:
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Choose where to save the torrents");
        break;
    }
    dialog->SetOptions(options);
    if (FAILED(show_dialog(dialog.get()))) return result; // includes cancel

    wil::com_ptr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(&items))) return result;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        wil::com_ptr<IShellItem> item;
        if (SUCCEEDED(items->GetItemAt(i, &item))) {
            auto path = item_path(item.get());
            if (!path.empty()) result.push_back(std::move(path));
        }
    }
    return result;
}

std::optional<std::filesystem::path> WindowsHostServices::pick_save(
    SaveKind kind, std::string const& suggested_name, std::filesystem::path const& folder)
{
    if (!ui_.is_owner_thread()) return ui_.invoke([this, kind, suggested_name, folder] { return pick_save(kind, suggested_name, folder); });
    wil::com_ptr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;

    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    // The application reports and handles existing files itself (replace
    // policy), so the shell prompt is kept only as a second confirmation.
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_OVERWRITEPROMPT;
    dialog->SetOptions(options);

    switch (kind) {
    case SaveKind::Torrent:
        dialog->SetFileTypes(static_cast<UINT>(torrent_filter.size()), torrent_filter.data());
        dialog->SetDefaultExtension(L"torrent");
        dialog->SetTitle(L"Save torrent as");
        break;
    case SaveKind::Project:
        dialog->SetFileTypes(static_cast<UINT>(project_filter.size()), project_filter.data());
        dialog->SetDefaultExtension(L"tcproject");
        dialog->SetTitle(L"Save project as");
        break;
    case SaveKind::Magnet:
        dialog->SetFileTypes(static_cast<UINT>(magnet_filter.size()), magnet_filter.data());
        dialog->SetDefaultExtension(L"txt");
        dialog->SetTitle(L"Save magnet link as");
        break;
    case SaveKind::Profile:
        dialog->SetFileTypes(static_cast<UINT>(profile_filter.size()), profile_filter.data());
        dialog->SetDefaultExtension(L"json");
        dialog->SetTitle(L"Export profile");
        break;
    }
    set_folder(dialog.get(), folder);
    if (!suggested_name.empty()) dialog->SetFileName(to_wide(suggested_name).c_str());

    if (FAILED(show_dialog(dialog.get()))) return std::nullopt;
    wil::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::nullopt;
    auto path = item_path(item.get());
    if (path.empty()) return std::nullopt;
    return path;
}

void WindowsHostServices::show_in_folder(std::filesystem::path const& file)
{
    if (!ui_.is_owner_thread()) return ui_.invoke([this, file] { show_in_folder(file); });
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(file.c_str(), nullptr, &pidl, 0, nullptr)) && pidl != nullptr) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
        return;
    }
    // The file is gone: open its folder instead, if that still exists.
    std::wstring const folder = file.parent_path().wstring();
    ShellExecuteW(owner_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool WindowsHostServices::open_with_default_app(std::filesystem::path const& file)
{
    if (core::fold_case(core::to_utf8(file.extension())) != ".TORRENT") return false;
    if (!ui_.is_owner_thread()) return ui_.invoke([this, file] { return open_with_default_app(file); });
    // The path is the "file" argument, not part of a command line, so it is
    // never parsed as parameters.
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner_;
    info.lpVerb = L"open";
    std::wstring const path = file.wstring();
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

bool WindowsHostServices::open_url(std::string const& url)
{
    if (!ui_.is_owner_thread()) return ui_.invoke([this, url] { return open_url(url); });
    // The bridge only passes http(s) URLs without control characters.
    std::wstring const wide = to_wide(url);
    auto const result = reinterpret_cast<INT_PTR>(ShellExecuteW(owner_, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
}

} // namespace tc::app
