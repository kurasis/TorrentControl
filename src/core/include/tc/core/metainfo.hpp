#pragma once

// Lossless metainfo layer (specification sections 7 and 8).
//
// A Metainfo keeps the exact file bytes. The `info` dictionary is never
// re-encoded: identifiers are computed over its raw slice and outer-only
// edits copy that slice verbatim into the output.

#include "tc/core/bencode.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tc::core {

enum class MetainfoFormat { V1, V2, Hybrid };

std::string_view to_string(MetainfoFormat f) noexcept;

using Sha1Digest = std::array<std::uint8_t, 20>;
using Sha256Digest = std::array<std::uint8_t, 32>;

struct InfoHashes {
    // SHA-1 of the raw info bytes. Present for v1 and hybrid torrents.
    std::optional<Sha1Digest> v1;
    // Full SHA-256 of the raw info bytes. Present for v2 and hybrid torrents.
    std::optional<Sha256Digest> v2;
};

struct MetainfoLimits {
    std::size_t max_bytes = 64ull * 1024 * 1024;
    bencode::Limits bencode;
};

class Metainfo {
public:
    // Parses and classifies metainfo. Throws CoreError:
    //  - ResourceLimit for oversized input,
    //  - InvalidMetainfo for malformed structure or a missing/invalid `info`,
    //  - UnsupportedFormat for an unknown `meta version`.
    static Metainfo parse(std::string bytes, MetainfoLimits const& limits = {});

    std::string const& bytes() const noexcept { return bytes_; }
    bencode::Value const& root() const noexcept { return root_; }
    bencode::Value const& info() const noexcept { return *root_.find("info"); }
    std::string_view raw_info() const noexcept;

    MetainfoFormat format() const noexcept { return format_; }
    InfoHashes const& info_hashes() const noexcept { return hashes_; }

    // UTF-8 decoded name when `info.name` is present (no validation of encoding).
    std::string name() const;

    // A legacy BEP 30 `root hash` key is present next to ordinary v1 pieces.
    // It is preserved but never generated or trusted (section 7.3).
    bool has_legacy_root_hash() const noexcept { return legacy_root_hash_; }

private:
    std::string bytes_;
    bencode::Value root_;
    MetainfoFormat format_ = MetainfoFormat::V1;
    InfoHashes hashes_;
    bool legacy_root_hash_ = false;
};

// One file of the payload layout described by a metainfo, in hashing order:
// the v1 file list (including BEP 47 padding entries) for v1 and hybrid
// torrents, the sorted file tree for pure v2.
struct MetainfoFile {
    // Components below the torrent root; empty for a single-file torrent.
    std::vector<std::string> path;
    // "name/a/b" style path ("name" for a single-file torrent).
    std::string torrent_path;
    std::uint64_t length = 0;
    bool pad = false;
    // v2 `pieces root` (v2 and hybrid, non-empty real files).
    std::optional<Sha256Digest> pieces_root;
};

// Extracts the payload layout. Throws CoreError(InvalidMetainfo) when the
// file description is structurally invalid or unsafe (absolute or relative
// path components, separators inside components, inconsistent hybrid lists).
std::vector<MetainfoFile> metainfo_files(Metainfo const& m);

// Metainfo validation without reading payload (section 9.3): structure,
// counts, identifiers, safe paths, hybrid v1/v2 consistency, and v2 piece
// layers against their file roots. Returns human-readable problems; empty
// means valid.
std::vector<std::string> validate_metainfo(Metainfo const& m);

Sha1Digest sha1(std::string_view data);
Sha256Digest sha256(std::string_view data);

std::string to_hex(Sha1Digest const& d);
std::string to_hex(Sha256Digest const& d);

// Outer-only edit: each key maps to a replacement value, or to std::nullopt
// to remove it. Keys `info` and `piece layers` are rejected (they are computed
// or raw-preserved). Unchanged values are copied byte-for-byte from the
// original; the raw `info` slice is always copied verbatim, so all
// identifiers stay unchanged.
using OuterEdit = std::map<std::string, std::optional<bencode::Value>>;

std::string apply_outer_edit(Metainfo const& original, OuterEdit const& edit);

// Edit of non-layout keys inside `info` (section 8), e.g. `source` or
// `private`. Each key maps to a replacement value or std::nullopt to remove
// it. Layout keys (name, piece length, pieces, length, files, meta version,
// file tree) require a rebuild and are rejected. Unchanged values, the outer
// dictionary and the piece layers are copied byte-for-byte, so existing
// payload hashes are reused; the identifiers change.
using InfoEdit = std::map<std::string, std::optional<bencode::Value>>;

struct InfoEditOptions {
    // BEP 35 signatures cover the info dictionary and become invalid. They are
    // removed only with this explicit choice; otherwise the edit is refused.
    bool remove_invalidated_signatures = false;
};

struct InfoEditResult {
    std::string bytes;
    InfoHashes old_hashes;
    InfoHashes new_hashes;
    bool removed_signatures = false;
};

InfoEditResult apply_info_edit(Metainfo const& original, InfoEdit const& edit, InfoEditOptions const& options = {});

struct MagnetOptions {
    bool include_name = true;
    bool include_trackers = true;
    bool include_web_seeds = false;
};

// Public magnet export (section 8): v1 `urn:btih`, v2 `urn:btmh:1220...`, and
// both topics for hybrid torrents. Parameters are percent-encoded once.
std::string make_magnet(Metainfo const& m, MagnetOptions const& options = {});

} // namespace tc::core
