#pragma once

// Local verification (specification section 9.3).
//
// Level 1, metainfo validation, is validate_metainfo() in metainfo.hpp and
// never reads payload data. Level 2, implemented here, reads every mapped
// source byte once and compares the torrent's v1 piece hashes and v2 file
// roots (both for hybrid torrents). Padding is synthesized, never read.

#include "tc/core/metainfo.hpp"
#include "tc/core/payload_source.hpp"
#include "tc/core/torrent_engine.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

namespace tc::core {

// Torrent path ("name/a/b", or "name" for a single-file torrent) to the native
// file that should hold its data.
using PayloadMapping = std::map<std::string, std::filesystem::path>;

// Maps every file of `m` below `root`: the root folder for a multi-file
// torrent (its own name does not have to match), or the file itself for a
// single-file torrent. Embedded paths are never used outside `root`.
PayloadMapping map_to_root(Metainfo const& m, std::filesystem::path const& root);

enum class VerifyStatus {
    Ok,
    // No mapping or the file does not exist.
    Missing,
    Unreadable,
    // The file exists but its length differs from the torrent.
    SizeMismatch,
    // Readable with the right length, but hashes do not match.
    Corrupt,
};

std::string_view to_string(VerifyStatus s) noexcept;

struct VerifyFileResult {
    std::string torrent_path;
    std::uint64_t length = 0;
    VerifyStatus status = VerifyStatus::Ok;
    std::string message;
    // v1 pieces overlapping this file that failed (v1 and hybrid).
    std::uint64_t bad_v1_pieces = 0;
    // v2 pieces of this file that failed (v2 and hybrid).
    std::uint64_t bad_v2_pieces = 0;
};

struct VerifyResult {
    bool ok = false;
    MetainfoFormat format = MetainfoFormat::V1;
    // Problems found by metainfo validation; payload is not read when present.
    std::vector<std::string> metainfo_problems;
    std::uint64_t v1_pieces_total = 0;
    std::uint64_t v1_pieces_bad = 0;
    std::uint64_t v2_files_checked = 0;
    std::uint64_t v2_files_bad = 0;
    std::uint64_t payload_bytes_read = 0;
    // Real files only (padding is not listed).
    std::vector<VerifyFileResult> files;
};

struct VerifyOptions {
    std::size_t buffer_budget = 128u * 1024 * 1024;
    std::size_t read_buffer_size = 4u * 1024 * 1024;
    int hash_threads = 0;
    // Optional cooperative pause (not owned; must outlive the call).
    PauseControl* pause = nullptr;
};

// Full payload verification. Missing or unreadable files are reported per
// file and do not stop the check of other files. Throws CoreError(Cancelled)
// when `stop` is requested.
VerifyResult verify_payload(Metainfo const& m, PayloadMapping const& mapping, PayloadSource& source,
    std::stop_token stop = {}, ProgressCallback const& progress = {}, VerifyOptions const& options = {});

} // namespace tc::core
