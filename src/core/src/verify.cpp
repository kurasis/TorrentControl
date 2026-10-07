#include "tc/core/verify.hpp"

#include "tc/core/error.hpp"

#include "hash_pipeline.hpp"

#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace tc::core {

std::string_view to_string(VerifyStatus s) noexcept
{
    switch (s) {
    case VerifyStatus::Ok: return "ok";
    case VerifyStatus::Missing: return "missing";
    case VerifyStatus::Unreadable: return "unreadable";
    case VerifyStatus::SizeMismatch: return "size-mismatch";
    case VerifyStatus::Corrupt: return "corrupt";
    }
    return "unknown";
}

PayloadMapping map_to_root(Metainfo const& m, fs::path const& root)
{
    std::error_code ec;
    fs::path const absolute_root = fs::absolute(root, ec);
    if (ec) throw CoreError(ErrorCode::SourceUnreadable, "Cannot resolve the selected payload folder", ec.value());
    fs::path resolved_root = fs::weakly_canonical(absolute_root, ec);
    if (ec) throw CoreError(ErrorCode::SourceUnreadable, "Cannot resolve the selected payload folder", ec.value());
    if (resolved_root.filename().empty() && resolved_root.has_relative_path()) resolved_root = resolved_root.parent_path();
    PayloadMapping mapping;
    for (auto const& f : metainfo_files(m)) {
        if (f.pad) continue;
        fs::path p = resolved_root;
        for (auto const& c : f.path) p /= path_from_utf8(c);
        fs::path const resolved = fs::weakly_canonical(p, ec);
        if (ec) throw CoreError(ErrorCode::SourceUnreadable, "Cannot resolve a payload file", ec.value());
        auto base = resolved_root.begin();
        auto candidate = resolved.begin();
        for (; base != resolved_root.end() && candidate != resolved.end(); ++base, ++candidate) {
#ifdef _WIN32
            if (fold_case(to_utf8(*base)) != fold_case(to_utf8(*candidate))) break;
#else
            if (*base != *candidate) break;
#endif
        }
        if (base != resolved_root.end())
            throw CoreError(ErrorCode::InvalidArgument, "A payload path resolves outside the selected folder")
                .with_phase(Phase::Verifying);
        // Use the resolved path so replacing the original link cannot redirect
        // the later read. Hostile replacement of resolved parent directories
        // still needs handle-based protection; this is a mapping-time check.
        mapping.emplace(f.torrent_path, resolved);
    }
    return mapping;
}

