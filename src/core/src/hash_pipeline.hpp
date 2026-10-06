#pragma once

// Internal single-pass hashing pipeline shared by torrent creation and local
// payload verification (specification sections 6.1 and 9.1).
//
// The calling thread reads payload data in canonical layout order into
// bounded buffers; a small worker pool computes SHA-1 piece hashes over the
// v1 logical stream (real data plus synthetic BEP 47 padding) and BEP 52
// per-piece Merkle roots over each real file. Every real byte is read once.

#include "tc/core/manifest.hpp"
#include "tc/core/hash_metrics.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/payload_source.hpp"

#include <cstdint>
#include <functional>
#include <stop_token>

#include "tc/core/pause.hpp"
#include <string>
#include <vector>

namespace tc::core::detail {

inline constexpr int block_size = 16 * 1024; // BEP 52 leaf block size

struct LayoutFile {
    std::string torrent_path;
    std::uint64_t length = 0;
    bool pad = false;
    // Source to read. Null for padding, and for files the caller knows are
    // unmapped (verification then treats them as missing).
    ManifestEntry const* source = nullptr;
};

enum class ReadPolicy {
    // Creation: any missing, unreadable or changed source aborts with CoreError.
    Strict,
    // Verification: problems are recorded per file and the missing bytes are
    // hashed as zeros so the remaining pieces can still be checked.
    Record,
};

struct HashJob {
    std::vector<LayoutFile> files;
    int piece_length = 0;
    bool v1 = true;
    bool v2 = true;
    ReadPolicy policy = ReadPolicy::Strict;
    // Upper bound for payload bytes held in buffers at once (one unit of work
    // must hold at least one complete piece).
    std::size_t buffer_budget = 128u * 1024 * 1024;
    // Largest single read request.
    std::size_t read_size = 4u * 1024 * 1024;
    // 0 selects min(4, hardware threads).
    int threads = 0;
    // Optional cooperative pause; checked between reads.
    PauseControl* pause = nullptr;
};

enum class FileStatus { Ok, Missing, Unreadable, SizeMismatch, Changed };

std::string_view to_string(FileStatus s) noexcept;

struct FileOutcome {
    FileStatus status = FileStatus::Ok;
    std::string message;
};

struct HashProgress {
    std::uint64_t payload_bytes_read = 0;
    std::uint64_t payload_bytes_total = 0;
    std::uint64_t padding_bytes_processed = 0;
    std::uint64_t padding_bytes_total = 0;
    std::size_t files_completed = 0;
    std::size_t files_total = 0;
    // Torrent path of the file being read.
    std::string current_file;
};

struct HashOutput {
    // One digest per v1 piece (empty unless job.v1).
    std::vector<Sha1Digest> v1_pieces;
    // Per layout file: one Merkle root per piece of the file (empty for
    // padding, empty files, and unless job.v2).
    std::vector<std::vector<Sha256Digest>> v2_piece_roots;
    // Per layout file (Record policy fills problems; Strict never returns any).
    std::vector<FileOutcome> outcomes;
    HashProgress progress;
    HashMetrics metrics;
};

struct BufferPlan {
    std::size_t unit_bytes = 0;
    std::size_t max_buffers = 0;
    int workers = 0;
};
// Validates the hard payload budget, shared by preflight and execution.
BufferPlan plan_buffers(int piece_length, std::size_t budget, int threads);

HashOutput hash_payload(HashJob const& job, PayloadSource& source, std::stop_token stop,
    std::function<void(HashProgress const&)> const& progress);

// BEP 52 file root from per-piece roots: a single piece is the root itself;
// otherwise the piece layer is completed with the root of an all-zero piece
// subtree up to the next power of two.
Sha256Digest file_root(std::vector<Sha256Digest> const& piece_roots, int piece_length);

// Merkle root over `leaves` padded with zero hashes to `width` leaves.
Sha256Digest merkle_root(std::vector<Sha256Digest> leaves, std::size_t width);

std::size_t next_power_of_two(std::size_t n);

} // namespace tc::core::detail
