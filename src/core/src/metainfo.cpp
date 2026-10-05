#include "tc/core/metainfo.hpp"

#include "tc/core/error.hpp"

#include "hash_pipeline.hpp"

#include <libtorrent/hasher.hpp>

#include <algorithm>
#include <cstring>
#include <set>
#include <array>

namespace tc::core {

namespace {

template <std::size_t N>
std::string hex(std::array<std::uint8_t, N> const& d)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(N * 2);
    for (auto b : d) {
        out += digits[b >> 4];
        out += digits[b & 0xf];
    }
    return out;
}

std::string percent_encode(std::string_view s)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        bool const unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 0xf];
        }
    }
    return out;
}

bool bytes_less(std::string_view a, std::string_view b) noexcept
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
        [](char x, char y) { return static_cast<unsigned char>(x) < static_cast<unsigned char>(y); });
}

[[noreturn]] void invalid(std::string message)
{
    throw CoreError(ErrorCode::InvalidMetainfo, std::move(message));
}

} // namespace

std::string_view to_string(MetainfoFormat f) noexcept
{
    switch (f) {
    case MetainfoFormat::V1: return "v1";
    case MetainfoFormat::V2: return "v2";
    case MetainfoFormat::Hybrid: return "hybrid";
    }
    return "unknown";
}

Sha1Digest sha1(std::string_view data)
{
    lt::hasher h;
    h.update(data.data(), static_cast<int>(data.size()));
    auto const digest = h.final();
    Sha1Digest out{};
    std::memcpy(out.data(), digest.data(), out.size());
    return out;
}

Sha256Digest sha256(std::string_view data)
{
    lt::hasher256 h;
    h.update(data.data(), static_cast<int>(data.size()));
    auto const digest = h.final();
    Sha256Digest out{};
    std::memcpy(out.data(), digest.data(), out.size());
    return out;
}

std::string to_hex(Sha1Digest const& d) { return hex(d); }
std::string to_hex(Sha256Digest const& d) { return hex(d); }

Metainfo Metainfo::parse(std::string bytes, MetainfoLimits const& limits)
{
    if (bytes.size() > limits.max_bytes)
        throw CoreError(ErrorCode::ResourceLimit,
            "Metainfo is larger than the limit of " + std::to_string(limits.max_bytes) + " bytes");

    Metainfo m;
    m.root_ = bencode::parse(bytes, limits.bencode);
    m.bytes_ = std::move(bytes);

    if (!m.root_.is_dictionary()) invalid("Metainfo root is not a dictionary");
    bencode::Value const* info = m.root_.find("info");
    if (info == nullptr) invalid("Metainfo has no info dictionary");
    if (!info->is_dictionary()) invalid("Metainfo info is not a dictionary");

    bencode::Value const* piece_length = info->find("piece length");
    if (piece_length == nullptr || !piece_length->is_integer() || piece_length->as_int64().value_or(0) <= 0)
        invalid("info.piece length is missing or invalid");

    bencode::Value const* pieces = info->find("pieces");
    if (pieces != nullptr && (!pieces->is_string() || pieces->text().size() % 20 != 0))
        invalid("info.pieces is not a multiple of 20 bytes");

    if (bencode::Value const* meta_version = info->find("meta version")) {
        if (!meta_version->is_integer()) invalid("info.meta version is not an integer");
        if (meta_version->text() != "2")
            throw CoreError(ErrorCode::UnsupportedFormat,
                "Unsupported metainfo format: meta version " + meta_version->text());
        bencode::Value const* tree = info->find("file tree");
        if (tree == nullptr || !tree->is_dictionary()) invalid("v2 metainfo has no file tree");
        m.format_ = pieces != nullptr ? MetainfoFormat::Hybrid : MetainfoFormat::V2;
    } else {
        if (pieces == nullptr) {
            if (info->find("root hash") != nullptr)
                throw CoreError(ErrorCode::UnsupportedFormat,
                    "Unsupported metainfo format: legacy BEP 30 Merkle torrent (root hash without pieces)");
            invalid("v1 metainfo has no info.pieces");
        }
        m.format_ = MetainfoFormat::V1;
    }
    m.legacy_root_hash_ = info->find("root hash") != nullptr;

    std::string_view const raw = m.raw_info();
    if (m.format_ != MetainfoFormat::V2) m.hashes_.v1 = sha1(raw);
    if (m.format_ != MetainfoFormat::V1) m.hashes_.v2 = sha256(raw);
    return m;
}

