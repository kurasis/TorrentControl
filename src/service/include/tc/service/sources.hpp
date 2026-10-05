#pragma once

// Builds the frozen manifest for a set of selected sources (section 5.1).

#include "tc/core/manifest.hpp"
#include "tc/service/draft.hpp"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace tc::service {

core::ScanOptions scan_options(SourceSpec const& s, std::vector<std::filesystem::path> const& excluded_paths);

// One source: the scan of that file or folder, renamed to `name` when given.
// Several sources: a collection named `name` (or "Collection") whose
// top-level entries are the selected files and folders, each under its own
// name. Nothing is copied; source IDs stay native-owned.
core::Manifest build_manifest(std::vector<SourceSpec> const& sources, std::string const& name,
    std::vector<std::filesystem::path> const& excluded_paths = {}, std::stop_token stop = {});

// Default output path: "<name>.torrent" beside the first source.
std::filesystem::path default_output(std::vector<SourceSpec> const& sources, std::string const& name);

// Replaces characters Windows does not allow in a file name.
std::string safe_file_name(std::string_view name);

} // namespace tc::service
