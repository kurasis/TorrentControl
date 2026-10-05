// tc-proof: internal developer tool driving the headless core. Not shipped.
//
//   tc-proof scan PATH [scan options]
//   tc-proof create (--source PATH [scan options] | --name NAME --map SRC DEST...)
//                   [--format v1|v2|hybrid] [--piece-length BYTES] [--tracker URL]...
//                   [--tier-break] [--web-seed URL]... [--comment TEXT] [--creator TEXT]
//                   [--date UNIX_SECONDS] [--private] [--buffer BYTES] [--budget BYTES]
//                   [--threads N] [--allow-hydration] [--accept-resources]
//                   [--cancel-after-bytes N] [--replace] -o OUT
//   tc-proof info FILE
//   tc-proof validate FILE
//   tc-proof verify FILE (--root PATH | --map TORRENT_PATH SRC...)
//   tc-proof edit-outer IN OUT [--set-comment TEXT] [--set-announce URL] [--remove KEY]...
//   tc-proof edit-info IN OUT [--set-source TEXT] [--set-private] [--remove KEY]...
//                   [--remove-signatures]
//
// Scan options: --non-recursive, --exclude PATTERN, --no-default-exclusions,
// --follow-links, --skip-cloud.
//
// Results are printed as JSON so integration tests can check them.

#include "tc/core/error.hpp"
#include "tc/core/manifest.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/output.hpp"
#include "tc/core/payload_source.hpp"
#include "tc/core/torrent_engine.hpp"
#include "tc/core/verify.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stop_token>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace tc::core;
using nlohmann::json;

namespace {

struct Args {
    std::vector<std::string> items;
    std::size_t pos = 0;

