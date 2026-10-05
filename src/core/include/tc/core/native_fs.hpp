#pragma once

// Thin native filesystem layer (specification section 5.2).
//
// std::filesystem hides details the manifest needs: Windows reparse tags
// (junctions versus symbolic links versus cloud placeholders), file identity
// for hard-link and replacement detection, and timestamps in their native
// resolution. Nothing here opens file contents, so enumeration and
// observation never trigger a cloud download.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tc::core::native {

// Identity of a file object: Windows volume serial number plus file ID, or
// POSIX device plus inode. Hard links share an identity; a file replaced by
// rename or delete-and-recreate gets a new one.
struct FileIdentity {
    std::uint64_t volume = 0;
    std::array<std::uint8_t, 16> id{};
    bool valid = false;

    friend bool operator==(FileIdentity const&, FileIdentity const&) = default;
};

// What was observed about a file at one moment. Timestamps are in native
// units (100 ns ticks on Windows, nanoseconds on POSIX) and are only ever
// compared with other observations from the same platform.
struct FileObservation {
    std::uint64_t size = 0;
    std::int64_t last_write = 0;
    // Metadata change time (Windows ChangeTime, POSIX ctime) when available.
    std::optional<std::int64_t> change_time;
    FileIdentity identity;
    std::uint32_t link_count = 1;
};

// Fields that differ between two observations of what should be the same,
// unchanged file. Empty when nothing relevant changed. Link count and change
// time are not compared: creating a hard link elsewhere does not change data.
std::vector<std::string> describe_changes(FileObservation const& before, FileObservation const& after);

enum class EntryKind {
    Regular,
    Directory,
    // Symbolic link (file or directory).
    Symlink,
    // NTFS mount point / directory junction.
    Junction,
    // Any other reparse point that is not a cloud placeholder (deduplication
    // and similar tags are reported as Regular because their data is local).
    OtherReparsePoint,
    // Devices, sockets, FIFOs and other non-regular objects.
    Other,
};

std::string_view to_string(EntryKind kind) noexcept;

struct EntryInfo {
    std::filesystem::path path;
    EntryKind kind = EntryKind::Other;
    std::uint32_t reparse_tag = 0;
    bool hidden = false;
    bool system = false;
    // Managed by a cloud sync provider (OneDrive and other cloud-files tags).
    bool cloud_placeholder = false;
    // Data is not present locally; reading it would start a download.
    bool requires_hydration = false;
    // Size and last-write time as reported by the directory entry; identity is
    // only filled in by observe().
    FileObservation observation;
};

// Pure classification of Windows attributes and reparse tag, exposed so the
// policy can be tested on every platform.
struct WindowsClassification {
    EntryKind kind = EntryKind::Other;
    bool cloud_placeholder = false;
    bool requires_hydration = false;
};
WindowsClassification classify_windows(std::uint32_t attributes, std::uint32_t reparse_tag) noexcept;

// Information about `path` itself; a link is not followed. Returns
// std::nullopt when the path does not exist. Throws CoreError on other errors.
std::optional<EntryInfo> stat_entry(std::filesystem::path const& path);

// Entries of a directory (excluding "." and ".."), not following links.
// Throws CoreError(SourceUnreadable) when the directory cannot be listed.
std::vector<EntryInfo> list_directory(std::filesystem::path const& dir);

// Current observation of `path` including identity. Does not follow a final
// link and does not read file data. Returns std::nullopt when the path does
// not exist. Throws CoreError(SourceUnreadable) on other errors.
std::optional<FileObservation> observe(std::filesystem::path const& path);

#ifdef _WIN32
// Extended-length form (\\?\ or \\?\UNC\) so paths beyond MAX_PATH work even
// without the process-wide long-path opt-in.
std::wstring extended_path(std::filesystem::path const& p);
// Observation of an open file handle (HANDLE).
FileObservation observe_handle(void* handle);
#else
// Observation of an open file descriptor.
FileObservation observe_fd(int fd);
#endif

} // namespace tc::core::native
