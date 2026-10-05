#pragma once

// Source manifest (specification section 5). A manifest is an explicit mapping
// from native source files to torrent-relative path components. The source
// location never has to match the torrent layout: files from unrelated
// directories or volumes can be combined under one virtual root without being
// copied or renamed.
//
// A manifest is frozen before hashing: every entry records what was observed
// about its source (identity, size, last-write time) so later reads can
// detect replaced, truncated or modified files (section 9.2).

#include "tc/core/error.hpp"
#include "tc/core/native_fs.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace tc::core {

// UTF-8 conversions that behave identically on Windows (UTF-16 paths) and POSIX.
std::string to_utf8(std::filesystem::path const& p);
std::filesystem::path path_from_utf8(std::string_view utf8);

struct EntryFlags {
    bool hidden = false;
    bool system = false;
    bool cloud_placeholder = false;
    // Reading the file would download it from a cloud provider.
    bool requires_hydration = false;
};

struct ManifestEntry {
    // Native-owned identifier; never a path supplied by the UI.
    std::string source_id;
    std::filesystem::path source_path;
    // UTF-8 components below the torrent root. Empty in single-file mode.
    std::vector<std::string> torrent_path;
    // Expected length captured when the manifest was frozen.
    std::uint64_t length = 0;
    // Identity and timestamps captured when the manifest was frozen. An
    // observation without a valid identity (for example a hand-built test
    // manifest) disables identity checks for this entry only.
    native::FileObservation observed = {};
    EntryFlags flags = {};
    // Why the file is in the manifest ("selected file", "in selected folder", ...).
    std::string inclusion_reason = {};
};

enum class LayoutMode {
    // The torrent describes exactly one file named `name`.
    SingleFile,
    // The torrent has a root directory `name` containing `torrent_path` entries.
    Directory,
};

enum class SkipKind {
    // Matched an exclusion rule (default or user-defined).
    Excluded,
    // The current output file or one of its temporary files.
    OutputFile,
    SymbolicLink,
    Junction,
    ReparsePoint,
    SpecialFile,
    // A subfolder of a non-recursive selection.
    NotRecursive,
    // Cloud placeholder skipped by the selected cloud policy.
    CloudPlaceholder,
    // Followed-link traversal only: a link back into an ancestor folder.
    LinkCycle,
    // Followed-link traversal only: a link target outside the selected root.
    OutsideRoot,
    DepthLimit,
};

std::string_view to_string(SkipKind kind) noexcept;

struct SkippedItem {
    std::filesystem::path path;
    SkipKind kind = SkipKind::Excluded;
    // User-visible explanation, e.g. the matching rule's reason.
    std::string reason;
};

// A selected item that could not be read. It blocks creation until the user
// excludes it or resolves the error (section 5.2); it is never silently omitted.
struct UnreadableItem {
    std::filesystem::path path;
    std::string message;
    std::optional<int> os_error;
};

struct Manifest {
    std::string name;
    LayoutMode mode = LayoutMode::Directory;
    std::vector<ManifestEntry> entries;
    // Items seen during enumeration but not included, with a visible reason.
    std::vector<SkippedItem> skipped;
    std::vector<UnreadableItem> unreadable;
    // Incremented by every destination edit; recorded with created output.
    std::uint64_t revision = 1;

    std::uint64_t total_length() const;
    // "name/a/b.txt" style torrent path of an entry ("name" in single-file mode).
    std::string torrent_path_string(ManifestEntry const& e) const;
    ManifestEntry const* find(std::string_view source_id) const;
};

enum class Severity { Warning, Error };

struct ManifestIssue {
    ErrorCode code;
    std::string message;
    Severity severity = Severity::Error;
    // Source ID of the entry the issue refers to, when there is one.
    std::string source_id = {};
};

// Initial manifest-size safeguards (section 15).
struct ManifestLimits {
    std::size_t warn_files = 100'000;
    std::size_t max_files = 1'000'000;
};

// Validates names and destination paths against the torrent and Windows rules
// in section 5.2: empty/relative components, invalid characters, reserved
// device names, trailing dots or spaces, exact and case-insensitive
// collisions, file-versus-directory collisions, unreadable selected items,
// empty payloads and manifest-size limits. Warnings do not block creation.
std::vector<ManifestIssue> validate_manifest(Manifest const& m, ManifestLimits const& limits = {});

// Throws CoreError with the first error-severity issue.
void require_valid_manifest(Manifest const& m, ManifestLimits const& limits = {});

// Case folding used for destination collision checks. On Windows this is the
// operating system's invariant uppercase mapping; elsewhere a built-in table
// covering Latin, Greek, Cyrillic, Armenian and fullwidth Latin letters.
// Unicode normalization is deliberately not applied.
std::string fold_case(std::string_view utf8);

// Glob match used by exclusion rules. `*` and `?` do not cross '/', `**`
// matches any sequence including '/'. ASCII letters compare case-insensitively.
// A pattern without '/' is matched against the last path component only.
bool glob_match(std::string_view pattern, std::string_view relative_path);

struct ExclusionRule {
    std::string pattern;
    std::string reason;
};

// Visible default exclusions for application outputs, temporary files and
// project files. Ordinary `.torrent` files are deliberately not excluded.
std::vector<ExclusionRule> default_exclusions();

enum class CloudPolicy {
    // Include placeholders; creation still requires explicit consent to hydrate
    // files that are not available locally (CreateOptions::allow_hydration).
    Include,
    // List placeholders that need hydration as skipped.
    SkipUnavailable,
};

struct ScanOptions {
    bool recursive = true;
    bool use_default_exclusions = true;
    std::vector<ExclusionRule> exclusions = {};
    // Exact paths that are never included (current output and its temp files).
    std::vector<std::filesystem::path> excluded_paths = {};
    CloudPolicy cloud_policy = CloudPolicy::Include;
    // Advanced traversal: follow symbolic links and junctions whose target is
    // inside the selected root, with identity-based cycle detection.
    bool follow_links = false;
    int max_depth = 64;
};

// A selected file produces single-file mode with its basename. A selected
// folder becomes the torrent root and preserves relative descendants.
// Symbolic links, junctions and other reparse points are not followed by
// default; they are listed in Manifest::skipped with a reason. Never reads
// file contents. Throws CoreError(Cancelled) when `stop` is requested.
Manifest scan_source(std::filesystem::path const& source, ScanOptions const& options = {}, std::stop_token stop = {});

// Changes an entry's destination components (directory mode only). Source
// files are never renamed. Throws InvalidArgument for an unknown source ID.
void set_destination(Manifest& m, std::string_view source_id, std::vector<std::string> torrent_path);

// Re-observes every entry. Returns SourceMissing / SourceChanged errors for
// entries whose file disappeared, was replaced, or changed size or last-write
// time since the manifest was frozen. This is best-effort detection, not a
// snapshot guarantee (section 9.2).
std::vector<ManifestIssue> recheck_sources(Manifest const& m);

// Builds a manifest for a virtual collection: arbitrary source files mapped to
// arbitrary destination components below a user-visible root `name`.
class ManifestBuilder {
public:
    explicit ManifestBuilder(std::string name);

    // Records the file's current observation. Throws SourceMissing for a
    // missing file and SourceUnreadable for links and non-regular files.
    ManifestBuilder& add_file(std::filesystem::path const& source, std::vector<std::string> destination);

    // Finalizes the manifest (sorted, with source IDs). The builder is left empty.
    Manifest build();

private:
    Manifest manifest_;
};

} // namespace tc::core