    bool done() const { return pos >= items.size(); }
    std::string const& peek() const { return items[pos]; }
    std::string next(std::string_view what)
    {
        if (done()) throw CoreError(ErrorCode::InvalidArgument, "Missing value for " + std::string(what));
        return items[pos++];
    }
};

std::string read_file(fs::path const& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) throw CoreError(ErrorCode::SourceMissing, "Cannot open " + to_utf8(p));
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_output(fs::path const& p, std::string const& bytes, bool replace = true)
{
    CommitOptions o;
    o.replace_existing = replace;
    commit_output(p, bytes, o);
}

// Consumes a scan option at the front of `a`. Returns false if `flag` is not one.
bool scan_option(std::string const& flag, Args& a, ScanOptions& scan)
{
    if (flag == "--non-recursive") scan.recursive = false;
    else if (flag == "--exclude") scan.exclusions.push_back({a.next(flag), "excluded by rule " + a.items[a.pos - 1]});
    else if (flag == "--no-default-exclusions") scan.use_default_exclusions = false;
    else if (flag == "--follow-links") scan.follow_links = true;
    else if (flag == "--skip-cloud") scan.cloud_policy = CloudPolicy::SkipUnavailable;
    else return false;
    return true;
}

json issue_json(ManifestIssue const& i)
{
    return {{"code", std::string(to_string(i.code))}, {"message", i.message},
        {"severity", i.severity == Severity::Error ? "error" : "warning"}, {"source_id", i.source_id}};
}

json manifest_json(Manifest const& m)
{
    json j;
    j["name"] = m.name;
    j["mode"] = m.mode == LayoutMode::SingleFile ? "single-file" : "directory";
    j["revision"] = m.revision;
    j["entries"] = json::array();
    for (auto const& e : m.entries) {
        j["entries"].push_back({{"source_id", e.source_id}, {"torrent_path", m.torrent_path_string(e)},
            {"source", to_utf8(e.source_path)}, {"length", e.length}, {"reason", e.inclusion_reason},
            {"hidden", e.flags.hidden}, {"system", e.flags.system}, {"cloud", e.flags.cloud_placeholder},
            {"requires_hydration", e.flags.requires_hydration}, {"links", e.observed.link_count}});
    }
    j["skipped"] = json::array();
    for (auto const& s : m.skipped)
        j["skipped"].push_back({{"path", to_utf8(s.path)}, {"kind", std::string(to_string(s.kind))}, {"reason", s.reason}});
    j["unreadable"] = json::array();
    for (auto const& u : m.unreadable) j["unreadable"].push_back({{"path", to_utf8(u.path)}, {"message", u.message}});
    j["issues"] = json::array();
    for (auto const& i : validate_manifest(m)) j["issues"].push_back(issue_json(i));
    return j;
}

json describe(Metainfo const& m)
{
    json j;
    j["format"] = std::string(to_string(m.format()));
    j["name"] = m.name();
    if (m.info_hashes().v1) j["infohash_v1"] = to_hex(*m.info_hashes().v1);
    if (m.info_hashes().v2) j["infohash_v2"] = to_hex(*m.info_hashes().v2);
    j["magnet"] = make_magnet(m);
    return j;
}

TorrentFormat parse_format(std::string const& s)
{
    if (s == "v1") return TorrentFormat::V1;
    if (s == "v2") return TorrentFormat::V2;
    if (s == "hybrid") return TorrentFormat::Hybrid;
    throw CoreError(ErrorCode::InvalidArgument, "Unknown format: " + s);
}

int cmd_create(Args& a)
{
    CreateOptions opt;
    std::optional<fs::path> source;
    ScanOptions scan;
    std::string name;
    std::vector<std::pair<fs::path, std::string>> maps;
    fs::path out;
    std::uint64_t cancel_after = 0;
    bool replace = false;
    opt.tracker_tiers.emplace_back();

    while (!a.done()) {
        std::string const flag = a.next("option");
        if (flag == "--source") source = path_from_utf8(a.next(flag));
        else if (scan_option(flag, a, scan)) continue;
        else if (flag == "--name") name = a.next(flag);
        else if (flag == "--map") {
            fs::path src = path_from_utf8(a.next(flag));
            maps.emplace_back(std::move(src), a.next(flag));
        }
        else if (flag == "--format") opt.format = parse_format(a.next(flag));
        else if (flag == "--piece-length") opt.piece_length = std::stoi(a.next(flag));
        else if (flag == "--tracker") opt.tracker_tiers.back().push_back(a.next(flag));
        else if (flag == "--tier-break") opt.tracker_tiers.emplace_back();
        else if (flag == "--web-seed") opt.web_seeds.push_back(a.next(flag));
        else if (flag == "--comment") opt.comment = a.next(flag);
        else if (flag == "--creator") opt.creator = a.next(flag);
        else if (flag == "--date") opt.creation_date = std::stoll(a.next(flag));
        else if (flag == "--private") opt.private_flag = true;
        else if (flag == "--buffer") opt.read_buffer_size = std::stoul(a.next(flag));
        else if (flag == "--budget") opt.buffer_budget = std::stoul(a.next(flag));
        else if (flag == "--threads") opt.hash_threads = std::stoi(a.next(flag));
        else if (flag == "--allow-hydration") opt.allow_hydration = true;
        else if (flag == "--accept-resources") opt.accept_large_resource_use = true;
        else if (flag == "--replace") replace = true;
        else if (flag == "--cancel-after-bytes") cancel_after = std::stoull(a.next(flag));
        else if (flag == "-o") out = path_from_utf8(a.next(flag));
        else throw CoreError(ErrorCode::InvalidArgument, "Unknown option: " + flag);
    }
    if (out.empty()) throw CoreError(ErrorCode::InvalidArgument, "Missing -o OUT");

    Manifest manifest;
    if (source) {
        // A new output file is never part of the payload (its temporary files
        // are covered by the default exclusions). An existing file at the
        // output path stays in the scan: it may be payload, so a collision is
        // reported (W06) instead of silently dropping and overwriting it.
        std::error_code ec;
        if (!fs::exists(out, ec)) scan.excluded_paths.push_back(out);
        manifest = scan_source(*source, scan);
    } else {
        if (name.empty() || maps.empty())
            throw CoreError(ErrorCode::InvalidArgument, "Use --source PATH, or --name NAME with one or more --map SRC DEST");
        ManifestBuilder b(name);
        for (auto& [src, dest] : maps) {
            std::vector<std::string> components;
            std::size_t start = 0;
            while (true) {
                std::size_t const slash = dest.find('/', start);
                components.push_back(dest.substr(start, slash - start));
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            b.add_file(src, std::move(components));
        }
        manifest = b.build();
    }

    std::stop_source stop;
    auto payload = make_file_payload_source();
    std::uint64_t progress_events = 0;
    CreateProgress last;
    auto on_progress = [&](CreateProgress const& p) {
        ++progress_events;
        last = p;
        if (cancel_after != 0 && p.payload_bytes_read >= cancel_after) stop.request_stop();
    };

    json j;
    j["manifest_files"] = manifest.entries.size();
    j["skipped"] = manifest_json(manifest)["skipped"];
    try {
        // Section 14.1 step 1: reject a source collision before reading payload.
        check_output_target(out, manifest);
        CreateResult const r = create_torrent(manifest, opt, *payload, stop.get_token(), on_progress);
        CommitOptions commit;
        commit.replace_existing = replace;
        commit.manifest = &manifest;
        CommitResult const committed = commit_output(out, r.torrent_bytes, commit);
        Metainfo const m = Metainfo::parse(read_file(out)); // reopen the committed file
        j["status"] = "succeeded";
        j["result"] = describe(m);
        j["piece_length"] = r.piece_length;
        j["num_pieces"] = r.num_pieces;
        j["payload_bytes"] = r.payload_bytes;
        j["padding_bytes"] = r.padding_bytes;
        j["replaced_existing"] = committed.replaced_existing;
        j["warnings"] = json::array();
        for (auto const& w : r.preflight.warnings) j["warnings"].push_back(issue_json(w));
        j["estimated_memory_bytes"] = r.preflight.estimate.memory_bytes;
    } catch (CoreError const& e) {
        if (e.code() != ErrorCode::Cancelled) throw;
        j["status"] = "cancelled";
    }
    j["engine"] = engine_version();
    j["progress_events"] = progress_events;
    j["payload_bytes_read"] = last.payload_bytes_read;
    j["padding_bytes_processed"] = last.padding_bytes_processed;
    std::cout << j.dump(2) << "\n";
    return j["status"] == "succeeded" ? 0 : 3;
}

int cmd_info(Args& a)
{
    Metainfo const m = Metainfo::parse(read_file(path_from_utf8(a.next("FILE"))));
    std::cout << describe(m).dump(2) << "\n";
    return 0;
}

int cmd_edit_outer(Args& a)
{
    fs::path const in = path_from_utf8(a.next("IN"));
    fs::path const out = path_from_utf8(a.next("OUT"));
    OuterEdit edit;
    while (!a.done()) {
        std::string const flag = a.next("option");
        if (flag == "--set-comment") edit["comment"] = bencode::Value::string(a.next(flag));
        else if (flag == "--set-announce") edit["announce"] = bencode::Value::string(a.next(flag));
        else if (flag == "--remove") edit[a.next(flag)] = std::nullopt;
        else throw CoreError(ErrorCode::InvalidArgument, "Unknown option: " + flag);
    }
    Metainfo const original = Metainfo::parse(read_file(in));
    write_output(out, apply_outer_edit(original, edit));
    Metainfo const edited = Metainfo::parse(read_file(out));

    json j;
    j["raw_info_identical"] = edited.raw_info() == original.raw_info();
    j["before"] = describe(original);
    j["after"] = describe(edited);
    std::cout << j.dump(2) << "\n";
    return j["raw_info_identical"] ? 0 : 4;
}

int cmd_scan(Args& a)
{
    fs::path const source = path_from_utf8(a.next("PATH"));
    ScanOptions scan;
    while (!a.done()) {
        std::string const flag = a.next("option");
        if (!scan_option(flag, a, scan)) throw CoreError(ErrorCode::InvalidArgument, "Unknown option: " + flag);
    }
    std::cout << manifest_json(scan_source(source, scan)).dump(2) << "\n";
    return 0;
}

int cmd_validate(Args& a)
{
    Metainfo const m = Metainfo::parse(read_file(path_from_utf8(a.next("FILE"))));
    json j = describe(m);
    j["problems"] = validate_metainfo(m);
    j["legacy_root_hash"] = m.has_legacy_root_hash();
    j["valid"] = j["problems"].empty();
    std::cout << j.dump(2) << "\n";
    return j["valid"] ? 0 : 5;
}

int cmd_verify(Args& a)
{
    Metainfo const m = Metainfo::parse(read_file(path_from_utf8(a.next("FILE"))));
    PayloadMapping mapping;
    while (!a.done()) {
        std::string const flag = a.next("option");
        if (flag == "--root") {
            mapping = map_to_root(m, path_from_utf8(a.next(flag)));
        } else if (flag == "--map") {
            std::string torrent_path = a.next(flag);
            mapping[torrent_path] = path_from_utf8(a.next(flag));
        } else {
            throw CoreError(ErrorCode::InvalidArgument, "Unknown option: " + flag);
        }
    }
    auto payload = make_file_payload_source();
    VerifyResult const r = verify_payload(m, mapping, *payload);
    json j = describe(m);
    j["ok"] = r.ok;
    j["metainfo_problems"] = r.metainfo_problems;
    j["v1_pieces_total"] = r.v1_pieces_total;
    j["v1_pieces_bad"] = r.v1_pieces_bad;
    j["v2_files_checked"] = r.v2_files_checked;
    j["v2_files_bad"] = r.v2_files_bad;
    j["payload_bytes_read"] = r.payload_bytes_read;
    j["files"] = json::array();
    for (auto const& f : r.files)
        j["files"].push_back({{"path", f.torrent_path}, {"status", std::string(to_string(f.status))}, {"message", f.message},
            {"bad_v1_pieces", f.bad_v1_pieces}, {"bad_v2_pieces", f.bad_v2_pieces}});
    std::cout << j.dump(2) << "\n";
    return r.ok ? 0 : 5;
}

int cmd_edit_info(Args& a)
{
    fs::path const in = path_from_utf8(a.next("IN"));
    fs::path const out = path_from_utf8(a.next("OUT"));
    InfoEdit edit;
    InfoEditOptions options;
    while (!a.done()) {
        std::string const flag = a.next("option");
        if (flag == "--set-source") edit["source"] = bencode::Value::string(a.next(flag));
        else if (flag == "--set-private") edit["private"] = bencode::Value::integer(1);
        else if (flag == "--remove") edit[a.next(flag)] = std::nullopt;
        else if (flag == "--remove-signatures") options.remove_invalidated_signatures = true;
        else throw CoreError(ErrorCode::InvalidArgument, "Unknown option: " + flag);
    }
    Metainfo const original = Metainfo::parse(read_file(in));
    InfoEditResult const r = apply_info_edit(original, edit, options);
    // A changed identity is saved as a new file by default (section 8).
    write_output(out, r.bytes, false);
    Metainfo const edited = Metainfo::parse(read_file(out));

    json j;
    j["before"] = describe(original);
    j["after"] = describe(edited);
    j["removed_signatures"] = r.removed_signatures;
    j["pieces_identical"] = original.info().find("pieces") == nullptr
        || original.info().find("pieces")->text() == edited.info().find("pieces")->text();
    j["problems"] = validate_metainfo(edited);
    std::cout << j.dump(2) << "\n";
    return 0;
}

int run(std::vector<std::string> argv)
{
    Args a{std::move(argv), 1};
    if (a.done())
        throw CoreError(ErrorCode::InvalidArgument, "Usage: tc-proof scan|create|info|validate|verify|edit-outer|edit-info ...");
    std::string const cmd = a.next("command");
    if (cmd == "scan") return cmd_scan(a);
    if (cmd == "create") return cmd_create(a);
    if (cmd == "info") return cmd_info(a);
    if (cmd == "validate") return cmd_validate(a);
    if (cmd == "verify") return cmd_verify(a);
    if (cmd == "edit-outer") return cmd_edit_outer(a);
    if (cmd == "edit-info") return cmd_edit_info(a);
    throw CoreError(ErrorCode::InvalidArgument, "Unknown command: " + cmd);
}

int guarded(std::vector<std::string> argv)
{
    try {
        return run(std::move(argv));
    } catch (CoreError const& e) {
        json j{{"status", "failed"}, {"code", std::string(to_string(e.code()))}, {"message", e.what()},
            {"phase", std::string(to_string(e.phase()))}, {"source_id", e.source_id()}, {"retryable", e.retryable()}};
        if (e.os_error()) j["os_error"] = *e.os_error();
        std::cout << j.dump(2) << "\n";
        return 2;
    } catch (std::exception const& e) {
        json j{{"status", "failed"}, {"code", "INTERNAL"}, {"message", e.what()}};
        std::cout << j.dump(2) << "\n";
        return 2;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    // Arguments arrive as UTF-16 and are converted to UTF-8 once, here.
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(to_utf8(fs::path(argv[i])));
    return guarded(std::move(args));
}
#else
int main(int argc, char** argv)
{
    return guarded(std::vector<std::string>(argv, argv + argc));
}
#endif