std::string_view Metainfo::raw_info() const noexcept
{
    bencode::ByteRange const r = info().raw();
    return std::string_view(bytes_).substr(r.offset, r.size);
}

std::string Metainfo::name() const
{
    bencode::Value const* n = info().find("name");
    return (n != nullptr && n->is_string()) ? n->text() : std::string();
}

namespace {

bool safe_component(std::string_view c)
{
    if (c.empty() || c == "." || c == "..") return false;
    if (c.find_first_of(std::string_view("/\\\0", 3)) != std::string_view::npos) return false;
    // Drive-relative forms such as "C:" or "C:name".
    if (c.size() >= 2 && c[1] == ':' && ((c[0] >= 'A' && c[0] <= 'Z') || (c[0] >= 'a' && c[0] <= 'z'))) return false;
    return true;
}

std::string join_path(std::string const& name, std::vector<std::string> const& parts)
{
    std::string out = name;
    for (auto const& p : parts) {
        out += '/';
        out += p;
    }
    return out;
}

std::uint64_t file_length(bencode::Value const* v, std::string_view what)
{
    if (v == nullptr || !v->is_integer()) invalid(std::string(what) + " has no integer length");
    auto const n = v->as_int64();
    if (!n || *n < 0) invalid(std::string(what) + " has an invalid length");
    return static_cast<std::uint64_t>(*n);
}

std::vector<MetainfoFile> v1_layout(bencode::Value const& info, std::string const& name)
{
    std::vector<MetainfoFile> out;
    bencode::Value const* length = info.find("length");
    bencode::Value const* files = info.find("files");
    if (length != nullptr && files != nullptr) invalid("info has both length and files");
    if (length != nullptr) {
        MetainfoFile f;
        f.torrent_path = name;
        f.length = file_length(length, "info");
        out.push_back(std::move(f));
        return out;
    }
    if (files == nullptr || !files->is_list() || files->items().empty()) invalid("info has neither length nor a non-empty files list");
    for (auto const& entry : files->items()) {
        if (!entry.is_dictionary()) invalid("info.files contains a non-dictionary entry");
        MetainfoFile f;
        f.length = file_length(entry.find("length"), "info.files entry");
        bencode::Value const* path = entry.find("path");
        if (path == nullptr || !path->is_list() || path->items().empty()) invalid("info.files entry has no path");
        for (auto const& c : path->items()) {
            if (!c.is_string() || !safe_component(c.text())) invalid("info.files entry has an unsafe path component");
            f.path.push_back(c.text());
        }
        if (bencode::Value const* attr = entry.find("attr"); attr != nullptr && attr->is_string())
            f.pad = attr->text().find('p') != std::string::npos;
        f.torrent_path = join_path(name, f.path);
        out.push_back(std::move(f));
    }
    return out;
}

void v2_walk(bencode::Value const& node, std::vector<std::string>& prefix, std::vector<MetainfoFile>& out)
{
    if (!node.is_dictionary() || node.entries().empty()) invalid("v2 file tree contains an empty or non-dictionary node");
    for (auto const& entry : node.entries()) {
        if (!safe_component(entry.key)) invalid("v2 file tree has an unsafe path component");
        bencode::Value const& child = entry.value;
        if (!child.is_dictionary()) invalid("v2 file tree contains a non-dictionary node");
        prefix.push_back(entry.key);
        if (bencode::Value const* props = child.find("")) {
            if (child.entries().size() != 1) invalid("v2 file tree node is both a file and a folder");
            if (!props->is_dictionary()) invalid("v2 file properties are not a dictionary");
            MetainfoFile f;
            f.path = prefix;
            f.length = file_length(props->find("length"), "v2 file");
            bencode::Value const* root = props->find("pieces root");
            if (f.length > 0) {
                if (root == nullptr || !root->is_string() || root->text().size() != 32)
                    invalid("v2 file has no valid 32-byte pieces root");
                Sha256Digest d{};
                std::memcpy(d.data(), root->text().data(), 32);
                f.pieces_root = d;
            } else if (root != nullptr) {
                invalid("empty v2 file has a pieces root");
            }
            out.push_back(std::move(f));
        } else {
            v2_walk(child, prefix, out);
        }
        prefix.pop_back();
    }
}

std::vector<MetainfoFile> v2_layout(bencode::Value const& info, std::string const& name)
{
    bencode::Value const* tree = info.find("file tree");
    if (tree == nullptr) invalid("v2 metainfo has no file tree");
    std::vector<MetainfoFile> out;
    std::vector<std::string> prefix;
    v2_walk(*tree, prefix, out);
    // A single-file torrent's tree holds exactly one file named after `name`.
    bool const single = out.size() == 1 && out.front().path.size() == 1 && out.front().path.front() == name
        && tree->entries().size() == 1;
    for (auto& f : out) {
        if (single) f.path.clear();
        f.torrent_path = join_path(name, f.path);
    }
    return out;
}

void check_canonical(bencode::Value const& v, std::vector<std::string>& problems, std::string const& where)
{
    if (v.is_dictionary()) {
        if (!v.canonical_order()) problems.push_back(where + " has keys that are not in sorted order");
        for (auto const& e : v.entries()) check_canonical(e.value, problems, where);
    } else if (v.is_list()) {
        for (auto const& item : v.items()) check_canonical(item, problems, where);
    }
}

std::vector<std::string_view> merged_keys(bencode::Value const& dict, std::map<std::string, std::optional<bencode::Value>> const& edit)
{
    std::vector<std::string_view> keys;
    for (auto const& e : dict.entries()) {
        auto it = edit.find(e.key);
        if (it != edit.end() && !it->second.has_value()) continue; // removed
        keys.push_back(e.key);
    }
    for (auto const& [key, value] : edit)
        if (value.has_value() && dict.find(key) == nullptr) keys.push_back(key);
    std::sort(keys.begin(), keys.end(), bytes_less);
    return keys;
}

void append_key(std::string& out, std::string_view key)
{
    out += std::to_string(key.size());
    out += ':';
    out += key;
}

} // namespace

