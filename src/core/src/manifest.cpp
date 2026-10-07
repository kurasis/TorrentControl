#include "tc/core/manifest.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <system_error>
#include <tuple>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace tc::core {

namespace {

bool valid_utf8(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        auto const c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xe0) == 0xc0) { len = 2; cp = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { len = 3; cp = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            auto const cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        // Reject overlong encodings, surrogates and out-of-range code points.
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += len;
    }
    return true;
}

#ifndef _WIN32

// Decodes valid UTF-8 (checked by the caller) into code points.
std::vector<char32_t> decode_utf8(std::string_view s)
{
    std::vector<char32_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        auto const c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
        char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1fu) : len == 3 ? (c & 0x0fu) : (c & 0x07u);
        for (std::size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3fu);
        out.push_back(cp);
        i += len;
    }
    return out;
}

void append_utf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

// Simple uppercase mapping for the scripts most likely to appear in file
// names. Pairs in the "alternating" Latin Extended-A and Cyrillic blocks map
// the lowercase member to its uppercase neighbour.
char32_t simple_upper(char32_t c)
{
    auto pair_even_upper = [](char32_t ch) { return (ch & 1u) ? ch - 1 : ch; };
    auto pair_odd_upper = [](char32_t ch) { return (ch & 1u) ? ch : ch - 1; };

    if (c >= U'a' && c <= U'z') return c - 0x20;
    if (c < 0x80) return c;
    if (c == 0xb5) return 0x39c;
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 0x20;
    if (c == 0xff) return 0x178;
    if (c >= 0x100 && c <= 0x137 && c != 0x131) return pair_even_upper(c);
    if (c == 0x131) return U'I';
    if (c >= 0x139 && c <= 0x148) return pair_odd_upper(c);
    if (c >= 0x14a && c <= 0x177) return pair_even_upper(c);
    if (c >= 0x179 && c <= 0x17e) return pair_odd_upper(c);
    if (c == 0x17f) return U'S';
    if (c == 0x3ac) return 0x386;
    if (c >= 0x3ad && c <= 0x3af) return c - 0x25;
    if (c == 0x3c2) return 0x3a3;
    if (c >= 0x3b1 && c <= 0x3cb) return c - 0x20;
    if (c == 0x3cc) return 0x38c;
    if (c == 0x3cd || c == 0x3ce) return c - 0x3f;
    if (c >= 0x430 && c <= 0x44f) return c - 0x20;
    if (c >= 0x450 && c <= 0x45f) return c - 0x50;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48a && c <= 0x4bf)) return pair_even_upper(c);
    if (c >= 0x4c1 && c <= 0x4ce) return pair_odd_upper(c);
    if (c == 0x4cf) return 0x4c0;
    if (c >= 0x4d0 && c <= 0x52f) return pair_even_upper(c);
    if (c >= 0x561 && c <= 0x586) return c - 0x30;
    if (c >= 0xff41 && c <= 0xff5a) return c - 0x20;
    return c;
}

#endif

bool is_reserved_device_name(std::string_view component)
{
    // Device names are reserved with or without an extension ("CON.txt").
    std::string base(component.substr(0, component.find('.')));
    for (char& c : base)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    while (!base.empty() && base.back() == ' ') base.pop_back();
    static constexpr std::array<std::string_view, 6> fixed{"con", "prn", "aux", "nul", "conin$", "conout$"};
    if (std::find(fixed.begin(), fixed.end(), base) != fixed.end()) return true;
    if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9')
        return true;
    // COM and LPT with superscript digits ¹ ² ³ are reserved as well.
    if (base.size() == 5 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] == '\xc2'
        && (base[4] == '\xb9' || base[4] == '\xb2' || base[4] == '\xb3'))
        return true;
    return false;
}

