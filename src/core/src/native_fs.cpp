#include "tc/core/native_fs.hpp"

#include "tc/core/error.hpp"
#include "tc/core/manifest.hpp"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace tc::core::native {

namespace {

// Values from the Windows SDK, repeated so the classification compiles and is
// tested on every platform.
#ifdef _WIN32
constexpr std::uint32_t attr_hidden = 0x2;
constexpr std::uint32_t attr_system = 0x4;
#endif
constexpr std::uint32_t attr_directory = 0x10;
constexpr std::uint32_t attr_device = 0x40;
constexpr std::uint32_t attr_reparse_point = 0x400;
constexpr std::uint32_t attr_offline = 0x1000;
constexpr std::uint32_t attr_recall_on_open = 0x40000;
constexpr std::uint32_t attr_recall_on_data_access = 0x400000;

constexpr std::uint32_t tag_mount_point = 0xA0000003;
constexpr std::uint32_t tag_symlink = 0xA000000C;
constexpr std::uint32_t tag_lx_symlink = 0xA000001D;
constexpr std::uint32_t tag_dedup = 0x80000013;
constexpr std::uint32_t tag_wof = 0x80000017;
constexpr std::uint32_t tag_onedrive = 0x80000021;
constexpr std::uint32_t tag_cloud = 0x9000001A;
constexpr std::uint32_t tag_cloud_mask = 0xFFFF0FFF; // IO_REPARSE_TAG_CLOUD_1..F

[[noreturn]] void unreadable(fs::path const& p, std::string const& what, int os_error)
{
    throw CoreError(ErrorCode::SourceUnreadable, what + ": " + to_utf8(p.filename()), os_error);
}

#ifdef _WIN32

std::int64_t ticks(FILETIME const& ft)
{
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

bool not_found(DWORD err)
{
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND || err == ERROR_BAD_NETPATH
        || err == ERROR_BAD_NET_NAME || err == ERROR_INVALID_NAME;
}

EntryInfo make_entry(fs::path path, DWORD attributes, DWORD reparse_tag, DWORD size_high, DWORD size_low,
    FILETIME const& last_write)
{
    EntryInfo e;
    e.path = std::move(path);
    WindowsClassification const c = classify_windows(attributes, reparse_tag);
    e.kind = c.kind;
    e.cloud_placeholder = c.cloud_placeholder;
    e.requires_hydration = c.requires_hydration;
    e.reparse_tag = (attributes & attr_reparse_point) ? reparse_tag : 0;
    e.hidden = (attributes & attr_hidden) != 0;
    e.system = (attributes & attr_system) != 0;
    e.observation.size = (static_cast<std::uint64_t>(size_high) << 32) | size_low;
    e.observation.last_write = ticks(last_write);
    return e;
}

#else

std::int64_t nanos(struct timespec const& ts)
{
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

FileObservation observation_from_stat(struct stat const& st)
{
    FileObservation o;
    o.size = S_ISREG(st.st_mode) ? static_cast<std::uint64_t>(st.st_size) : 0;
    o.last_write = nanos(st.st_mtim);
    o.change_time = nanos(st.st_ctim);
    o.identity.valid = true;
    o.identity.volume = static_cast<std::uint64_t>(st.st_dev);
    auto const ino = static_cast<std::uint64_t>(st.st_ino);
    std::memcpy(o.identity.id.data(), &ino, sizeof ino);
    o.link_count = static_cast<std::uint32_t>(st.st_nlink);
    return o;
}

EntryInfo entry_from_stat(fs::path path, struct stat const& st)
{
    EntryInfo e;
    e.path = std::move(path);
    if (S_ISREG(st.st_mode)) e.kind = EntryKind::Regular;
    else if (S_ISDIR(st.st_mode)) e.kind = EntryKind::Directory;
    else if (S_ISLNK(st.st_mode)) e.kind = EntryKind::Symlink;
    else e.kind = EntryKind::Other;
    std::string const name = to_utf8(e.path.filename());
    e.hidden = name.size() > 1 && name.front() == '.';
    e.observation = observation_from_stat(st);
    return e;
}

#endif

} // namespace

std::string_view to_string(EntryKind kind) noexcept
{
    switch (kind) {
    case EntryKind::Regular: return "file";
    case EntryKind::Directory: return "directory";
    case EntryKind::Symlink: return "symbolic link";
    case EntryKind::Junction: return "junction";
    case EntryKind::OtherReparsePoint: return "reparse point";
    case EntryKind::Other: return "special file";
    }
    return "unknown";
}

std::vector<std::string> describe_changes(FileObservation const& before, FileObservation const& after)
{
    std::vector<std::string> changes;
    if (before.identity.valid && after.identity.valid && before.identity != after.identity)
        changes.emplace_back("the file was replaced");
    if (before.size != after.size) changes.emplace_back("size changed");
    if (before.last_write != after.last_write) changes.emplace_back("last write time changed");
    return changes;
}

WindowsClassification classify_windows(std::uint32_t attributes, std::uint32_t reparse_tag) noexcept
{
    WindowsClassification c;
    bool const directory = (attributes & attr_directory) != 0;
    EntryKind const plain = directory ? EntryKind::Directory : EntryKind::Regular;

    if (attributes & attr_reparse_point) {
        if (reparse_tag == tag_symlink || reparse_tag == tag_lx_symlink) {
            c.kind = EntryKind::Symlink;
            return c;
        }
        if (reparse_tag == tag_mount_point) {
            c.kind = EntryKind::Junction;
            return c;
        }
        if ((reparse_tag & tag_cloud_mask) == tag_cloud || reparse_tag == tag_onedrive) {
            c.kind = plain;
            c.cloud_placeholder = true;
        } else if (reparse_tag == tag_dedup || reparse_tag == tag_wof) {
            // Data deduplication and compressed (WOF) files keep their data on
            // the local volume and read like ordinary files.
            c.kind = plain;
        } else {
            c.kind = EntryKind::OtherReparsePoint;
            return c;
        }
    } else if (directory) {
        c.kind = EntryKind::Directory;
    } else if (attributes & attr_device) {
        c.kind = EntryKind::Other;
        return c;
    } else {
        c.kind = EntryKind::Regular;
    }

    if (c.kind == EntryKind::Regular
        && (attributes & (attr_recall_on_data_access | attr_recall_on_open | attr_offline)) != 0)
        c.requires_hydration = true;
    return c;
}

#ifdef _WIN32

std::wstring extended_path(fs::path const& p)
{
    std::wstring s = p.lexically_normal().native();
    if (s.starts_with(L"\\\\?\\")) return s;
    if (s.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + s.substr(2);
    return L"\\\\?\\" + s;
}

std::optional<EntryInfo> stat_entry(fs::path const& path)
{
    std::wstring const ext = extended_path(path);
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(ext.c_str(), GetFileExInfoStandard, &d)) {
        DWORD const err = GetLastError();
        if (not_found(err)) return std::nullopt;
        unreadable(path, "Cannot read file attributes", static_cast<int>(err));
    }
    DWORD tag = 0;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(ext.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            tag = fd.dwReserved0;
            FindClose(h);
        }
    }
    return make_entry(path, d.dwFileAttributes, tag, d.nFileSizeHigh, d.nFileSizeLow, d.ftLastWriteTime);
}

std::vector<EntryInfo> list_directory(fs::path const& dir)
{
    std::wstring pattern = extended_path(dir);
    if (!pattern.ends_with(L'\\')) pattern += L'\\';
    pattern += L'*';

    std::vector<EntryInfo> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD const err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return out;
        unreadable(dir, "Cannot list folder", static_cast<int>(err));
    }
    do {
        std::wstring_view const name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        out.push_back(make_entry(dir / fs::path(std::wstring(name)), fd.dwFileAttributes, fd.dwReserved0,
            fd.nFileSizeHigh, fd.nFileSizeLow, fd.ftLastWriteTime));
    } while (FindNextFileW(h, &fd));
    DWORD const err = GetLastError();
    FindClose(h);
    if (err != ERROR_NO_MORE_FILES) unreadable(dir, "Cannot list folder", static_cast<int>(err));
    return out;
}

