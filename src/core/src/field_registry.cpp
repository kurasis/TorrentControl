#include "tc/core/field_registry.hpp"
#include "tc/core/error.hpp"

#include <unordered_map>

namespace tc::core {
namespace {
using bencode::Value;
constexpr FieldDescriptor fields[] = {
    {"top","info","dict","all","computed",false,true,"BEP 3/52","rebuild"},
    {"top","piece layers","dict","v2,hybrid","computed",false,false,"BEP 52","rebuild"},
    {"top","announce","str","all","form",true,false,"BEP 3","tracker-url"},
    {"top","announce-list","list","all","form",true,false,"BEP 12","tracker-tiers"},
    {"top","url-list","str|list","all","form",true,false,"BEP 19","web-seeds"},
    {"top","httpseeds","list","all","form",true,false,"BEP 17","http-seeds"},
    {"top","nodes","list","all","expert",true,false,"BEP 5","nodes"},
    {"top","comment","str","all","form",true,false,"BEP 3","text"},
    {"top","created by","str","all","form",true,false,"BEP 3","text"},
    {"top","creation date","int","all","form",true,false,"BEP 3","unix-seconds"},
    {"info","name","str","all","computed",false,true,"BEP 3/52","rebuild"},
    {"info","piece length","int","all","computed",false,true,"BEP 3/52","rebuild"},
    {"info","pieces","str","v1,hybrid","computed",false,true,"BEP 3","rebuild"},
    {"info","length","int","v1,hybrid","computed",false,true,"BEP 3","rebuild"},
    {"info","files","list","v1,hybrid","computed",false,true,"BEP 3","rebuild"},
    {"info","meta version","int","v2,hybrid","computed",false,true,"BEP 52","rebuild"},
    {"info","file tree","dict","v2,hybrid","computed",false,true,"BEP 52","rebuild"},
    {"info","private","int","all","form",true,true,"BEP 27","private-one"},
    {"info","source","str","all","form",true,true,"nonstandard","text"},
    {"info","attr","str","all","preserve",false,true,"BEP 47","rebuild"},
    {"info","symlink path","list","all","preserve",false,true,"BEP 47","rebuild"},
    {"info","pieces root","str","v2,hybrid","computed",false,true,"BEP 52","rebuild"},
    {"info","sha1","str","v1,hybrid","computed",false,true,"BEP 47","rebuild"},
    {"info","mtime","int","all","preserve",false,true,"BEP 47","preserve"},
    {"info","root hash","str","v1","preserve",false,true,"BEP 30 (legacy)","preserve"},
    {"info","collections","list","all","preserve",false,true,"BEP 38","preserve"},
    {"info","similar","list","all","preserve",false,true,"BEP 38","preserve"},
    {"top","collections","list","all","preserve",false,false,"BEP 38","preserve"},
    {"top","similar","list","all","preserve",false,false,"BEP 38","preserve"},
    {"top","signatures","dict","all","preserve",false,false,"BEP 35","explicit removal on info edit"},
    {"info","update-url","str","all","expert",true,true,"BEP 39","http-url"},
    {"info","originator","str","all","preserve",false,true,"BEP 39","preserve"},
    {"info","ssl-cert","str","all","preserve",false,true,"vendor","preserve"},
    {"top","encoding","str","all","expert",true,false,"vendor","text"},
    {"top","publisher","str","all","expert",true,false,"vendor","text"},
    {"top","publisher-url","str","all","expert",true,false,"vendor","http-url"},
    {"top","azureus_properties","dict","all","expert",true,false,"vendor","dict"},
    {"top","comment.utf-8","str","all","preserve",false,false,"vendor","preserve alias"},
    {"info","name.utf-8","str","all","preserve",false,true,"vendor","preserve alias"},
    {"info","md5sum","str","v1,hybrid","preserve",false,true,"BEP 3 (optional)","preserve"},
    {"file","length","int","all","computed",false,true,"BEP 3/52","rebuild"},
    {"file","path","list","v1,hybrid","computed",false,true,"BEP 3","rebuild"},
    {"file","path.utf-8","list","v1,hybrid","preserve",false,true,"vendor","preserve alias"},
    {"file","attr","str","all","preserve",false,true,"BEP 47","rebuild"},
    {"file","symlink path","list","all","preserve",false,true,"BEP 47","rebuild"},
    {"file","pieces root","str","v2,hybrid","computed",false,true,"BEP 52","rebuild"},
    {"file","sha1","str","all","computed",false,true,"BEP 47","rebuild"},
    {"file","mtime","int","all","preserve",false,true,"BEP 47","preserve"},
    {"file","md5sum","str","all","preserve",false,true,"BEP 3 (optional)","preserve"},
};

[[noreturn]] void bad(char const* message) { throw CoreError(ErrorCode::InvalidArgument, message); }

bool utf8(std::string_view s)
{
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i++]);
        if (c < 128) continue;
        unsigned n = 0, cp = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 31; minimum = 128; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 15; minimum = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7; minimum = 65536; }
        else return false;
        while (n--) {
            if (i == s.size()) return false;
            c = static_cast<unsigned char>(s[i++]);
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

void text(Value const& v) { if (!v.is_string() || !utf8(v.text())) bad("Expected a UTF-8 byte string"); }
void url(Value const& v, bool tracker)
{
    text(v);
    auto const& s = v.text();
    auto split = s.find("://");
    if (split == std::string::npos || split + 3 == s.size()) bad("URL requires a scheme and host");
    if (!s.starts_with("https://") && !s.starts_with("http://") && !(tracker && s.starts_with("udp://")))
        bad("Unsupported URL scheme");
    auto host_end = s.find_first_of("/?#", split + 3);
    if (host_end == split + 3) bad("URL requires a host");
    for (unsigned char c : s) if (c <= 32 || c == 127 || c == '\\') bad("URL contains whitespace or an invalid character");
}
void urls(Value const& v, bool tracker)
{
    if (!v.is_list() || v.items().empty()) bad("Expected a non-empty URL list; remove the field to clear it");
    for (auto const& item : v.items()) url(item, tracker);
}
void validate(std::string_view scope, std::string const& key, std::optional<Value> const& v)
{
    if (key.empty() || key.size() > 4096) bad("Field key must contain 1 to 4096 bytes");
    auto const* f = find_field(scope, key);
    if (!f) return; // typed unknown extension
    if (!f->editable) bad("This field is protected; it requires a rebuild or dedicated workflow");
    if (!v) return;
    auto rule = f->validation;
    if (rule == "text") text(*v);
    else if (rule == "tracker-url") url(*v, true);
    else if (rule == "http-url") url(*v, false);
    else if (rule == "http-seeds") urls(*v, false);
    else if (rule == "web-seeds") { if (v->is_string()) url(*v, false); else urls(*v, false); }
    else if (rule == "tracker-tiers") {
        if (!v->is_list() || v->items().empty()) bad("Expected non-empty tracker tiers");
        for (auto const& tier : v->items()) urls(tier, true);
    } else if (rule == "unix-seconds") {
        if (!v->is_integer() || !v->as_int64() || *v->as_int64() < 0) bad("Unix seconds must be a non-negative signed 64-bit integer");
    } else if (rule == "private-one") {
        if (!v->is_integer() || v->as_int64() != 1) bad("Private must be 1; remove the key to make it public");
    } else if (rule == "dict" && !v->is_dictionary()) bad("Expected a dictionary");
    else if (rule == "nodes") {
        if (!v->is_list()) bad("Expected a node list");
        for (auto const& node : v->items()) {
            if (!node.is_list() || node.items().size() != 2) bad("Node must be [host, port]");
            text(node.items()[0]);
            auto port = node.items()[1].as_int64();
            if (node.items()[0].text().empty() || !port || *port < 1 || *port > 65535) bad("Invalid node host or port");
        }
    }
}

OuterEdit changes(Value const& dict, OuterEdit const& patch, std::string_view scope)
{
    OuterEdit result;
    for (auto const& [key, value] : patch) {
        auto const* old = dict.find(key);
        // A display/editor no-op must not canonicalize an imported info dict,
        // change its hash, or invalidate signatures (including private=0).
        if ((!value && !old) || (value && old && metadata_values_equal(*value, *old))) continue;
        validate(scope, key, value);
        result[key] = value;
    }
    return result;
}
} // namespace

