#include "tc/service/profiles.hpp"

#include "tc/core/error.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace tc::service {

namespace {

bool is_alnum(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

// A path segment that looks like a passkey or token: long and mostly
// alphanumeric with digits.
bool passkey_like(std::string_view seg)
{
    if (seg.size() < 16) return false;
    std::size_t alnum = 0;
    std::size_t digits = 0;
    for (char c : seg) {
        if (is_alnum(c)) ++alnum;
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) ++digits;
    }
    return alnum * 10 >= seg.size() * 9 && digits > 0;
}

json rows_json(std::vector<TrackerRow> const& rows)
{
    json a = json::array();
    for (auto const& r : rows) a.push_back({{"url", r.url}, {"tier", r.tier}, {"enabled", r.enabled}});
    return a;
}

json nodes_json(std::vector<std::pair<std::string, int>> const& nodes)
{
    json a = json::array();
    for (auto const& [h, p] : nodes) a.push_back({{"host", h}, {"port", p}});
    return a;
}

} // namespace

TrackerCatalog const& builtin_tracker_catalog()
{
    static TrackerCatalog const catalog{
        "https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_best.txt",
        "2026-10-03",
        {
            "udp://tracker.opentrackr.org:1337/announce",
            "udp://open.stealth.si:80/announce",
            "udp://tracker.torrent.eu.org:451/announce",
            "udp://open.demonii.com:1337/announce",
            "udp://exodus.desync.com:6969/announce",
            "http://tracker.qu.ax:6969/announce",
            "udp://tracker.skynetcloud.site:6969/announce",
            "udp://tracker.tryhackx.org:6969/announce",
            "udp://tracker.gmi.gd:6969/announce",
            "udp://explodie.org:6969/announce",
        },
    };
    return catalog;
}

std::vector<Profile> builtin_profiles()
{
    std::vector<Profile> p;
    p.push_back({.id = "public", .name = "Public", .builtin = true, .format = core::TorrentFormat::Hybrid,
        .use_public_trackers = true});
    p.push_back({.id = "compatible", .name = "Maximum compatibility", .builtin = true,
        .format = core::TorrentFormat::V1, .use_public_trackers = true});
    p.push_back({.id = "private", .name = "Private tracker", .builtin = true, .format = core::TorrentFormat::V1,
        .private_flag = true, .allow_web_seeds = false});
    p.push_back({.id = "trackerless", .name = "Trackerless", .builtin = true, .format = core::TorrentFormat::Hybrid,
        .clear_trackers = true});
    return p;
}

ProfilePlan plan_profile(Draft const& current, Profile const& p)
{
    ProfilePlan plan;
    Draft next = current;
    next.profile_id = p.id;
    next.format = p.format;
    next.private_flag = p.private_flag;
    next.piece_length = 0; // every profile uses the automatic piece size

    std::vector<std::string> const& preset = builtin_tracker_catalog().urls;
    std::set<std::string, std::less<>> const preset_set(preset.begin(), preset.end());
    auto const is_preset = [&](TrackerRow const& r) { return preset_set.contains(r.url); };

    std::vector<TrackerRow> user_rows;
    for (auto const& r : current.trackers)
        if (!is_preset(r)) user_rows.push_back(r);

    if (p.clear_trackers) {
        next.trackers.clear();
    } else if (p.private_flag) {
        // Only the user's own (authorized) trackers, plus any the profile defines.
        next.trackers = user_rows;
        for (auto const& r : p.trackers)
            if (std::none_of(next.trackers.begin(), next.trackers.end(), [&](auto const& t) { return t.url == r.url; }))
                next.trackers.push_back(r);
    } else {
        std::vector<TrackerRow> rows;
        int tier = 0;
        // Custom rows first, in their own tiers, then the profile's rows and the preset.
        for (auto const& r : user_rows) {
            TrackerRow copy = r;
            copy.tier = tier++;
            rows.push_back(copy);
        }
        auto add = [&](std::string const& url) {
            if (std::any_of(rows.begin(), rows.end(), [&](auto const& t) { return t.url == url; })) return;
            rows.push_back({url, tier++, true});
        };
        for (auto const& r : p.trackers) add(r.url);
        if (p.use_public_trackers)
            for (auto const& url : preset) add(url);
        next.trackers = std::move(rows);
    }

    if (!p.allow_web_seeds) next.web_seeds.clear();
    if (p.private_flag || p.clear_trackers) {
        // Private torrents never carry DHT nodes; trackerless keeps user nodes.
        if (p.private_flag) next.dht_nodes.clear();
    }
    if (!p.source_tag.empty()) next.source_tag = p.source_tag;

    auto change = [&](std::string field, json before, json after, bool removes) {
        if (before != after) plan.changes.push_back({std::move(field), std::move(before), std::move(after), removes});
    };
    change("format", std::string(core::to_string(current.format)), std::string(core::to_string(next.format)), false);
    change("private", current.private_flag, next.private_flag, false);
    change("pieceLength", current.piece_length, next.piece_length, false);

    bool removes_user_tracker = false;
    for (auto const& r : user_rows)
        if (std::none_of(next.trackers.begin(), next.trackers.end(), [&](auto const& t) { return t.url == r.url; }))
            removes_user_tracker = true;
    change("trackers", rows_json(current.trackers), rows_json(next.trackers), removes_user_tracker);
    change("webSeeds", current.web_seeds, next.web_seeds, !current.web_seeds.empty() && next.web_seeds.empty());
    change("dhtNodes", nodes_json(current.dht_nodes), nodes_json(next.dht_nodes), !current.dht_nodes.empty());
    change("source", current.source_tag, next.source_tag, !current.source_tag.empty());
    change("profile", current.profile_id, next.profile_id, false);

    plan.result = std::move(next);
    return plan;
}

