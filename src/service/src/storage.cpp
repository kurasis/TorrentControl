#include "tc/service/storage.hpp"

#include "tc/core/error.hpp"

#include <fstream>
#include <random>
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#endif

namespace tc::service {

namespace fs = std::filesystem;
using nlohmann::json;
using core::CoreError;
using core::ErrorCode;

void write_file_atomic(fs::path const& path, std::string_view bytes)
{
    std::random_device rd;
    fs::path const tmp = path.parent_path()
        / core::path_from_utf8("." + core::to_utf8(path.filename()) + ".tc-" + std::to_string(rd()) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw CoreError(ErrorCode::OutputWriteFailed, "Cannot write " + core::to_utf8(path));
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        bool const flushed = static_cast<bool>(out);
        out.close();
        if (!flushed || !out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw CoreError(ErrorCode::OutputWriteFailed, "Cannot write " + core::to_utf8(path));
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw CoreError(ErrorCode::OutputWriteFailed, "Cannot replace " + core::to_utf8(path));
    }
}

std::string read_small_file(fs::path const& path, std::size_t max_bytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw CoreError(ErrorCode::SourceMissing, "Cannot open " + core::to_utf8(path));
    std::string data;
    char buf[65536];
    while (in) {
        in.read(buf, sizeof buf);
        data.append(buf, static_cast<std::size_t>(in.gcount()));
        if (data.size() > max_bytes) throw CoreError(ErrorCode::ResourceLimit, "File is too large: " + core::to_utf8(path));
    }
    return data;
}

json project_json(Draft const& d, int resolved_piece_length)
{
    json draft = to_json(d);
    draft.erase("revision");
    draft.erase("effectiveName");
    return json{{"format", "torrentcontrol-project"}, {"version", project_format_version}, {"draft", std::move(draft)},
        {"pieceSizePolicy", {{"version", core::PieceSizeDecision::policy_version}, {"resolvedPieceLength", resolved_piece_length}}}};
}

void save_project(fs::path const& path, Draft const& d, int resolved_piece_length)
{
    auto bytes = project_json(d, resolved_piece_length).dump(2);
    if (bytes.size() > max_project_bytes) throw CoreError(ErrorCode::ResourceLimit, "Project exceeds the 64 MiB file limit");
    write_file_atomic(path, bytes);
}

Draft load_project(fs::path const& path)
{
    json const j = json::parse(read_small_file(path, max_project_bytes), nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("format", "") != "torrentcontrol-project")
        throw CoreError(ErrorCode::UnsupportedFormat, "Not a TorrentControl project file");
    if (!j.contains("version") || !j.at("version").is_number_integer()
        || j.at("version") < 1 || j.at("version") > project_format_version)
        throw CoreError(ErrorCode::UnsupportedFormat, "The project was saved by a newer version of TorrentControl");
    if (!j.contains("draft")) throw CoreError(ErrorCode::UnsupportedFormat, "The project has no draft");
    Draft draft = draft_from_json(j.at("draft"));
    if (auto policy = j.find("pieceSizePolicy"); policy != j.end()) {
        if (!policy->is_object() || !policy->contains("version") || !policy->at("version").is_number_integer()
            || policy->at("version") != core::PieceSizeDecision::policy_version)
            throw CoreError(ErrorCode::UnsupportedFormat, "Unsupported project piece-size policy");
        auto resolved = policy->find("resolvedPieceLength");
        if (resolved == policy->end() || !resolved->is_number_integer() || *resolved < 0 || *resolved > 128 * 1024 * 1024)
            throw CoreError(ErrorCode::UnsupportedFormat, "Invalid resolved project piece size");
        int const length = resolved->get<int>();
        if (length != 0 && (length < 16 * 1024 || (length & (length - 1)) != 0))
            throw CoreError(ErrorCode::UnsupportedFormat, "Invalid resolved project piece size");
        if (draft.piece_length != 0 && length != draft.piece_length)
            throw CoreError(ErrorCode::UnsupportedFormat, "Project piece size disagrees with its saved policy");
        // Restore the actual decision as a fixed size. Selecting Automatic in
        // the existing piece-size control explicitly requests a fresh decision.
        if (draft.piece_length == 0) draft.piece_length = length;
    }
    return draft;
}

namespace {

constexpr char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(std::string_view in)
{
    std::string out;
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        auto const n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8)
            | static_cast<unsigned char>(in[i + 2]);
        out += {b64[(n >> 18) & 63], b64[(n >> 12) & 63], b64[(n >> 6) & 63], b64[n & 63]};
        i += 3;
    }
    if (i + 1 == in.size()) {
        auto const n = static_cast<unsigned char>(in[i]) << 16;
        out += {b64[(n >> 18) & 63], b64[(n >> 12) & 63], '=', '='};
    } else if (i + 2 == in.size()) {
        auto const n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += {b64[(n >> 18) & 63], b64[(n >> 12) & 63], b64[(n >> 6) & 63], '='};
    }
    return out;
}

std::string unbase64(std::string_view in)
{
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        char const* p = std::char_traits<char>::find(b64, 64, c);
        if (p == nullptr) throw CoreError(ErrorCode::InvalidArgument, "Corrupt protected value");
        buf = (buf << 6) | static_cast<unsigned>(p - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace

std::string protect_secret(std::string_view plaintext)
{
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(plaintext.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"TorrentControl", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw CoreError(ErrorCode::OutputWriteFailed, "Cannot protect a secret", static_cast<int>(GetLastError()));
    std::string result = "dpapi:" + base64(std::string_view(reinterpret_cast<char const*>(out.pbData), out.cbData));
    LocalFree(out.pbData);
    return result;
#else
    return "plain:" + base64(plaintext);
#endif
}

std::string unprotect_secret(std::string_view stored)
{
#ifdef _WIN32
    if (stored.starts_with("dpapi:")) {
        std::string const raw = unbase64(stored.substr(6));
        DATA_BLOB in{static_cast<DWORD>(raw.size()), reinterpret_cast<BYTE*>(const_cast<char*>(raw.data()))};
        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
            throw CoreError(ErrorCode::SourceUnreadable, "Cannot read a protected secret", static_cast<int>(GetLastError()));
        std::string result(reinterpret_cast<char const*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
        return result;
    }
#endif
    if (stored.starts_with("plain:")) return unbase64(stored.substr(6));
    throw CoreError(ErrorCode::InvalidArgument, "Unknown protected value format");
}

json to_json(AppSettings const& s, bool include_profiles)
{
    json profiles = json::array();
    if (include_profiles) for (auto const& p : s.custom_profiles) profiles.push_back(to_json(p));
    json value{{"theme", s.theme}, {"language", s.language}, {"mode", s.mode}, {"lastProfile", s.last_profile},
        {"maxConcurrentJobs", s.max_concurrent_jobs}, {"openClientWithoutAsking", s.open_client_without_asking},
        {"customProfiles", std::move(profiles)}};
    if (!include_profiles) value.erase("customProfiles");
    return value;
}

void apply_settings_patch(AppSettings& s, json const& patch)
{
    if (!patch.is_object()) throw CoreError(ErrorCode::InvalidArgument, "settings must be an object");
    AppSettings next = s;
    for (auto const& [key, v] : patch.items()) {
        auto str = [&](std::set<std::string> const& allowed) {
            if (!v.is_string() || !allowed.contains(v.get<std::string>()))
                throw CoreError(ErrorCode::InvalidArgument, "invalid value for " + key);
            return v.get<std::string>();
        };
        if (key == "theme") next.theme = str({"system", "light", "dark"});
        else if (key == "language") next.language = str({"", "en", "ru"});
        else if (key == "mode") next.mode = str({"simple", "advanced"});
        else if (key == "lastProfile") {
            if (!v.is_string() || v.get<std::string>().size() > 64) throw CoreError(ErrorCode::InvalidArgument, "invalid profile");
            next.last_profile = v.get<std::string>();
        } else if (key == "maxConcurrentJobs") {
            if (!v.is_number_integer() || v.get<int>() < 1 || v.get<int>() > 8)
                throw CoreError(ErrorCode::InvalidArgument, "maxConcurrentJobs must be 1 to 8");
            next.max_concurrent_jobs = v.get<int>();
        } else if (key == "openClientWithoutAsking") {
            if (!v.is_boolean()) throw CoreError(ErrorCode::InvalidArgument, "invalid openClientWithoutAsking");
            next.open_client_without_asking = v.get<bool>();
        }
    }
    s = std::move(next);
}

void save_settings(fs::path const& path, AppSettings const& s)
{
    json j = to_json(s);
    j["version"] = 1;
    for (auto& p : j["customProfiles"]) {
        for (auto& t : p["trackers"]) {
            std::string const url = t["url"].get<std::string>();
            if (looks_secret(url)) {
                t.erase("url");
                t["urlProtected"] = protect_secret(url);
            }
        }
    }
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    if (ec) throw CoreError(ErrorCode::OutputWriteFailed, "Cannot create the settings folder", ec.value());
    auto bytes = j.dump(2);
    if (bytes.size() > max_settings_bytes) throw CoreError(ErrorCode::ResourceLimit, "Settings exceed the 16 MiB file limit");
    write_file_atomic(path, bytes);
}

AppSettings load_settings(fs::path const& path)
{
    AppSettings s;
    std::error_code ec;
    if (!fs::exists(path, ec)) return s;
    json j = json::parse(read_small_file(path, max_settings_bytes), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return s; // a damaged file falls back to defaults
    json patch = j;
    patch.erase("customProfiles");
    patch.erase("version");
    try {
        apply_settings_patch(s, patch);
    } catch (CoreError const&) {
        s = AppSettings{};
    }
    if (auto it = j.find("customProfiles"); it != j.end() && it->is_array()) {
        for (auto p : *it) {
            try {
                if (p.contains("trackers") && p["trackers"].is_array())
                    for (auto& t : p["trackers"])
                        if (t.contains("urlProtected")) t["url"] = unprotect_secret(t["urlProtected"].get<std::string>());
                Profile prof = profile_from_json(p);
                prof.builtin = false;
                s.custom_profiles.push_back(std::move(prof));
            } catch (std::exception const&) {
                // Skip an unreadable profile rather than losing all settings.
            }
        }
    }
    return s;
}

json export_profile(Profile const& p, bool include_secrets)
{
    json j = to_json(p);
    j.erase("builtin");
    if (!include_secrets)
        for (auto& t : j["trackers"]) t["url"] = redact_url(t["url"].get<std::string>());
    return j;
}

} // namespace tc::service