std::vector<MetainfoFile> metainfo_files(Metainfo const& m)
{
    bencode::Value const& info = m.info();
    bencode::Value const* name_value = info.find("name");
    if (name_value == nullptr || !name_value->is_string() || !safe_component(name_value->text()))
        invalid("info.name is missing or unsafe");
    std::string const name = name_value->text();

    if (m.format() == MetainfoFormat::V2) return v2_layout(info, name);

    std::vector<MetainfoFile> files = v1_layout(info, name);
    if (m.format() == MetainfoFormat::Hybrid) {
        std::vector<MetainfoFile> const v2 = v2_layout(info, name);
        std::size_t k = 0;
        for (auto& f : files) {
            if (f.pad) continue;
            if (k >= v2.size() || v2[k].path != f.path || v2[k].length != f.length)
                invalid("hybrid v1 and v2 file lists do not describe the same files in the same order");
            f.pieces_root = v2[k].pieces_root;
            ++k;
        }
        if (k != v2.size()) invalid("hybrid v1 and v2 file lists do not describe the same files in the same order");
    }
    return files;
}

std::vector<std::string> validate_metainfo(Metainfo const& m, bool require_canonical_info)
{
    std::vector<std::string> problems;
    std::vector<MetainfoFile> files;
    try {
        files = metainfo_files(m);
    } catch (CoreError const& e) {
        problems.emplace_back(e.what());
        return problems;
    }

    bencode::Value const& info = m.info();
    // BEP 52 requires canonical info. Legacy v1 imports may retain their raw
    // ordering during outer-only edits; every structural check still runs.
    if (require_canonical_info || m.format() != MetainfoFormat::V1) check_canonical(info, problems, "info");

    auto const piece_length = static_cast<std::uint64_t>(info.find("piece length")->as_int64().value_or(0));
    bool const has_v2 = m.format() != MetainfoFormat::V1;
    if (has_v2 && (piece_length < static_cast<std::uint64_t>(detail::block_size) || (piece_length & (piece_length - 1)) != 0))
        problems.emplace_back("v2 piece length must be a power of two of at least 16 KiB");
    if (piece_length == 0) return problems;

    if (m.format() != MetainfoFormat::V2) {
        std::uint64_t offset = 0;
        for (auto const& f : files) {
            if (m.format() == MetainfoFormat::Hybrid && !f.pad && f.length > 0 && offset % piece_length != 0)
                problems.push_back("hybrid file is not aligned to a piece boundary: " + f.torrent_path);
            if (f.length > UINT64_MAX - offset) {
                problems.emplace_back("total length overflows");
                return problems;
            }
            offset += f.length;
        }
        std::uint64_t const expected = (offset + piece_length - 1) / piece_length;
        std::uint64_t const actual = info.find("pieces")->text().size() / 20;
        if (expected != actual)
            problems.push_back("info.pieces has " + std::to_string(actual) + " hashes, the layout needs " + std::to_string(expected));
    }

    if (has_v2) {
        bencode::Value const* layers = m.root().find("piece layers");
        if (layers != nullptr && !layers->is_dictionary()) {
            problems.emplace_back("piece layers is not a dictionary");
            return problems;
        }
        std::set<std::string> needed;
        for (auto const& f : files) {
            if (f.pad || !f.pieces_root || f.length <= piece_length) continue;
            std::string const key(reinterpret_cast<char const*>(f.pieces_root->data()), 32);
            needed.insert(key);
            bencode::Value const* layer = layers != nullptr ? layers->find(key) : nullptr;
            if (layer == nullptr || !layer->is_string()) {
                problems.push_back("piece layer missing for " + f.torrent_path);
                continue;
            }
            std::uint64_t const pieces = (f.length + piece_length - 1) / piece_length;
            if (layer->text().size() != pieces * 32) {
                problems.push_back("piece layer has the wrong size for " + f.torrent_path);
                continue;
            }
            std::vector<Sha256Digest> roots(static_cast<std::size_t>(pieces));
            for (std::size_t i = 0; i < roots.size(); ++i) std::memcpy(roots[i].data(), layer->text().data() + i * 32, 32);
            if (detail::file_root(roots, static_cast<int>(piece_length)) != *f.pieces_root)
                problems.push_back("piece layer does not match the pieces root of " + f.torrent_path);
        }
        if (layers != nullptr)
            for (auto const& e : layers->entries())
                if (!needed.count(e.key)) problems.emplace_back("piece layers has an entry that matches no file");
    }
    return problems;
}