json to_json(Profile const& p)
{
    return json{{"id", p.id}, {"name", p.name}, {"builtin", p.builtin}, {"format", std::string(core::to_string(p.format))},
        {"private", p.private_flag}, {"publicTrackers", p.use_public_trackers}, {"trackers", rows_json(p.trackers)},
        {"allowWebSeeds", p.allow_web_seeds}, {"clearTrackers", p.clear_trackers}, {"source", p.source_tag}};
}

Profile profile_from_json(json const& j)
{
    if (!j.is_object()) throw core::CoreError(core::ErrorCode::InvalidArgument, "profile must be an object");
    Profile p;
    p.id = j.value("id", "");
    p.name = j.value("name", "");
    if (p.id.empty() || p.id.size() > 64 || p.name.empty() || p.name.size() > 200)
        throw core::CoreError(core::ErrorCode::InvalidArgument, "profile needs an id and a name");
    auto f = format_from(j.value("format", "hybrid"));
    if (!f) throw core::CoreError(core::ErrorCode::InvalidArgument, "profile format must be v1, v2 or hybrid");
    p.format = *f;
    p.private_flag = j.value("private", false);
    p.use_public_trackers = j.value("publicTrackers", false) && !p.private_flag;
    p.allow_web_seeds = j.value("allowWebSeeds", true);
    p.clear_trackers = j.value("clearTrackers", false);
    p.source_tag = j.value("source", "");
    if (auto it = j.find("trackers"); it != j.end() && it->is_array()) {
        for (auto const& r : *it) {
            if (!r.is_object() || !r.contains("url") || !r.at("url").is_string()) continue;
            p.trackers.push_back({r.at("url").get<std::string>(), r.value("tier", 0), r.value("enabled", true)});
        }
    }
    return p;
}

std::string redact_url(std::string_view url)
{
    std::string out;
    std::size_t const scheme_end = url.find("://");
    std::size_t pos = 0;
    if (scheme_end != std::string_view::npos) {
        out.append(url.substr(0, scheme_end + 3));
        pos = scheme_end + 3;
    }
    std::size_t const auth_end = url.find_first_of("/?#", pos);
    std::string_view authority = url.substr(pos, auth_end == std::string_view::npos ? std::string_view::npos : auth_end - pos);
    if (auto at = authority.rfind('@'); at != std::string_view::npos) {
        out.append("***@");
        authority = authority.substr(at + 1);
    }
    out.append(authority);
    if (auth_end == std::string_view::npos) return out;
    pos = auth_end;

    std::size_t const query = url.find_first_of("?#", pos);
    std::string_view path = url.substr(pos, query == std::string_view::npos ? std::string_view::npos : query - pos);
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t const slash = path.find('/', start);
        std::string_view seg = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        out.append(passkey_like(seg) ? "***" : std::string(seg));
        if (slash == std::string_view::npos) break;
        out.push_back('/');
        start = slash + 1;
    }
    if (query == std::string_view::npos) return out;

    std::string_view rest = url.substr(query);
    if (rest.front() == '#') return out;
    std::size_t const frag = rest.find('#');
    std::string_view q = rest.substr(1, frag == std::string_view::npos ? std::string_view::npos : frag - 1);
    out.push_back('?');
    std::size_t s = 0;
    bool first = true;
    while (s <= q.size()) {
        std::size_t const amp = q.find('&', s);
        std::string_view kv = q.substr(s, amp == std::string_view::npos ? std::string_view::npos : amp - s);
        if (!first) out.push_back('&');
        first = false;
        std::size_t const eq = kv.find('=');
        out.append(kv.substr(0, eq));
        if (eq != std::string_view::npos) out.append("=***");
        if (amp == std::string_view::npos) break;
        s = amp + 1;
    }
    return out;
}

bool looks_secret(std::string_view url)
{
    return redact_url(url) != url;
}

} // namespace tc::service