FileObservation observe_handle(void* handle)
{
    auto const h = static_cast<HANDLE>(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info))
        throw CoreError(ErrorCode::SourceUnreadable, "Cannot read file information", static_cast<int>(GetLastError()));

    FileObservation o;
    o.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    o.last_write = ticks(info.ftLastWriteTime);
    o.link_count = info.nNumberOfLinks;
    o.identity.valid = true;

    FILE_ID_INFO id{};
    if (GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id)) {
        // 128-bit IDs (ReFS) are not representable in the legacy index.
        o.identity.volume = id.VolumeSerialNumber;
        static_assert(sizeof id.FileId.Identifier == 16);
        std::memcpy(o.identity.id.data(), id.FileId.Identifier, 16);
    } else {
        o.identity.volume = info.dwVolumeSerialNumber;
        std::uint64_t const index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        std::memcpy(o.identity.id.data(), &index, sizeof index);
    }

    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic))
        o.change_time = basic.ChangeTime.QuadPart;
    return o;
}

std::optional<FileObservation> observe(fs::path const& path)
{
    // Attribute-only access with the reparse point itself opened: this never
    // recalls cloud data and never follows a link.
    HANDLE h = CreateFileW(extended_path(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD const err = GetLastError();
        if (not_found(err)) return std::nullopt;
        unreadable(path, "Cannot open file", static_cast<int>(err));
    }
    try {
        FileObservation o = observe_handle(h);
        CloseHandle(h);
        return o;
    } catch (...) {
        CloseHandle(h);
        throw;
    }
}

#else

std::optional<EntryInfo> stat_entry(fs::path const& path)
{
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        int const err = errno;
        if (err == ENOENT || err == ENOTDIR) return std::nullopt;
        unreadable(path, std::string("Cannot read file attributes (") + std::strerror(err) + ")", err);
    }
    return entry_from_stat(path, st);
}

std::vector<EntryInfo> list_directory(fs::path const& dir)
{
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        int const err = errno;
        unreadable(dir, std::string("Cannot list folder (") + std::strerror(err) + ")", err);
    }
    std::vector<EntryInfo> out;
    try {
        errno = 0;
        while (dirent* de = ::readdir(d)) {
            std::string_view const name(de->d_name);
            if (name == "." || name == "..") continue;
            fs::path const child = dir / std::string(name);
            // An entry removed between readdir and lstat is simply gone.
            if (auto info = stat_entry(child)) out.push_back(std::move(*info));
            errno = 0;
        }
        if (errno != 0) {
            int const err = errno;
            unreadable(dir, std::string("Cannot list folder (") + std::strerror(err) + ")", err);
        }
    } catch (...) {
        ::closedir(d);
        throw;
    }
    ::closedir(d);
    return out;
}

FileObservation observe_fd(int fd)
{
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        int const err = errno;
        throw CoreError(ErrorCode::SourceUnreadable, std::string("Cannot read file information: ") + std::strerror(err), err);
    }
    return observation_from_stat(st);
}

std::optional<FileObservation> observe(fs::path const& path)
{
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        int const err = errno;
        if (err == ENOENT || err == ENOTDIR) return std::nullopt;
        unreadable(path, std::string("Cannot read file attributes (") + std::strerror(err) + ")", err);
    }
    return observation_from_stat(st);
}

#endif

} // namespace tc::core::native