InfoEditResult apply_info_edit(Metainfo const& original, InfoEdit const& edit, InfoEditOptions const& options)
{
    static constexpr std::array<std::string_view, 7> layout_keys{
        "name", "piece length", "pieces", "length", "files", "meta version", "file tree"};
    for (auto const& [key, value] : edit) {
        if (std::find(layout_keys.begin(), layout_keys.end(), key) != layout_keys.end())
            throw CoreError(ErrorCode::InvalidArgument, "info." + key + " describes the payload layout; changing it requires a rebuild");
        if (key == "private" && value.has_value() && (!value->is_integer() || value->text() != "1"))
            throw CoreError(ErrorCode::InvalidArgument, "info.private can only be set to 1 or removed");
    }

    bool const signed_torrent = original.root().find("signatures") != nullptr;
    if (signed_torrent && !options.remove_invalidated_signatures)
        throw CoreError(ErrorCode::InvalidArgument,
            "The torrent is signed and the signatures cover info; remove them explicitly to continue");

    std::string_view const source = original.bytes();
    bencode::Value const& info = original.info();

    std::string new_info = "d";
    for (std::string_view key : merged_keys(info, edit)) {
        append_key(new_info, key);
        auto it = edit.find(std::string(key));
        if (it != edit.end()) {
            bencode::encode_to(*it->second, new_info);
        } else {
            bencode::ByteRange const r = info.find(key)->raw();
            new_info += source.substr(r.offset, r.size);
        }
    }
    new_info += 'e';

    OuterEdit removal;
    if (signed_torrent) removal.emplace("signatures", std::nullopt);
    std::string out = "d";
    for (std::string_view key : merged_keys(original.root(), removal)) {
        append_key(out, key);
        if (key == "info") {
            out += new_info;
        } else {
            bencode::ByteRange const r = original.root().find(key)->raw();
            out += source.substr(r.offset, r.size);
        }
    }
    out += 'e';

    InfoEditResult result;
    result.old_hashes = original.info_hashes();
    result.new_hashes = Metainfo::parse(out).info_hashes();
    result.bytes = std::move(out);
    result.removed_signatures = signed_torrent;
    return result;
}

