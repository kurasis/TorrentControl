#pragma once

// Batch creation planning (specification section 4.3).

#include "tc/service/draft.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tc::service {

enum class BatchMode {
    // One torrent for the complete selection.
    Single,
    // One torrent per selected file; for a selected folder, one per file
    // directly inside it.
    PerFile,
    // One torrent per immediate child folder of each selected folder.
    PerChildFolder,
};

enum class ConflictPolicy { Skip, Replace, Rename };

std::optional<BatchMode> batch_mode_from(std::string_view s) noexcept;
std::string_view to_string(BatchMode m) noexcept;
std::optional<ConflictPolicy> conflict_policy_from(std::string_view s) noexcept;
std::string_view to_string(ConflictPolicy p) noexcept;

struct BatchItem {
    std::string id;
    std::string name;
    std::vector<SourceSpec> sources;
    // As planned before conflict resolution.
    std::filesystem::path planned_output;
    // After the policy is applied (renamed path for Rename).
    std::filesystem::path output;
    bool exists_on_disk = false;
    bool duplicate_in_batch = false;
    ConflictPolicy policy = ConflictPolicy::Rename;
    // False when the policy is Skip and the item conflicts.
    bool included = true;
};

struct BatchPlan {
    BatchMode mode = BatchMode::Single;
    core::TorrentFormat format = core::TorrentFormat::Hybrid;
    std::filesystem::path output_dir;
    std::vector<BatchItem> items;
    // Things the preview must tell the user, e.g. files not covered by any
    // per-folder torrent.
    std::vector<std::string> notes;
};

// Plans the items for `draft`'s sources. Every item uses the draft's format
// and settings (snapshotted when queued).
BatchPlan plan_batch(Draft const& draft, BatchMode mode, std::filesystem::path const& output_dir, ConflictPolicy policy);

// Applies each item's policy: Skip excludes conflicting items, Replace keeps
// the path (replacing an existing file only), Rename picks "name (2)" and so
// on. Two items never write the same file; a duplicate inside the batch is
// renamed even under Replace.
void resolve_conflicts(BatchPlan& plan);

nlohmann::json to_json(BatchPlan const& plan);

} // namespace tc::service
