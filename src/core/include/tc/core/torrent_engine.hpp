#pragma once

// TorrentEngine (specification sections 3.1, 6 and 9).
//
// libtorrent's create_torrent is used for the canonical layout (file order,
// BEP 47 padding) and for serialization. Payload hashing is done here, in a
// single streaming pass over a PayloadSource, so that:
//  - source files may live anywhere (virtual-to-physical mapping),
//  - each real payload byte is read exactly once, even for hybrid torrents,
//  - padding is synthesized in memory and never read from or written to disk,
//  - cancellation is a cooperative request checked between bounded reads.
// No libtorrent session is created; hashing performs no network activity.

#include "tc/core/manifest.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/pause.hpp"
#include "tc/core/payload_source.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace tc::core {

enum class TorrentFormat { V1, V2, Hybrid };

std::string_view to_string(TorrentFormat f) noexcept;

// Automatic piece-size policy version 1 (section 6.2).
struct PieceSizeDecision {
    static constexpr int policy_version = 1;
    int piece_length = 0;
    std::uint64_t logical_pieces = 0;
    std::uint64_t estimated_hash_bytes = 0;
    std::uint64_t padding_bytes = 0;
    bool exceeds_piece_count_target = false;
    bool exceeds_hash_size_target = false;
    // Virtual padding is more than 10% of the payload.
    bool padding_warning = false;
};

PieceSizeDecision choose_piece_size(Manifest const& manifest, TorrentFormat format);

// Describes the layout for an explicit piece length without hashing.
PieceSizeDecision evaluate_piece_size(Manifest const& manifest, TorrentFormat format, int piece_length);

struct CreateOptions {
    TorrentFormat format = TorrentFormat::Hybrid;
    // 0 selects the automatic policy. Otherwise a power of two, 16 KiB..128 MiB.
    int piece_length = 0;
    // Each inner vector is one tier, in order.
    std::vector<std::vector<std::string>> tracker_tiers;
    std::vector<std::string> web_seeds;
    std::vector<std::pair<std::string, int>> dht_nodes;
    std::string comment;
    std::string creator;
    // Unix seconds; std::nullopt omits `creation date` (reproducible output).
    std::optional<std::int64_t> creation_date;
    bool private_flag = false;

    // Resource control (section 9.1). The payload-buffer budget bounds the
    // bytes held in read buffers at once; it is not a cap on total memory.
    std::size_t buffer_budget = 128u * 1024 * 1024;
    // Largest single read request.
    std::size_t read_buffer_size = 4u * 1024 * 1024;
    // Hash worker threads; 0 selects min(4, available processors).
    int hash_threads = 0;
    // Explicit consent to download cloud files that are not available locally.
    bool allow_hydration = false;
    // Explicit acceptance of an estimate above the planned-memory warning level.
    bool accept_large_resource_use = false;
    // Optional cooperative pause (not owned; must outlive the call).
    PauseControl* pause = nullptr;
};

// Planned metainfo/engine memory above which explicit acceptance is required.
inline constexpr std::uint64_t planned_memory_warning_bytes = 512ull * 1024 * 1024;

struct ResourceEstimate {
    // v1 piece hashes plus v2 roots and piece layers.
    std::uint64_t hash_bytes = 0;
    // Rough size of the serialized torrent.
    std::uint64_t metainfo_bytes = 0;
    // Rough peak memory: payload buffers, manifest and metainfo copies.
    std::uint64_t memory_bytes = 0;
};

struct PreflightReport {
    PieceSizeDecision piece;
    ResourceEstimate estimate;
    // Non-blocking findings to show before hashing.
    std::vector<ManifestIssue> warnings;
    std::size_t files_requiring_hydration = 0;
};

// Checks everything that can be known before reading payload data: manifest
// validity, piece-size policy, hydration consent and resource estimates.
// Throws CoreError for blocking problems (phase Preflight).
PreflightReport preflight(Manifest const& manifest, CreateOptions const& options);

struct CreateProgress {
    std::uint64_t payload_bytes_read = 0;
    std::uint64_t payload_bytes_total = 0;
    // Synthetic padding fed to the v1 stream; never counted as disk reads.
    std::uint64_t padding_bytes_processed = 0;
    std::uint64_t padding_bytes_total = 0;
    std::size_t files_completed = 0;
    std::size_t files_total = 0;
    // Torrent path of the file being read.
    std::string current_file;
};

using ProgressCallback = std::function<void(CreateProgress const&)>;

struct CreateResult {
    std::string torrent_bytes;
    InfoHashes info_hashes;
    MetainfoFormat format = MetainfoFormat::V1;
    int piece_length = 0;
    int num_pieces = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t padding_bytes = 0;
    std::uint64_t manifest_revision = 0;
    PreflightReport preflight;
};

// Hashes the manifest and returns validated metainfo bytes. Runs preflight()
// first, so blocking problems are reported before any payload is read. Throws
// CoreError: Cancelled when `stop` is requested; SourceChanged when a source
// was replaced, resized or modified since the manifest was frozen (checked
// when each file is opened, after it is read, and for the whole manifest
// before returning); SourceMissing/SourceUnreadable on I/O failures.
CreateResult create_torrent(Manifest const& manifest, CreateOptions const& options, PayloadSource& source,
    std::stop_token stop = {}, ProgressCallback const& progress = {});

// Version string of the pinned libtorrent build, e.g. "2.1.2.0".
std::string engine_version();

} // namespace tc::core
