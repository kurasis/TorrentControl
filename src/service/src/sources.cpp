#include "tc/service/sources.hpp"

#include "tc/core/error.hpp"

#include <algorithm>

namespace tc::service {

core::ScanOptions scan_options(SourceSpec const& s, std::vector<std::filesystem::path> const& excluded_paths)
{
    core::ScanOptions o;
    o.recursive = s.recursive;
    o.follow_links = s.follow_links;
    o.cloud_policy = s.skip_cloud ? core::CloudPolicy::SkipUnavailable : core::CloudPolicy::Include;
    for (auto const& pattern : s.exclusions) o.exclusions.push_back({pattern, "excluded by rule " + pattern});
    o.excluded_paths = excluded_paths;
    return o;
}

core::Manifest build_manifest(std::vector<SourceSpec> const& sources, std::string const& name,
    std::vector<std::filesystem::path> const& excluded_paths, std::stop_token stop)
{
    if (sources.empty()) throw core::CoreError(core::ErrorCode::EmptyPayload, "No sources are selected");

    if (sources.size() == 1) {
        core::Manifest m = core::scan_source(sources.front().path, scan_options(sources.front(), excluded_paths), stop);
        if (!name.empty()) m.name = name;
        for (auto& e : m.entries) e.source_id = sources.front().id + "/" + e.source_id;
        return m;
    }

    core::Manifest out;
    out.name = name.empty() ? "Collection" : name;
    out.mode = core::LayoutMode::Directory;
    for (auto const& spec : sources) {
        core::Manifest m = core::scan_source(spec.path, scan_options(spec, excluded_paths), stop);
        for (auto& e : m.entries) {
            std::vector<std::string> path{m.name};
            path.insert(path.end(), e.torrent_path.begin(), e.torrent_path.end());
            e.torrent_path = std::move(path);
            e.source_id = spec.id + "/" + e.source_id;
            out.entries.push_back(std::move(e));
        }
        out.skipped.insert(out.skipped.end(), m.skipped.begin(), m.skipped.end());
        out.unreadable.insert(out.unreadable.end(), m.unreadable.begin(), m.unreadable.end());
    }
    std::stable_sort(out.entries.begin(), out.entries.end(),
        [](core::ManifestEntry const& a, core::ManifestEntry const& b) { return a.torrent_path < b.torrent_path; });
    return out;
}

std::string safe_file_name(std::string_view name)
{
    std::string out;
    for (char c : name) {
        auto const u = static_cast<unsigned char>(c);
        if (u < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            out.push_back('_');
        else
            out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "torrent";
    return out;
}

std::filesystem::path default_output(std::vector<SourceSpec> const& sources, std::string const& name)
{
    if (sources.empty()) return {};
    std::filesystem::path const dir = std::filesystem::absolute(sources.front().path).lexically_normal().parent_path();
    return dir / core::path_from_utf8(safe_file_name(name) + ".torrent");
}

} // namespace tc::service
