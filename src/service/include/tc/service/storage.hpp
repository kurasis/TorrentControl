#pragma once

// Local persistence (specification section 14): project files, application
// settings and protected secrets.

#include "tc/service/draft.hpp"
#include "tc/service/profiles.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <cstddef>
#include <string>
#include <vector>

namespace tc::service {

// Writes `bytes` to `path` through a temporary file in the same folder and a
// rename that replaces the previous file.
void write_file_atomic(std::filesystem::path const& path, std::string_view bytes);
std::string read_small_file(std::filesystem::path const& path, std::size_t max_bytes = 16 * 1024 * 1024);

// ---- Project files (.tcproject) -------------------------------------------
//
// Versioned JSON with the draft (configuration and source mappings), the
// profile reference and the resolved piece-size policy. Never a .torrent and
// never part of the payload (the default exclusions skip *.tcproject).

inline constexpr int project_format_version = 1;
// Large source mappings need more room than ordinary settings; read and write
// use the same cap so an oversized save never creates an unreadable project.
inline constexpr std::size_t max_project_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t max_settings_bytes = 16 * 1024 * 1024;

nlohmann::json project_json(Draft const& d, int resolved_piece_length);
void save_project(std::filesystem::path const& path, Draft const& d, int resolved_piece_length);
// Throws CoreError(UnsupportedFormat) for a newer or foreign file.
Draft load_project(std::filesystem::path const& path);

// ---- Secrets ---------------------------------------------------------------
//
// On Windows secrets are protected with DPAPI for the current user. Other
// platforms (headless tests) store them with a "plain:" marker.
std::string protect_secret(std::string_view plaintext);
std::string unprotect_secret(std::string_view stored);

// ---- Settings ----------------------------------------------------------------

struct AppSettings {
    std::string theme = "system";   // system | light | dark
    std::string language;           // "" follows the system
    std::string mode = "simple";    // simple | advanced
    std::string last_profile = "public";
    int max_concurrent_jobs = 1;
    // Opt-in: offer "Open in default torrent client" without asking each time.
    bool open_client_without_asking = false;
    std::vector<Profile> custom_profiles;
};

nlohmann::json to_json(AppSettings const& s, bool profiles = true);
// Applies known keys; unknown keys are ignored. Throws CoreError on bad values.
void apply_settings_patch(AppSettings& s, nlohmann::json const& patch);

// Tracker URLs of custom profiles are stored protected when they look like
// they carry a credential.
void save_settings(std::filesystem::path const& path, AppSettings const& s);
AppSettings load_settings(std::filesystem::path const& path);

// A profile for sharing: credentials are redacted unless explicitly included.
nlohmann::json export_profile(Profile const& p, bool include_secrets);

} // namespace tc::service
