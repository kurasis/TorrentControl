#include "tc/service/draft.hpp"

#include "tc/core/error.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace tc::service {

namespace {

using core::CoreError;
using core::ErrorCode;

[[noreturn]] void bad(std::string const& what)
{
    throw CoreError(ErrorCode::InvalidArgument, what);
}

std::string get_string(json const& v, char const* key, std::size_t max = 64 * 1024)
{
    if (!v.is_string()) bad(std::string(key) + " must be a string");
    std::string s = v.get<std::string>();
    if (s.size() > max) bad(std::string(key) + " is too long");
    return s;
}

bool get_bool(json const& v, char const* key)
{
    if (!v.is_boolean()) bad(std::string(key) + " must be true or false");
    return v.get<bool>();
}

std::int64_t get_int(json const& v, char const* key, std::int64_t lo, std::int64_t hi)
{
    std::int64_t n = 0;
    if (v.is_number_integer()) {
        n = v.get<std::int64_t>();
    } else if (v.is_string()) {
        // Large values cross the bridge as decimal strings (section 13.1).
        std::string const s = v.get<std::string>();
        try {
            std::size_t used = 0;
            n = std::stoll(s, &used);
            if (used != s.size()) bad(std::string(key) + " is not a decimal integer");
        } catch (std::logic_error const&) {
            bad(std::string(key) + " is not a decimal integer");
        }
    } else {
        bad(std::string(key) + " must be an integer");
    }
    if (n < lo || n > hi) bad(std::string(key) + " is out of range");
    return n;
}

std::vector<TrackerRow> trackers_from(json const& v)
{
    if (!v.is_array() || v.size() > 1000) bad("trackers must be an array of at most 1000 rows");
    std::vector<TrackerRow> rows;
    for (auto const& r : v) {
        if (!r.is_object()) bad("tracker rows must be objects");
        TrackerRow row;
        row.url = get_string(r.at("url"), "tracker url", 4096);
        if (auto it = r.find("tier"); it != r.end()) row.tier = static_cast<int>(get_int(*it, "tier", 0, 999));
        if (auto it = r.find("enabled"); it != r.end()) row.enabled = get_bool(*it, "enabled");
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::string> strings_from(json const& v, char const* key, std::size_t max_items)
{
    if (!v.is_array() || v.size() > max_items) bad(std::string(key) + " must be an array");
    std::vector<std::string> out;
    for (auto const& s : v) out.push_back(get_string(s, key, 4096));
    return out;
}

std::vector<std::pair<std::string, int>> nodes_from(json const& v)
{
    if (!v.is_array() || v.size() > 100) bad("dhtNodes must be an array");
    std::vector<std::pair<std::string, int>> out;
    for (auto const& n : v) {
        if (!n.is_object()) bad("DHT nodes must be objects");
        out.emplace_back(get_string(n.at("host"), "host", 255), static_cast<int>(get_int(n.at("port"), "port", 1, 65535)));
    }
    return out;
}

} // namespace

std::string_view to_string(CreationDatePolicy p) noexcept
{
    switch (p) {
    case CreationDatePolicy::Now: return "now";
    case CreationDatePolicy::Omit: return "omit";
    case CreationDatePolicy::Fixed: return "fixed";
    }
    return "now";
}

std::optional<CreationDatePolicy> creation_date_policy_from(std::string_view s) noexcept
{
    if (s == "now") return CreationDatePolicy::Now;
    if (s == "omit") return CreationDatePolicy::Omit;
    if (s == "fixed") return CreationDatePolicy::Fixed;
    return std::nullopt;
}

std::optional<core::TorrentFormat> format_from(std::string_view s) noexcept
{
    if (s == "v1") return core::TorrentFormat::V1;
    if (s == "v2") return core::TorrentFormat::V2;
    if (s == "hybrid") return core::TorrentFormat::Hybrid;
    return std::nullopt;
}

std::vector<std::vector<std::string>> tracker_tiers(std::vector<TrackerRow> const& rows)
{
    std::map<int, std::vector<std::string>> by_tier;
    for (auto const& r : rows)
        if (r.enabled && !r.url.empty()) by_tier[r.tier].push_back(r.url);
    std::vector<std::vector<std::string>> tiers;
    for (auto& [tier, urls] : by_tier) tiers.push_back(std::move(urls));
    return tiers;
}

core::CreateOptions create_options(Draft const& d, std::int64_t now)
{
    core::CreateOptions o;
    o.format = d.format;
    o.piece_length = d.piece_length;
    o.tracker_tiers = tracker_tiers(d.trackers);
    o.web_seeds = d.web_seeds;
    o.dht_nodes = d.private_flag ? std::vector<std::pair<std::string, int>>{} : d.dht_nodes;
    o.comment = d.comment;
    o.creator = d.creator;
    o.private_flag = d.private_flag;
    switch (d.date_policy) {
    case CreationDatePolicy::Now: o.creation_date = now; break;
    case CreationDatePolicy::Omit: o.creation_date = std::nullopt; break;
    case CreationDatePolicy::Fixed: o.creation_date = d.fixed_date; break;
    }
    o.allow_hydration = d.allow_hydration;
    o.accept_large_resource_use = d.accept_large_resource_use;
    return o;
}

std::string effective_name(Draft const& d)
{
    if (!d.name.empty()) return d.name;
    if (d.sources.size() == 1) return core::to_utf8(d.sources.front().path.filename());
    return {};
}

json to_json(SourceSpec const& s)
{
    return json{{"id", s.id}, {"path", core::to_utf8(s.path)}, {"name", core::to_utf8(s.path.filename())},
        {"isDirectory", s.is_directory}, {"recursive", s.recursive}, {"exclusions", s.exclusions},
        {"followLinks", s.follow_links}, {"skipCloud", s.skip_cloud}};
}

json to_json(Draft const& d, bool collections)
{
    json sources = json::array();
    if (collections) for (auto const& s : d.sources) sources.push_back(to_json(s));
    json trackers = json::array();
    if (collections) for (auto const& t : d.trackers) trackers.push_back({{"url", t.url}, {"tier", t.tier}, {"enabled", t.enabled}});
    json nodes = json::array();
    for (auto const& [host, port] : d.dht_nodes) nodes.push_back({{"host", host}, {"port", port}});
    return json{{"revision", std::to_string(d.revision)}, {"sources", std::move(sources)}, {"name", d.name},
        {"effectiveName", effective_name(d)}, {"profile", d.profile_id},
        {"format", std::string(core::to_string(d.format))}, {"pieceLength", d.piece_length},
        {"private", d.private_flag}, {"trackers", std::move(trackers)}, {"webSeeds", collections ? json(d.web_seeds) : json::array()},
        {"dhtNodes", std::move(nodes)}, {"comment", d.comment}, {"creator", d.creator}, {"source", d.source_tag},
        {"creationDate", std::string(to_string(d.date_policy))}, {"fixedDate", std::to_string(d.fixed_date)},
        {"output", core::to_utf8(d.output)}, {"replaceExisting", d.replace_existing},
        {"allowHydration", d.allow_hydration}, {"acceptLargeResourceUse", d.accept_large_resource_use}};
}

void apply_patch(Draft& d, json const& patch)
{
    if (!patch.is_object()) bad("patch must be an object");
    static std::set<std::string, std::less<>> const allowed = {"name", "format", "pieceLength", "private", "trackers",
        "webSeeds", "dhtNodes", "comment", "creator", "source", "creationDate", "fixedDate", "replaceExisting",
        "allowHydration", "acceptLargeResourceUse"};
    for (auto const& [key, value] : patch.items())
        if (!allowed.contains(key)) bad("unknown or read-only draft field: " + key);

    Draft next = d; // all-or-nothing
    for (auto const& [key, v] : patch.items()) {
        if (key == "name") next.name = get_string(v, "name", 4096);
        else if (key == "format") {
            auto f = format_from(get_string(v, "format"));
            if (!f) bad("format must be v1, v2 or hybrid");
            next.format = *f;
        } else if (key == "pieceLength") {
            auto const n = static_cast<int>(get_int(v, "pieceLength", 0, 128 * 1024 * 1024));
            if (n != 0 && (n < 16 * 1024 || (n & (n - 1)) != 0))
                bad("pieceLength must be 0 (automatic) or a power of two from 16 KiB to 128 MiB");
            next.piece_length = n;
        } else if (key == "private") next.private_flag = get_bool(v, "private");
        else if (key == "trackers") next.trackers = trackers_from(v);
        else if (key == "webSeeds") next.web_seeds = strings_from(v, "webSeeds", 1000);
        else if (key == "dhtNodes") next.dht_nodes = nodes_from(v);
        else if (key == "comment") next.comment = get_string(v, "comment");
        else if (key == "creator") next.creator = get_string(v, "creator", 4096);
        else if (key == "source") next.source_tag = get_string(v, "source", 4096);
        else if (key == "creationDate") {
            auto p = creation_date_policy_from(get_string(v, "creationDate"));
            if (!p) bad("creationDate must be now, omit or fixed");
            next.date_policy = *p;
        } else if (key == "fixedDate") next.fixed_date = get_int(v, "fixedDate", 0, 253402300799);
        else if (key == "replaceExisting") next.replace_existing = get_bool(v, "replaceExisting");
        else if (key == "allowHydration") next.allow_hydration = get_bool(v, "allowHydration");
        else if (key == "acceptLargeResourceUse") next.accept_large_resource_use = get_bool(v, "acceptLargeResourceUse");
    }
    d = std::move(next);
}

Draft draft_from_json(json const& j)
{
    if (!j.is_object()) bad("draft must be an object");
    Draft d;
    json patch = json::object();
    for (auto const& key : {"name", "format", "pieceLength", "private", "trackers", "webSeeds", "dhtNodes", "comment",
             "creator", "source", "creationDate", "fixedDate", "replaceExisting", "allowHydration",
             "acceptLargeResourceUse"})
        if (j.contains(key)) patch[key] = j.at(key);
    apply_patch(d, patch);
    if (auto it = j.find("profile"); it != j.end()) d.profile_id = get_string(*it, "profile", 200);
    if (auto it = j.find("output"); it != j.end()) d.output = core::path_from_utf8(get_string(*it, "output", 32768));
    if (auto it = j.find("sources"); it != j.end()) {
        if (!it->is_array() || it->size() > 100000) bad("sources must be an array");
        for (auto const& s : *it) {
            SourceSpec spec;
            spec.id = get_string(s.at("id"), "id", 64);
            spec.path = core::path_from_utf8(get_string(s.at("path"), "path", 32768));
            spec.is_directory = s.contains("isDirectory") && get_bool(s.at("isDirectory"), "isDirectory");
            if (s.contains("recursive")) spec.recursive = get_bool(s.at("recursive"), "recursive");
            if (s.contains("exclusions")) spec.exclusions = strings_from(s.at("exclusions"), "exclusions", 1000);
            if (s.contains("followLinks")) spec.follow_links = get_bool(s.at("followLinks"), "followLinks");
            if (s.contains("skipCloud")) spec.skip_cloud = get_bool(s.at("skipCloud"), "skipCloud");
            d.sources.push_back(std::move(spec));
        }
    }
    return d;
}

} // namespace tc::service
