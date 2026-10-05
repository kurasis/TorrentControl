#pragma once

// Creation profiles (specification section 4.2) and the built-in tracker
// seed list (section 10.1).

#include "tc/service/draft.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace tc::service {

// The seed list from the specification, consulted upstream on 2026-10-03. No
// local check result is implied: every entry starts Unchecked.
struct TrackerCatalog {
    std::string source_url;
    std::string source_date;
    std::vector<std::string> urls;
};

TrackerCatalog const& builtin_tracker_catalog();

struct Profile {
    std::string id;
    std::string name;
    bool builtin = false;
    core::TorrentFormat format = core::TorrentFormat::Hybrid;
    bool private_flag = false;
    // Public preset: one tier per catalog endpoint.
    bool use_public_trackers = false;
    // Trackers the profile itself defines (custom profiles).
    std::vector<TrackerRow> trackers = {};
    // False: the profile removes web seeds (private profile, until allowed).
    bool allow_web_seeds = true;
    bool clear_trackers = false;
    std::string source_tag = {};
};

std::vector<Profile> builtin_profiles();

// One visible consequence of applying a profile.
struct FieldChange {
    std::string field;
    json before;
    json after;
    // The change removes something the user typed (a custom tracker URL, a
    // passkey, a web seed); the UI must list it before the user applies it.
    bool removes_user_value = false;
};

struct ProfilePlan {
    std::vector<FieldChange> changes;
    Draft result;
};

// Computes the draft after switching to `p` without changing `current`.
// Profiles never touch sources, name, comment, output or creator. A private
// profile keeps every tracker URL the user configured; only public preset
// endpoints are removed.
ProfilePlan plan_profile(Draft const& current, Profile const& p);

json to_json(Profile const& p);
Profile profile_from_json(json const& j);

// Masks credentials in a URL for logs and exports: user info, query values
// and passkey-like path segments become "***". Scheme, host, port and the
// shape of the path stay visible.
std::string redact_url(std::string_view url);

// True when a URL carries something that looks like a credential.
bool looks_secret(std::string_view url);

} // namespace tc::service
