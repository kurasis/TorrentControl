#pragma once

// The editable creation draft (specification sections 4 and 14).
//
// A draft holds every setting of the creation workflow, including values that
// the simple mode hides, so switching modes never discards anything. The
// native service owns it; the frontend edits it with revisioned patches.

#include "tc/core/manifest.hpp"
#include "tc/core/torrent_engine.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tc::service {

using nlohmann::json;

// One selected file or folder. The ID is native-owned; the page never sends
// paths for privileged operations.
struct SourceSpec {
    std::string id;
    std::filesystem::path path;
    bool is_directory = false;
    bool recursive = true;
    std::vector<std::string> exclusions = {};
    bool follow_links = false;
    bool skip_cloud = false;
};

struct TrackerRow {
    std::string url;
    // Zero-based tier; rows with the same tier form one announce-list tier.
    int tier = 0;
    bool enabled = true;
};

enum class CreationDatePolicy { Now, Omit, Fixed };

std::string_view to_string(CreationDatePolicy p) noexcept;
std::optional<CreationDatePolicy> creation_date_policy_from(std::string_view s) noexcept;
std::optional<core::TorrentFormat> format_from(std::string_view s) noexcept;

struct Draft {
    std::uint64_t revision = 1;
    std::vector<SourceSpec> sources;
    // Empty: derived from the single selected source.
    std::string name;
    std::string profile_id = "public";
    core::TorrentFormat format = core::TorrentFormat::Hybrid;
    // 0 selects the automatic policy.
    int piece_length = 0;
    bool private_flag = false;
    std::vector<TrackerRow> trackers;
    std::vector<std::string> web_seeds;
    std::vector<std::pair<std::string, int>> dht_nodes;
    std::string comment;
    std::string creator;
    // info.source; empty omits it.
    std::string source_tag;
    CreationDatePolicy date_policy = CreationDatePolicy::Now;
    std::int64_t fixed_date = 0;
    // Chosen through a native dialog; empty until chosen or defaulted.
    std::filesystem::path output;
    bool replace_existing = false;
    bool allow_hydration = false;
    bool accept_large_resource_use = false;
};

// Tracker tiers in announce-list order, enabled rows only.
std::vector<std::vector<std::string>> tracker_tiers(std::vector<TrackerRow> const& rows);

// Engine options for a draft. `now` is the creation date for the Now policy.
core::CreateOptions create_options(Draft const& d, std::int64_t now);

// Name the torrent will get: the explicit name, or the single source's name.
std::string effective_name(Draft const& d);

// JSON used by the bridge and by project files. Paths are UTF-8 strings.
json to_json(SourceSpec const& s);
json to_json(Draft const& d, bool collections = true);

// Applies the editable fields present in `patch` (unknown keys are rejected).
// Sources and the output path are not editable through a patch. Throws
// core::CoreError(InvalidArgument) on a malformed value.
void apply_patch(Draft& d, json const& patch);

// Full restore from to_json(), used for project files.
Draft draft_from_json(json const& j);

} // namespace tc::service
