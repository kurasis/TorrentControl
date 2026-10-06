#include "tc/core/torrent_engine.hpp"

#include "tc/core/error.hpp"

#include "hash_pipeline.hpp"

#include <libtorrent/create_torrent.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

namespace tc::core {

namespace {

using detail::block_size;
constexpr int min_auto_piece = 256 * 1024;
constexpr int max_auto_piece = 16 * 1024 * 1024;
constexpr std::uint64_t target_max_pieces = 32'768;
constexpr std::uint64_t target_max_hash_bytes = 8ull * 1024 * 1024;

lt::create_flags_t engine_flags(TorrentFormat f)
{
    switch (f) {
    case TorrentFormat::V1: return lt::create_torrent::v1_only;
    case TorrentFormat::V2: return lt::create_torrent::v2_only;
    case TorrentFormat::Hybrid: return {};
    }
    return {};
}

std::vector<lt::create_file_entry> engine_entries(Manifest const& m)
{
    std::vector<lt::create_file_entry> files;
    files.reserve(m.entries.size());
    for (auto const& e : m.entries)
        files.emplace_back(m.torrent_path_string(e), static_cast<std::int64_t>(e.length));
    return files;
}

lt::create_torrent make_layout(Manifest const& m, TorrentFormat format, int piece_length)
{
    try {
        return lt::create_torrent(engine_entries(m), piece_length, engine_flags(format));
    } catch (lt::system_error const& e) {
        throw CoreError(ErrorCode::EngineError, std::string("The torrent layout was rejected by the engine: ") + e.what());
    }
}

bool valid_piece_length(int p) { return p >= block_size && p <= 128 * 1024 * 1024 && (p & (p - 1)) == 0; }

} // namespace

std::string_view to_string(TorrentFormat f) noexcept
{
    switch (f) {
    case TorrentFormat::V1: return "v1";
    case TorrentFormat::V2: return "v2";
    case TorrentFormat::Hybrid: return "hybrid";
    }
    return "unknown";
}

std::string engine_version() { return lt::version(); }

namespace {

// Hash tables beyond this size are refused before the engine allocates them
// (section 15, P02): the layout object itself holds one slot per piece.
constexpr std::uint64_t max_layout_hash_bytes = 2ull * 1024 * 1024 * 1024;

// Arithmetic upper bound of the hash-table size, computed without building
// the layout so absurd piece counts fail before any allocation.
std::uint64_t hash_bytes_upper_bound(Manifest const& m, TorrentFormat format, int piece_length)
{
    auto const p = static_cast<std::uint64_t>(piece_length);
    std::uint64_t aligned_pieces = 0;
    for (auto const& e : m.entries) aligned_pieces += (e.length + p - 1) / p;
    std::uint64_t bytes = 0;
    if (format == TorrentFormat::V1) bytes += 20 * ((m.total_length() + p - 1) / p);
    if (format == TorrentFormat::Hybrid) bytes += 20 * aligned_pieces;
    if (format != TorrentFormat::V1) bytes += 32 * aligned_pieces + 32 * m.entries.size();
    return bytes;
}

bool layout_too_large(Manifest const& m, TorrentFormat format, int piece_length)
{
    auto const p = static_cast<std::uint64_t>(piece_length);
    return hash_bytes_upper_bound(m, format, piece_length) > max_layout_hash_bytes
        || (m.total_length() + p - 1) / p + m.entries.size() > static_cast<std::uint64_t>(INT32_MAX);
}

} // namespace

PieceSizeDecision evaluate_piece_size(Manifest const& manifest, TorrentFormat format, int piece_length)
{
    if (!valid_piece_length(piece_length))
        throw CoreError(ErrorCode::InvalidArgument, "Piece length must be a power of two between 16 KiB and 128 MiB");
    if (layout_too_large(manifest, format, piece_length))
        throw CoreError(ErrorCode::ResourceLimit,
            "The piece size is too small for this payload: the piece hashes would not fit the engine's limits");

    lt::create_torrent const layout = make_layout(manifest, format, piece_length);
    std::uint64_t const payload = manifest.total_length();

    PieceSizeDecision d;
    d.piece_length = piece_length;
    d.logical_pieces = static_cast<std::uint64_t>(layout.num_pieces());
    d.padding_bytes = static_cast<std::uint64_t>(layout.total_size()) - payload;

    if (format != TorrentFormat::V2) d.estimated_hash_bytes += 20 * d.logical_pieces;
    if (format != TorrentFormat::V1) {
        for (auto const& f : layout.file_list()) {
            if (f.flags & lt::file_storage::flag_pad_file || f.size == 0) continue;
            auto const pieces = (static_cast<std::uint64_t>(f.size) + static_cast<std::uint64_t>(piece_length) - 1)
                / static_cast<std::uint64_t>(piece_length);
            d.estimated_hash_bytes += 32; // pieces root
            if (pieces > 1) d.estimated_hash_bytes += 32 * pieces; // piece layer
        }
    }
    d.exceeds_piece_count_target = d.logical_pieces > target_max_pieces;
    d.exceeds_hash_size_target = d.estimated_hash_bytes > target_max_hash_bytes;
    d.padding_warning = d.padding_bytes * 10 > payload;
    return d;
}

PieceSizeDecision choose_piece_size(Manifest const& manifest, TorrentFormat format)
{
    require_valid_manifest(manifest);
    PieceSizeDecision last;
    for (int candidate = min_auto_piece; candidate <= max_auto_piece; candidate *= 2) {
        if (candidate < max_auto_piece && layout_too_large(manifest, format, candidate)) continue;
        last = evaluate_piece_size(manifest, format, candidate);
        if (!last.exceeds_piece_count_target && !last.exceeds_hash_size_target) return last;
    }
    // No candidate meets the soft targets: the largest normal candidate is
    // used and the exceeded targets are reported to the caller.
    return last;
}

namespace {

std::uint64_t estimate_metainfo_bytes(Manifest const& m, TorrentFormat format, std::uint64_t hash_bytes)
{
    // Per file: path text plus dictionary overhead, once per representation.
    std::uint64_t per_files = 0;
    for (auto const& e : m.entries) {
        std::uint64_t path = 0;
        for (auto const& c : e.torrent_path) path += c.size() + 4;
        per_files += path + 48;
    }
    std::uint64_t const representations = format == TorrentFormat::Hybrid ? 2 : 1;
    return hash_bytes + per_files * representations + 1024;
}

CoreError at_phase(CoreError e, Phase phase)
{
    if (e.phase() == Phase::Unspecified) e.with_phase(phase);
    return e;
}

} // namespace

PreflightReport preflight(Manifest const& manifest, CreateOptions const& options)
{
    try {
        PreflightReport report;
        for (auto const& issue : validate_manifest(manifest)) {
            if (issue.severity == Severity::Warning) {
                report.warnings.push_back(issue);
                continue;
            }
            CoreError e(issue.code, issue.message);
            if (!issue.source_id.empty()) e.with_source(issue.source_id);
            throw e;
        }
        if (options.read_buffer_size < static_cast<std::size_t>(block_size))
            throw CoreError(ErrorCode::InvalidArgument, "Read buffer must be at least 16 KiB");
        if (options.hash_threads < 0 || options.hash_threads > 64)
            throw CoreError(ErrorCode::InvalidArgument, "Hash thread count must be between 0 and 64");

        for (auto const& e : manifest.entries)
            if (e.flags.requires_hydration) ++report.files_requiring_hydration;
        if (report.files_requiring_hydration > 0) {
            std::string const count = std::to_string(report.files_requiring_hydration);
            if (!options.allow_hydration)
                throw CoreError(ErrorCode::HydrationRequired,
                    count + " selected cloud file(s) are not available locally; allow downloading them or exclude them");
            report.warnings.push_back({ErrorCode::HydrationRequired,
                count + " cloud file(s) will be downloaded while hashing", Severity::Warning});
        }

        report.piece = options.piece_length == 0 ? choose_piece_size(manifest, options.format)
                                                 : evaluate_piece_size(manifest, options.format, options.piece_length);
        if (report.piece.logical_pieces > static_cast<std::uint64_t>(INT32_MAX))
            throw CoreError(ErrorCode::ResourceLimit, "The torrent would have more pieces than the engine supports; choose a larger piece size");
        if (options.piece_length == 0 && (report.piece.exceeds_piece_count_target || report.piece.exceeds_hash_size_target))
            report.warnings.push_back({ErrorCode::ResourceLimit,
                "No automatic piece size meets the piece-count and hash-size targets; the largest normal size is used",
                Severity::Warning});
        if (report.piece.padding_warning)
            report.warnings.push_back({ErrorCode::ResourceLimit,
                "Virtual padding is " + std::to_string(report.piece.padding_bytes)
                    + " bytes, more than 10% of the payload; consider a smaller piece size or v1",
                Severity::Warning});

        auto const buffers = detail::plan_buffers(report.piece.piece_length, options.buffer_budget, options.hash_threads);
        ResourceEstimate& est = report.estimate;
        est.payload_buffer_bytes = buffers.unit_bytes * buffers.max_buffers;
        est.hash_workers = buffers.workers;
        est.hash_bytes = report.piece.estimated_hash_bytes;
        est.metainfo_bytes = estimate_metainfo_bytes(manifest, options.format, est.hash_bytes);
        std::uint64_t manifest_bytes = 0;
        for (auto const& e : manifest.entries) {
            manifest_bytes += sizeof(ManifestEntry) + e.source_path.native().size() * sizeof(e.source_path.native()[0]);
            for (auto const& c : e.torrent_path) manifest_bytes += c.size() + sizeof(std::string);
        }
        // Hash results, libtorrent's copy of the layout and hashes, the
        // serialized buffer and the validation parses.
        est.memory_bytes = est.payload_buffer_bytes + manifest_bytes * 2 + est.hash_bytes * 2 + est.metainfo_bytes * 4;
        if (est.memory_bytes > planned_memory_warning_bytes) {
            std::string const mib = std::to_string(est.memory_bytes / (1024 * 1024));
            if (!options.accept_large_resource_use)
                throw CoreError(ErrorCode::ResourceLimit,
                    "This torrent needs an estimated " + mib + " MiB of memory; accept the estimate to continue");
            report.warnings.push_back({ErrorCode::ResourceLimit, "Estimated memory use is " + mib + " MiB", Severity::Warning});
        }
        return report;
    } catch (CoreError const& e) {
        throw at_phase(e, Phase::Preflight);
    }
}

CreateResult create_torrent(Manifest const& manifest, CreateOptions const& options, PayloadSource& source,
    std::stop_token stop, ProgressCallback const& progress)
{
    PreflightReport report = preflight(manifest, options);
    int const piece_length = report.piece.piece_length;

    lt::create_torrent ct = make_layout(manifest, options.format, piece_length);
    bool const want_v1 = options.format != TorrentFormat::V2;
    bool const want_v2 = options.format != TorrentFormat::V1;

    detail::HashProgress hash_progress;
    HashMetrics hash_metrics;
    { // Release the mapping and intermediate hash tables before serialization.
        // Map the engine's canonical file order back to manifest entries.
        std::unordered_map<std::string, ManifestEntry const*> by_torrent_path;
        for (auto const& e : manifest.entries) by_torrent_path.emplace(manifest.torrent_path_string(e), &e);

        detail::HashJob job;
        job.piece_length = piece_length;
        job.v1 = want_v1;
        job.v2 = want_v2;
        job.policy = detail::ReadPolicy::Strict;
        job.buffer_budget = options.buffer_budget;
        job.read_size = options.read_buffer_size;
        job.threads = options.hash_threads;
        job.pause = options.pause;
        for (auto const fi : ct.file_range()) {
            auto const& fe = ct.file_at(fi);
            detail::LayoutFile f;
            f.torrent_path = fe.filename;
            f.length = static_cast<std::uint64_t>(fe.size);
            f.pad = static_cast<bool>(fe.flags & lt::file_storage::flag_pad_file);
            if (!f.pad) {
                auto it = by_torrent_path.find(fe.filename);
                if (it == by_torrent_path.end())
                    throw CoreError(ErrorCode::EngineError, "Engine layout contains an unmapped file: " + fe.filename);
                f.source = it->second;
            }
            job.files.push_back(std::move(f));
        }

        std::function<void(detail::HashProgress const&)> forward;
        if (progress) {
            forward = [&](detail::HashProgress const& h) {
                CreateProgress p;
                p.payload_bytes_read = h.payload_bytes_read;
                p.payload_bytes_total = h.payload_bytes_total;
                p.padding_bytes_processed = h.padding_bytes_processed;
                p.padding_bytes_total = h.padding_bytes_total;
                p.files_completed = h.files_completed;
                p.files_total = h.files_total;
                p.current_file = h.current_file;
                progress(p);
            };
        }
        detail::HashOutput const hashed = detail::hash_payload(job, source, stop, forward);
        hash_progress = hashed.progress;
        hash_metrics = hashed.metrics;

        try {
            for (std::size_t i = 0; i < hashed.v1_pieces.size(); ++i)
                ct.set_hash(lt::piece_index_t{static_cast<int>(i)},
                    lt::sha1_hash(reinterpret_cast<char const*>(hashed.v1_pieces[i].data())));
            for (auto const fi : ct.file_range()) {
                auto const& roots = hashed.v2_piece_roots[static_cast<std::size_t>(static_cast<int>(fi))];
                for (std::size_t k = 0; k < roots.size(); ++k)
                    ct.set_hash2(fi, lt::piece_index_t::diff_type{static_cast<int>(k)},
                        lt::sha256_hash(reinterpret_cast<char const*>(roots[k].data())));
            }

            // Tier numbers follow the order of non-empty tiers.
            int tier = 0;
            for (auto const& tier_urls : options.tracker_tiers) {
                if (tier_urls.empty()) continue;
                for (auto const& url : tier_urls) ct.add_tracker(url, tier);
                ++tier;
            }
            for (auto const& seed : options.web_seeds) ct.add_url_seed(seed);
            for (auto const& node : options.dht_nodes) ct.add_node(node);
            if (!options.comment.empty()) ct.set_comment(options.comment.c_str());
            if (!options.creator.empty()) ct.set_creator(options.creator.c_str());
            ct.set_creation_date(static_cast<std::time_t>(options.creation_date.value_or(0)));
            ct.set_priv(options.private_flag);
        } catch (lt::system_error const& e) {
            throw CoreError(ErrorCode::EngineError, std::string("The engine rejected the torrent metadata: ") + e.what())
                .with_phase(Phase::Building);
        }

    }

    CreateResult result;
    result.hashing = hash_metrics;
    try {
        std::vector<char> const buf = ct.generate_buf();
        result.torrent_bytes.assign(buf.begin(), buf.end());
    } catch (lt::system_error const& e) {
        throw CoreError(ErrorCode::EngineError, std::string("The engine could not serialize the torrent: ") + e.what())
            .with_phase(Phase::Building);
    }

    try {
        // Validate output: our lossless parser and libtorrent must agree on
        // the identifiers, and libtorrent validates v2 piece layers.
        Metainfo const parsed = Metainfo::parse(result.torrent_bytes);
        lt::error_code ec;
        lt::add_torrent_params const loaded = lt::load_torrent_buffer(
            lt::span<char const>(result.torrent_bytes.data(), static_cast<std::ptrdiff_t>(result.torrent_bytes.size())), ec,
            lt::load_torrent_limits{});
        if (ec || !loaded.ti)
            throw CoreError(ErrorCode::InvalidMetainfo, "Generated metainfo failed engine validation: " + ec.message());
        auto const& ih = loaded.ti->info_hashes();
        if (parsed.info_hashes().v1.has_value() != ih.has_v1() || parsed.info_hashes().v2.has_value() != ih.has_v2()
            || (ih.has_v1() && std::memcmp(ih.v1.data(), parsed.info_hashes().v1->data(), 20) != 0)
            || (ih.has_v2() && std::memcmp(ih.v2.data(), parsed.info_hashes().v2->data(), 32) != 0))
            throw CoreError(ErrorCode::InvalidMetainfo, "Generated metainfo identifiers do not match the engine's");
        if (auto const problems = validate_metainfo(parsed); !problems.empty())
            throw CoreError(ErrorCode::InvalidMetainfo, "Generated metainfo is inconsistent: " + problems.front());

        // Best-effort consistency (section 9.2 step 4): the complete manifest
        // must still match what was frozen before the result is published.
        for (auto const& issue : recheck_sources(manifest)) {
            CoreError e(issue.code, issue.message);
            e.with_source(issue.source_id);
            throw e;
        }

        result.info_hashes = parsed.info_hashes();
        result.format = parsed.format();
    } catch (CoreError const& e) {
        throw at_phase(e, Phase::Validating);
    }

    result.piece_length = piece_length;
    result.num_pieces = ct.num_pieces();
    result.payload_bytes = hash_progress.payload_bytes_total;
    result.padding_bytes = hash_progress.padding_bytes_total;
    result.manifest_revision = manifest.revision;
    result.preflight = std::move(report);
    return result;
}

} // namespace tc::core