std::span<FieldDescriptor const> field_registry() { return fields; }
FieldDescriptor const* find_field(std::string_view scope, std::string_view key)
{
    for (auto const& f : fields) if (f.scope == scope && f.key == key) return &f;
    return nullptr;
}

bool metadata_values_equal(Value const& a, Value const& b)
{
    if (a.type() != b.type()) return false;
    if (a.is_integer() || a.is_string()) return a.text() == b.text();
    if (a.is_list()) {
        if (a.items().size() != b.items().size()) return false;
        for (std::size_t i = 0; i < a.items().size(); ++i)
            if (!metadata_values_equal(a.items()[i], b.items()[i])) return false;
        return true;
    }
    if (a.entries().size() != b.entries().size()) return false;
    std::unordered_map<std::string_view, Value const*> entries;
    entries.reserve(a.entries().size());
    for (auto const& e : a.entries()) entries.emplace(e.key, &e.value);
    for (auto const& e : b.entries()) {
        auto it = entries.find(e.key);
        if (it == entries.end() || !metadata_values_equal(*it->second, e.value)) return false;
    }
    return true;
}

MetadataPreview preview_metadata_edit(Metainfo const& original, OuterEdit const& outer, InfoEdit const& info, bool remove_signatures)
{
    if (!validate_metainfo(original, false).empty()) throw CoreError(ErrorCode::InvalidMetainfo, "Repair or rebuild the imported torrent before editing it");
    auto top_changes = changes(original.root(), outer, "top");
    auto info_changes = changes(original.info(), info, "info");
    if (!info_changes.empty() && original.has_legacy_root_hash())
        throw CoreError(ErrorCode::UnsupportedFormat, "Legacy Merkle torrents require a dedicated rebuild workflow");
    // Prevent two conflicting legacy comment aliases from being silently kept.
    if (top_changes.contains("comment") && original.root().find("comment.utf-8"))
        bad("The legacy comment.utf-8 alias needs a dedicated conflict resolution workflow");
    MetadataPreview out;
    out.bytes = std::string(original.bytes());
    out.old_hashes = original.info_hashes();
    if (!info_changes.empty()) {
        auto edited = apply_info_edit(original, info_changes, {remove_signatures});
        out.bytes = std::move(edited.bytes);
        out.removed_signatures = edited.removed_signatures;
    }
    if (!top_changes.empty()) out.bytes = apply_outer_edit(Metainfo::parse(out.bytes), top_changes);
    auto candidate = Metainfo::parse(out.bytes);
    auto problems = validate_metainfo(candidate, !info_changes.empty());
    if (!problems.empty()) throw CoreError(ErrorCode::InvalidMetainfo, problems.front());
    out.new_hashes = candidate.info_hashes();
    out.info_changed = candidate.raw_info() != original.raw_info();
    return out;
}
} // namespace tc::core