VerifyResult verify_payload(Metainfo const& m, PayloadMapping const& mapping, PayloadSource& source, std::stop_token stop,
    ProgressCallback const& progress, VerifyOptions const& options)
{
    VerifyResult result;
    result.format = m.format();
    result.metainfo_problems = validate_metainfo(m);
    if (!result.metainfo_problems.empty()) return result;

    std::vector<MetainfoFile> const files = metainfo_files(m);
    bencode::Value const& info = m.info();
    int const piece_length = static_cast<int>(*info.find("piece length")->as_int64());
    if (piece_length < detail::block_size || (piece_length & (piece_length - 1)) != 0)
        throw CoreError(ErrorCode::UnsupportedFormat, "Payload verification needs a power-of-two piece length of at least 16 KiB")
            .with_phase(Phase::Verifying);

    // Stable storage for the sources the pipeline reads.
    std::vector<ManifestEntry> entries;
    entries.reserve(files.size());
    detail::HashJob job;
    job.piece_length = piece_length;
    job.v1 = m.format() != MetainfoFormat::V2;
    job.v2 = m.format() != MetainfoFormat::V1;
    job.policy = detail::ReadPolicy::Record;
    job.buffer_budget = options.buffer_budget;
    job.read_size = options.read_buffer_size;
    job.threads = options.hash_threads;
    job.pause = options.pause;
    for (std::size_t i = 0; i < files.size(); ++i) {
        detail::LayoutFile lf;
        lf.torrent_path = files[i].torrent_path;
        lf.length = files[i].length;
        lf.pad = files[i].pad;
        if (!lf.pad) {
            if (auto it = mapping.find(files[i].torrent_path); it != mapping.end()) {
                ManifestEntry e;
                e.source_id = "file-" + std::to_string(i);
                e.source_path = it->second;
                e.length = files[i].length;
                entries.push_back(std::move(e));
                lf.source = &entries.back();
            }
        }
        job.files.push_back(std::move(lf));
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
    detail::HashOutput hashed;
    try {
        hashed = detail::hash_payload(job, source, stop, forward);
    } catch (CoreError& e) {
        e.with_phase(Phase::Verifying);
        throw;
    }
    result.payload_bytes_read = hashed.progress.payload_bytes_read;

    // v1: compare each piece and attribute failures to overlapping files.
    std::vector<bool> bad_piece;
    if (job.v1) {
        std::string const& pieces = info.find("pieces")->text();
        bad_piece.resize(hashed.v1_pieces.size());
        result.v1_pieces_total = hashed.v1_pieces.size();
        for (std::size_t i = 0; i < hashed.v1_pieces.size(); ++i) {
            if (std::memcmp(pieces.data() + i * 20, hashed.v1_pieces[i].data(), 20) != 0) {
                bad_piece[i] = true;
                ++result.v1_pieces_bad;
            }
        }
    }

    auto const piece = static_cast<std::uint64_t>(piece_length);
    bencode::Value const* layers = m.root().find("piece layers");
    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        MetainfoFile const& f = files[i];
        std::uint64_t const start = offset;
        offset += job.v1 ? f.length : 0;
        if (f.pad) continue;

        VerifyFileResult r;
        r.torrent_path = f.torrent_path;
        r.length = f.length;

        if (job.v1 && f.length > 0) {
            for (std::uint64_t p = start / piece; p <= (start + f.length - 1) / piece; ++p)
                if (bad_piece[static_cast<std::size_t>(p)]) ++r.bad_v1_pieces;
        }
        if (job.v2 && f.pieces_root) {
            ++result.v2_files_checked;
            auto const& roots = hashed.v2_piece_roots[i];
            if (detail::file_root(roots, piece_length) != *f.pieces_root) {
                ++result.v2_files_bad;
                // Count failing pieces against the stored layer when there is one.
                bencode::Value const* layer = nullptr;
                if (layers != nullptr && roots.size() > 1)
                    layer = layers->find(std::string_view(reinterpret_cast<char const*>(f.pieces_root->data()), 32));
                if (layer != nullptr && layer->text().size() == roots.size() * 32) {
                    for (std::size_t k = 0; k < roots.size(); ++k)
                        if (std::memcmp(layer->text().data() + k * 32, roots[k].data(), 32) != 0) ++r.bad_v2_pieces;
                } else {
                    r.bad_v2_pieces = roots.size();
                }
            }
        }

        detail::FileOutcome const& outcome = hashed.outcomes[i];
        switch (outcome.status) {
        case detail::FileStatus::Ok:
        case detail::FileStatus::Changed:
            r.status = (r.bad_v1_pieces > 0 || r.bad_v2_pieces > 0) ? VerifyStatus::Corrupt : VerifyStatus::Ok;
            break;
        case detail::FileStatus::Missing: r.status = VerifyStatus::Missing; break;
        case detail::FileStatus::Unreadable: r.status = VerifyStatus::Unreadable; break;
        case detail::FileStatus::SizeMismatch: r.status = VerifyStatus::SizeMismatch; break;
        }
        r.message = outcome.message;
        if (r.status == VerifyStatus::Corrupt && r.message.empty()) r.message = "data does not match the torrent hashes";
        result.files.push_back(std::move(r));
    }

    result.ok = result.v1_pieces_bad == 0 && result.v2_files_bad == 0
        && std::all_of(result.files.begin(), result.files.end(), [](auto const& r) { return r.status == VerifyStatus::Ok; });
    return result;
}

} // namespace tc::core