std::string component_problem(std::string_view c)
{
    if (c.empty()) return "empty path component";
    if (c == "." || c == "..") return "relative path component '" + std::string(c) + "'";
    if (!valid_utf8(c)) return "path component is not valid UTF-8";
    for (char ch : c) {
        auto const u = static_cast<unsigned char>(ch);
        if (u < 0x20) return "control character in path component";
        if (std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos)
            return "character '" + std::string(1, ch) + "' is not allowed in Windows file names";
    }
    if (c.back() == '.' || c.back() == ' ') return "path component ends with a dot or space";
    if (is_reserved_device_name(c)) return "'" + std::string(c) + "' is a reserved Windows device name";
    return {};
}

std::string join(std::vector<std::string> const& parts, std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

void sort_entries(std::vector<ManifestEntry>& entries)
{
    std::sort(entries.begin(), entries.end(), [](ManifestEntry const& a, ManifestEntry const& b) {
        return std::lexicographical_compare(a.torrent_path.begin(), a.torrent_path.end(), b.torrent_path.begin(),
            b.torrent_path.end(), [](std::string const& x, std::string const& y) {
                return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(),
                    [](char l, char r) { return static_cast<unsigned char>(l) < static_cast<unsigned char>(r); });
            });
    });
}

void assign_ids(Manifest& m)
{
    sort_entries(m.entries);
    for (std::size_t i = 0; i < m.entries.size(); ++i) m.entries[i].source_id = "src-" + std::to_string(i);
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool glob_impl(std::string_view p, std::string_view s)
{
    while (!p.empty()) {
        if (p.starts_with("**")) {
            std::string_view rest = p.substr(2);
            if (rest.starts_with('/')) {
                // "**/" also matches zero folders.
                if (glob_impl(rest.substr(1), s)) return true;
            }
            for (std::size_t i = 0; i <= s.size(); ++i)
                if (glob_impl(rest, s.substr(i))) return true;
            return false;
        }
        if (p.front() == '*') {
            for (std::size_t i = 0; i <= s.size(); ++i) {
                if (glob_impl(p.substr(1), s.substr(i))) return true;
                if (i < s.size() && s[i] == '/') break;
            }
            return false;
        }
        if (s.empty()) return false;
        if (p.front() == '?') {
            if (s.front() == '/') return false;
        } else if (ascii_lower(p.front()) != ascii_lower(s.front())) {
            return false;
        }
        p.remove_prefix(1);
        s.remove_prefix(1);
    }
    return s.empty();
}

struct IdentityLess {
    bool operator()(native::FileIdentity const& a, native::FileIdentity const& b) const
    {
        return std::tie(a.volume, a.id) < std::tie(b.volume, b.id);
    }
};

std::string path_key(fs::path const& p)
{
    std::error_code ec;
    fs::path const abs = fs::absolute(p, ec);
    std::string const s = to_utf8((ec ? p : abs).lexically_normal());
#ifdef _WIN32
    return fold_case(s);
#else
    return s;
#endif
}

std::string hex32(std::uint32_t v)
{
    char buf[11];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

// Resolves an explicitly selected link to its target. Explicit selection is
// a user decision, unlike links found while enumerating a folder.
fs::path resolve_selected(fs::path const& p, native::EntryInfo const& info)
{
    if (info.kind != native::EntryKind::Symlink && info.kind != native::EntryKind::Junction) return p;
    std::error_code ec;
    fs::path target = fs::canonical(p, ec);
    if (ec)
        throw CoreError(ErrorCode::SourceMissing, "The selected link points to a missing target: " + to_utf8(p.filename()),
            ec.value());
    return target;
}

ManifestEntry make_entry(fs::path const& path, native::EntryInfo const& info, std::vector<std::string> components,
    std::string reason)
{
    std::optional<native::FileObservation> obs = native::observe(path);
    if (!obs) throw CoreError(ErrorCode::SourceMissing, "Source file not found: " + to_utf8(path.filename()));
    ManifestEntry e;
    e.source_path = path;
    e.torrent_path = std::move(components);
    e.length = obs->size;
    e.observed = *obs;
    e.flags.hidden = info.hidden;
    e.flags.system = info.system;
    e.flags.cloud_placeholder = info.cloud_placeholder;
    e.flags.requires_hydration = info.requires_hydration;
    e.inclusion_reason = std::move(reason);
    return e;
}

class Scanner {
public:
    Scanner(fs::path root, ScanOptions const& options, std::stop_token stop)
        : root_(std::move(root)), options_(options), stop_(std::move(stop))
    {
        if (options_.use_default_exclusions) rules_ = default_exclusions();
        rules_.insert(rules_.end(), options_.exclusions.begin(), options_.exclusions.end());
        // Only the exact paths are excluded. A hard-link alias of the output
        // stays in the selection so check_output_target() reports the
        // conflict instead of the alias silently disappearing (W06).
        for (auto const& p : options_.excluded_paths) excluded_keys_.insert(path_key(p));
    }

    Manifest run()
    {
        std::optional<native::EntryInfo> info = native::stat_entry(root_);
        if (!info) throw CoreError(ErrorCode::SourceMissing, "Source not found: " + to_utf8(root_.filename()));

        m_.name = to_utf8(root_.filename());
        fs::path const resolved = resolve_selected(root_, *info);
        if (resolved != root_) {
            info = native::stat_entry(resolved);
            if (!info) throw CoreError(ErrorCode::SourceMissing, "Source not found: " + to_utf8(root_.filename()));
        }

        if (info->kind == native::EntryKind::Regular) {
            // An explicitly selected file is never excluded silently; if it is
            // also the output, check_output_target() reports the conflict.
            m_.mode = LayoutMode::SingleFile;
            m_.entries.push_back(make_entry(resolved, *info, {}, "selected file"));
        } else if (info->kind == native::EntryKind::Directory) {
            m_.mode = LayoutMode::Directory;
            root_canonical_ = canonical_or_self(resolved);
            if (options_.follow_links) visit_identity(resolved);
            walk(resolved);
        } else {
            throw CoreError(ErrorCode::SourceUnreadable, "Source is neither a file nor a folder: " + m_.name);
        }
        assign_ids(m_);
        return std::move(m_);
    }

private:
    struct Pending {
        fs::path dir;
        std::vector<std::string> prefix;
        int depth = 0;
    };

    static fs::path canonical_or_self(fs::path const& p)
    {
        std::error_code ec;
        fs::path c = fs::canonical(p, ec);
        return ec ? p : c;
    }

    void check_stop()
    {
        if (stop_.stop_requested()) throw CoreError(ErrorCode::Cancelled, "Scanning was cancelled");
    }

    // Returns false when the folder was already visited (a cycle).
    bool visit_identity(fs::path const& dir)
    {
        auto const obs = native::observe(dir);
        if (!obs || !obs->identity.valid) return true;
        return visited_.insert(obs->identity).second;
    }

    bool is_excluded_path(fs::path const& p) { return excluded_keys_.count(path_key(p)) != 0; }

    ExclusionRule const* matching_rule(std::string const& relative)
    {
        for (auto const& r : rules_)
            if (glob_match(r.pattern, relative)) return &r;
        return nullptr;
    }

    // Applies the cloud and output policies to a regular file. Returns true
    // when the file was recorded as skipped.
    bool skip_file(fs::path const& path, native::EntryInfo const& info)
    {
        if (is_excluded_path(path)) {
            m_.skipped.push_back({path, SkipKind::OutputFile, "current output file"});
            return true;
        }
        if (info.requires_hydration && options_.cloud_policy == CloudPolicy::SkipUnavailable) {
            m_.skipped.push_back({path, SkipKind::CloudPlaceholder, "cloud file not available locally"});
            return true;
        }
        return false;
    }

    void add_file(fs::path const& path, native::EntryInfo const& info, std::vector<std::string> components,
        std::string reason)
    {
        if (skip_file(path, info)) return;
        try {
            m_.entries.push_back(make_entry(path, info, std::move(components), std::move(reason)));
        } catch (CoreError const& e) {
            // Vanished between listing and observation: no longer selected.
            if (e.code() == ErrorCode::SourceMissing) return;
            m_.unreadable.push_back({path, e.what(), e.os_error()});
        }
    }

    void walk(fs::path const& start)
    {
        std::vector<Pending> stack;
        stack.push_back({start, {}, 0});
        while (!stack.empty()) {
            check_stop();
            Pending current = std::move(stack.back());
            stack.pop_back();

            std::vector<native::EntryInfo> children;
            try {
                children = native::list_directory(current.dir);
            } catch (CoreError const& e) {
                m_.unreadable.push_back({current.dir, e.what(), e.os_error()});
                continue;
            }

            std::size_t counter = 0;
            for (auto& child : children) {
                if ((++counter & 0x3ff) == 0) check_stop();
                std::vector<std::string> components = current.prefix;
                components.push_back(to_utf8(child.path.filename()));
                std::string const relative = join(components, components.size());

                if (ExclusionRule const* rule = matching_rule(relative)) {
                    m_.skipped.push_back({child.path, SkipKind::Excluded, rule->reason});
                    continue;
                }
                handle(child, std::move(components), current.depth, stack);
            }
        }
    }

    void handle(native::EntryInfo const& child, std::vector<std::string> components, int depth, std::vector<Pending>& stack)
    {
        using native::EntryKind;
        switch (child.kind) {
        case EntryKind::Regular:
            add_file(child.path, child, std::move(components), "in selected folder");
            return;
        case EntryKind::Directory:
            if (!options_.recursive) {
                m_.skipped.push_back({child.path, SkipKind::NotRecursive, "subfolder of a non-recursive selection"});
            } else if (depth + 1 > options_.max_depth) {
                m_.skipped.push_back({child.path, SkipKind::DepthLimit, "folder depth limit reached"});
            } else if (options_.follow_links && !visit_identity(child.path)) {
                m_.skipped.push_back({child.path, SkipKind::LinkCycle, "folder already visited (cycle)"});
            } else {
                stack.push_back({child.path, std::move(components), depth + 1});
            }
            return;
        case EntryKind::Symlink:
        case EntryKind::Junction:
            if (!options_.follow_links) {
                bool const junction = child.kind == EntryKind::Junction;
                m_.skipped.push_back({child.path, junction ? SkipKind::Junction : SkipKind::SymbolicLink,
                    junction ? "junction (not followed by default)" : "symbolic link (not followed by default)"});
                return;
            }
            follow(child, std::move(components), depth, stack);
            return;
        case EntryKind::OtherReparsePoint:
            m_.skipped.push_back({child.path, SkipKind::ReparsePoint, "reparse point " + hex32(child.reparse_tag) + " (not followed)"});
            return;
        case EntryKind::Other:
            m_.skipped.push_back({child.path, SkipKind::SpecialFile, "not a regular file"});
            return;
        }
    }

    void follow(native::EntryInfo const& link, std::vector<std::string> components, int depth, std::vector<Pending>& stack)
    {
        auto const kind = link.kind == native::EntryKind::Junction ? SkipKind::Junction : SkipKind::SymbolicLink;
        std::error_code ec;
        fs::path const target = fs::canonical(link.path, ec);
        if (ec) {
            m_.skipped.push_back({link.path, kind, "link target is missing or unreachable"});
            return;
        }
        fs::path const rel = target.lexically_relative(root_canonical_);
        if (rel.empty() || *rel.begin() == "..") {
            m_.skipped.push_back({link.path, SkipKind::OutsideRoot, "link target is outside the selected folder"});
            return;
        }
        std::optional<native::EntryInfo> info = native::stat_entry(target);
        if (!info) {
            m_.skipped.push_back({link.path, kind, "link target is missing or unreachable"});
            return;
        }
        if (info->kind == native::EntryKind::Regular) {
            add_file(target, *info, std::move(components), "followed link");
        } else if (info->kind == native::EntryKind::Directory) {
            if (depth + 1 > options_.max_depth) {
                m_.skipped.push_back({link.path, SkipKind::DepthLimit, "folder depth limit reached"});
            } else if (!visit_identity(target)) {
                m_.skipped.push_back({link.path, SkipKind::LinkCycle, "link leads back to a folder already visited (cycle)"});
            } else {
                stack.push_back({target, std::move(components), depth + 1});
            }
        } else {
            m_.skipped.push_back({link.path, kind, "link target is not a regular file or folder"});
        }
    }

    fs::path root_;
    fs::path root_canonical_;
    ScanOptions const& options_;
    std::stop_token stop_;
    std::vector<ExclusionRule> rules_;
    std::set<std::string> excluded_keys_;
    std::set<native::FileIdentity, IdentityLess> visited_;
    Manifest m_;
};

} // namespace

std::string to_utf8(fs::path const& p)
{
    std::u8string const u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path path_from_utf8(std::string_view utf8)
{
    if (utf8.find('\0') != std::string_view::npos)
        throw CoreError(ErrorCode::InvalidPath, "File paths cannot contain NUL characters");
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string_view to_string(SkipKind kind) noexcept
{
    switch (kind) {
    case SkipKind::Excluded: return "excluded";
    case SkipKind::OutputFile: return "output-file";
    case SkipKind::SymbolicLink: return "symbolic-link";
    case SkipKind::Junction: return "junction";
    case SkipKind::ReparsePoint: return "reparse-point";
    case SkipKind::SpecialFile: return "special-file";
    case SkipKind::NotRecursive: return "not-recursive";
    case SkipKind::CloudPlaceholder: return "cloud-placeholder";
    case SkipKind::LinkCycle: return "link-cycle";
    case SkipKind::OutsideRoot: return "outside-root";
    case SkipKind::DepthLimit: return "depth-limit";
    }
    return "unknown";
}

std::string fold_case(std::string_view utf8)
{
    if (!valid_utf8(utf8)) return std::string(utf8);
#ifdef _WIN32
    if (utf8.empty()) return {};
    int const wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wide_len);
    std::wstring upper(wide.size(), L'\0');
    int const n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.c_str(), wide_len, upper.data(),
        static_cast<int>(upper.size()), nullptr, nullptr, 0);
    if (n <= 0) return std::string(utf8);
    upper.resize(static_cast<std::size_t>(n));
    int const out_len = WideCharToMultiByte(CP_UTF8, 0, upper.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(out_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, upper.data(), n, out.data(), out_len, nullptr, nullptr);
    return out;
#else
    std::string out;
    out.reserve(utf8.size());
    for (char32_t cp : decode_utf8(utf8)) append_utf8(out, simple_upper(cp));
    return out;
#endif
}

bool glob_match(std::string_view pattern, std::string_view relative_path)
{
    if (pattern.find('/') == std::string_view::npos) {
        auto const slash = relative_path.rfind('/');
        if (slash != std::string_view::npos) relative_path.remove_prefix(slash + 1);
    }
    return glob_impl(pattern, relative_path);
}

std::vector<ExclusionRule> default_exclusions()
{
    return {
        {".*.tc-*.tmp", "TorrentControl temporary output file"},
        {"*.tcproject", "TorrentControl project file"},
    };
}

std::uint64_t Manifest::total_length() const
{
    std::uint64_t total = 0;
    for (auto const& e : entries) {
        if (e.length > UINT64_MAX - total) throw CoreError(ErrorCode::ResourceLimit, "Total payload length overflows");
        total += e.length;
    }
    return total;
}

std::string Manifest::torrent_path_string(ManifestEntry const& e) const
{
    if (mode == LayoutMode::SingleFile) return name;
    return name + "/" + join(e.torrent_path, e.torrent_path.size());
}

ManifestEntry const* Manifest::find(std::string_view source_id) const
{
    for (auto const& e : entries)
        if (e.source_id == source_id) return &e;
    return nullptr;
}

std::vector<ManifestIssue> validate_manifest(Manifest const& m, ManifestLimits const& limits)
{
    std::vector<ManifestIssue> issues;
    if (std::string p = component_problem(m.name); !p.empty())
        issues.push_back({ErrorCode::InvalidPath, "Torrent name: " + p});

    for (auto const& u : m.unreadable)
        issues.push_back({ErrorCode::SourceUnreadable, "Cannot read " + to_utf8(u.path) + ": " + u.message});

    if (m.entries.size() > limits.max_files) {
        issues.push_back({ErrorCode::ResourceLimit, "The selection has " + std::to_string(m.entries.size())
                + " files; the limit is " + std::to_string(limits.max_files)});
        return issues;
    }
    if (m.entries.size() > limits.warn_files)
        issues.push_back({ErrorCode::ResourceLimit,
            "The selection has " + std::to_string(m.entries.size()) + " files; large manifests need more memory and time",
            Severity::Warning});

    if (m.entries.empty()) {
        issues.push_back({ErrorCode::EmptyPayload, "The selection contains no files"});
        return issues;
    }
    if (m.mode == LayoutMode::SingleFile && m.entries.size() != 1)
        issues.push_back({ErrorCode::InvalidArgument, "Single-file mode requires exactly one file"});

    std::uint64_t total = 0;
    bool overflow = false;
    for (auto const& e : m.entries) {
        if (e.length > UINT64_MAX - total) overflow = true;
        else total += e.length;
    }
    if (overflow) issues.push_back({ErrorCode::ResourceLimit, "Total payload length overflows"});
    // Product/engine limitation (section 5.2): the pinned engine cannot create
    // a torrent whose total payload length is zero.
    else if (total == 0)
        issues.push_back({ErrorCode::EmptyPayload, "All selected files are empty; a torrent needs at least one byte of payload"});
    else if (total > static_cast<std::uint64_t>(INT64_MAX))
        issues.push_back({ErrorCode::ResourceLimit, "Total payload length exceeds the engine limit"});

    if (m.mode == LayoutMode::SingleFile) return issues;

    std::map<std::string, std::string> files_folded; // folded path -> original
    std::set<std::string> dirs_folded;
    for (auto const& e : m.entries) {
        if (e.torrent_path.empty()) {
            issues.push_back({ErrorCode::InvalidPath, "A file in a directory torrent has an empty destination path",
                Severity::Error, e.source_id});
            continue;
        }
        bool component_ok = true;
        for (auto const& c : e.torrent_path) {
            if (std::string p = component_problem(c); !p.empty()) {
                issues.push_back({ErrorCode::InvalidPath, m.torrent_path_string(e) + ": " + p, Severity::Error, e.source_id});
                component_ok = false;
            }
        }
        if (!component_ok) continue;

        std::string const path = join(e.torrent_path, e.torrent_path.size());
        std::string const folded = fold_case(path);
        auto const [it, inserted] = files_folded.emplace(folded, path);
        if (!inserted) {
            issues.push_back({ErrorCode::PathCollision,
                it->second == path ? "Duplicate destination path: " + path
                                   : "Destination paths differ only by case: " + it->second + " and " + path,
                Severity::Error, e.source_id});
        }
        for (std::size_t n = 1; n < e.torrent_path.size(); ++n) dirs_folded.insert(fold_case(join(e.torrent_path, n)));
    }
    for (auto const& [folded, original] : files_folded)
        if (dirs_folded.count(folded))
            issues.push_back({ErrorCode::PathCollision, "Destination is used both as a file and as a folder: " + original});
    return issues;
}

void require_valid_manifest(Manifest const& m, ManifestLimits const& limits)
{
    for (auto const& issue : validate_manifest(m, limits)) {
        if (issue.severity != Severity::Error) continue;
        CoreError error(issue.code, issue.message);
        error.with_phase(Phase::Preflight);
        if (!issue.source_id.empty()) error.with_source(issue.source_id);
        throw error;
    }
}

Manifest scan_source(fs::path const& source, ScanOptions const& options, std::stop_token stop)
{
    std::error_code ec;
    fs::path const root = fs::absolute(source, ec);
    if (ec) throw CoreError(ErrorCode::SourceMissing, "Cannot resolve source path: " + ec.message(), ec.value());
    try {
        return Scanner(root.lexically_normal(), options, std::move(stop)).run();
    } catch (CoreError& e) {
        e.with_phase(Phase::Scanning);
        throw;
    }
}

void set_destination(Manifest& m, std::string_view source_id, std::vector<std::string> torrent_path)
{
    if (m.mode != LayoutMode::Directory)
        throw CoreError(ErrorCode::InvalidArgument, "Single-file torrents have no destination folders; rename the torrent instead");
    for (auto& e : m.entries) {
        if (e.source_id != source_id) continue;
        e.torrent_path = std::move(torrent_path);
        sort_entries(m.entries);
        ++m.revision;
        return;
    }
    throw CoreError(ErrorCode::InvalidArgument, "Unknown source ID: " + std::string(source_id));
}

std::vector<ManifestIssue> recheck_sources(Manifest const& m)
{
    std::vector<ManifestIssue> issues;
    for (auto const& e : m.entries) {
        std::string const label = m.torrent_path_string(e);
        std::optional<native::FileObservation> now;
        try {
            now = native::observe(e.source_path);
        } catch (CoreError const& err) {
            issues.push_back({ErrorCode::SourceUnreadable, label + ": " + err.what(), Severity::Error, e.source_id});
            continue;
        }
        if (!now) {
            issues.push_back({ErrorCode::SourceMissing, label + ": the source file no longer exists", Severity::Error, e.source_id});
            continue;
        }
        native::FileObservation expected = e.observed;
        if (!expected.identity.valid) {
            // Hand-built entry without a frozen observation: only the length is known.
            expected = *now;
            expected.size = e.length;
        }
        std::vector<std::string> const changes = native::describe_changes(expected, *now);
        if (changes.empty()) continue;
        std::string message = label + ": ";
        for (std::size_t i = 0; i < changes.size(); ++i) message += (i ? ", " : "") + changes[i];
        issues.push_back({ErrorCode::SourceChanged, message, Severity::Error, e.source_id});
    }
    return issues;
}

ManifestBuilder::ManifestBuilder(std::string name)
{
    manifest_.name = std::move(name);
    manifest_.mode = LayoutMode::Directory;
}

ManifestBuilder& ManifestBuilder::add_file(fs::path const& source, std::vector<std::string> destination)
{
    std::error_code ec;
    fs::path const abs = fs::absolute(source, ec);
    if (ec) throw CoreError(ErrorCode::SourceMissing, "Cannot resolve source path: " + ec.message(), ec.value());

    std::optional<native::EntryInfo> info = native::stat_entry(abs);
    if (!info) throw CoreError(ErrorCode::SourceMissing, "Source file not found: " + to_utf8(abs.filename()));
    fs::path const resolved = resolve_selected(abs, *info);
    if (resolved != abs) info = native::stat_entry(resolved);
    if (!info || info->kind != native::EntryKind::Regular)
        throw CoreError(ErrorCode::SourceUnreadable, "Source is not a regular file: " + to_utf8(abs.filename()));

    manifest_.entries.push_back(make_entry(resolved, *info, std::move(destination), "added to collection"));
    return *this;
}

Manifest ManifestBuilder::build()
{
    assign_ids(manifest_);
    return std::move(manifest_);
}

} // namespace tc::core