std::string apply_outer_edit(Metainfo const& original, OuterEdit const& edit)
{
    for (auto const& [key, value] : edit) {
        if (key == "info")
            throw CoreError(ErrorCode::InvalidArgument, "The info dictionary cannot be changed by an outer-only edit");
        if (key == "piece layers")
            throw CoreError(ErrorCode::InvalidArgument, "piece layers is a computed field and cannot be edited directly");
    }

    std::string_view const source = original.bytes();
    std::vector<std::string_view> keys;
    for (auto const& e : original.root().entries()) {
        auto it = edit.find(e.key);
        if (it != edit.end() && !it->second.has_value()) continue; // removed
        keys.push_back(e.key);
    }
    for (auto const& [key, value] : edit) {
        if (!value.has_value()) continue;
        if (original.root().find(key) == nullptr) keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), bytes_less);

    std::string out;
    out.reserve(original.bytes().size() + 256);
    out += 'd';
    for (std::string_view key : keys) {
        out += std::to_string(key.size());
        out += ':';
        out += key;
        if (key == "info") {
            out += original.raw_info();
            continue;
        }
        auto it = edit.find(std::string(key));
        if (it != edit.end()) {
            bencode::encode_to(*it->second, out);
        } else {
            bencode::ByteRange const r = original.root().find(key)->raw();
            out += source.substr(r.offset, r.size);
        }
    }
    out += 'e';
    return out;
}

std::string make_magnet(Metainfo const& m, MagnetOptions const& options)
{
    std::string uri = "magnet:?";
    bool first = true;
    auto add = [&](std::string_view key, std::string_view value) {
        if (!first) uri += '&';
        first = false;
        uri += key;
        uri += '=';
        uri += value;
    };

    if (m.info_hashes().v1) add("xt", "urn:btih:" + to_hex(*m.info_hashes().v1));
    if (m.info_hashes().v2) add("xt", "urn:btmh:1220" + to_hex(*m.info_hashes().v2));

    if (options.include_name) {
        std::string const name = m.name();
        if (!name.empty()) add("dn", percent_encode(name));
    }

    if (options.include_trackers) {
        std::vector<std::string> trackers;
        std::set<std::string> seen;
        auto push = [&](bencode::Value const& v) {
            if (v.is_string() && !v.text().empty() && seen.insert(v.text()).second) trackers.push_back(v.text());
        };
        if (bencode::Value const* list = m.root().find("announce-list"); list != nullptr && list->is_list()) {
            for (auto const& tier : list->items())
                if (tier.is_list())
                    for (auto const& url : tier.items()) push(url);
        }
        if (bencode::Value const* announce = m.root().find("announce")) push(*announce);
        for (auto const& t : trackers) add("tr", percent_encode(t));
    }

    if (options.include_web_seeds) {
        if (bencode::Value const* seeds = m.root().find("url-list")) {
            if (seeds->is_string()) add("ws", percent_encode(seeds->text()));
            else if (seeds->is_list())
                for (auto const& s : seeds->items())
                    if (s.is_string()) add("ws", percent_encode(s.text()));
        }
    }
    return uri;
}

} // namespace tc::core
