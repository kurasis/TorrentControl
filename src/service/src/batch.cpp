#include "tc/service/batch.hpp"

#include "tc/core/error.hpp"
#include "tc/core/manifest.hpp"
#include "tc/service/sources.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace tc::service {

namespace fs = std::filesystem;

std::optional<BatchMode> batch_mode_from(std::string_view s) noexcept
{
    if (s == "single") return BatchMode::Single;
    if (s == "perFile") return BatchMode::PerFile;
    if (s == "perChildFolder") return BatchMode::PerChildFolder;
    return std::nullopt;
}

std::string_view to_string(BatchMode m) noexcept
{
    switch (m) {
    case BatchMode::Single: return "single";
    case BatchMode::PerFile: return "perFile";
    case BatchMode::PerChildFolder: return "perChildFolder";
    }
    return "single";
}

std::optional<ConflictPolicy> conflict_policy_from(std::string_view s) noexcept
{
    if (s == "skip") return ConflictPolicy::Skip;
    if (s == "replace") return ConflictPolicy::Replace;
    if (s == "rename") return ConflictPolicy::Rename;
    return std::nullopt;
}

std::string_view to_string(ConflictPolicy p) noexcept
{
    switch (p) {
    case ConflictPolicy::Skip: return "skip";
    case ConflictPolicy::Replace: return "replace";
    case ConflictPolicy::Rename: return "rename";
    }
    return "rename";
}

namespace {

// Immediate children of a folder, sorted, without following links.
std::vector<core::native::EntryInfo> children(fs::path const& dir)
{
    auto list = core::native::list_directory(dir);
    std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.path < b.path; });
    return list;
}

std::string key_of(fs::path const& p)
{
    return core::fold_case(core::to_utf8(fs::absolute(p).lexically_normal()));
}

} // namespace

BatchPlan plan_batch(Draft const& draft, BatchMode mode, fs::path const& output_dir, ConflictPolicy policy)
{
    if (draft.sources.empty()) throw core::CoreError(core::ErrorCode::EmptyPayload, "No sources are selected");
    BatchPlan plan;
    plan.mode = mode;
    plan.format = draft.format;
    plan.output_dir = output_dir;
    int counter = 0;

    auto add = [&](std::string name, std::vector<SourceSpec> sources) {
        BatchItem item;
        item.id = "item-" + std::to_string(++counter);
        item.name = std::move(name);
        item.sources = std::move(sources);
        item.planned_output = output_dir / core::path_from_utf8(safe_file_name(item.name) + ".torrent");
        item.output = item.planned_output;
        item.policy = policy;
        plan.items.push_back(std::move(item));
    };

    switch (mode) {
    case BatchMode::Single: add(effective_name(draft).empty() ? "Collection" : effective_name(draft), draft.sources); break;
    case BatchMode::PerFile:
        for (auto const& s : draft.sources) {
            if (!s.is_directory) {
                add(core::to_utf8(s.path.filename()), {s});
                continue;
            }
            std::size_t folders = 0;
            for (auto const& child : children(s.path)) {
                if (child.kind == core::native::EntryKind::Regular) {
                    SourceSpec spec = s;
                    spec.id = s.id + "-" + std::to_string(counter + 1);
                    spec.path = child.path;
                    spec.is_directory = false;
                    add(core::to_utf8(child.path.filename()), {spec});
                } else if (child.kind == core::native::EntryKind::Directory) {
                    ++folders;
                }
            }
            if (folders > 0)
                plan.notes.push_back(std::to_string(folders) + " subfolder(s) of " + core::to_utf8(s.path.filename())
                    + " are not included: per-file mode uses only the files directly inside a selected folder.");
        }
        break;
    case BatchMode::PerChildFolder:
        for (auto const& s : draft.sources) {
            if (!s.is_directory) {
                plan.notes.push_back(core::to_utf8(s.path.filename())
                    + " is a file, not a folder; per-folder mode does not include it.");
                continue;
            }
            std::size_t files = 0;
            for (auto const& child : children(s.path)) {
                if (child.kind == core::native::EntryKind::Directory) {
                    SourceSpec spec = s; // recursion follows the folder's own setting
                    spec.id = s.id + "-" + std::to_string(counter + 1);
                    spec.path = child.path;
                    add(core::to_utf8(child.path.filename()), {spec});
                } else if (child.kind == core::native::EntryKind::Regular) {
                    ++files;
                }
            }
            if (files > 0)
                plan.notes.push_back(std::to_string(files) + " file(s) directly inside " + core::to_utf8(s.path.filename())
                    + " are not part of any per-folder torrent.");
        }
        break;
    }
    if (plan.items.empty()) throw core::CoreError(core::ErrorCode::EmptyPayload, "The batch would create no torrents");
    resolve_conflicts(plan);
    return plan;
}

void resolve_conflicts(BatchPlan& plan)
{
    std::set<std::string> used;
    std::map<std::string, int> planned_count;
    for (auto const& item : plan.items) ++planned_count[key_of(item.planned_output)];

    for (auto& item : plan.items) {
        std::error_code ec;
        item.exists_on_disk = fs::exists(item.planned_output, ec);
        item.duplicate_in_batch = planned_count[key_of(item.planned_output)] > 1;
        item.output = item.planned_output;
        item.included = true;

        bool const taken = used.contains(key_of(item.output));
        if (!item.exists_on_disk && !taken) {
            used.insert(key_of(item.output));
            continue;
        }
        if (item.policy == ConflictPolicy::Skip) {
            item.included = false;
            continue;
        }
        if (item.policy == ConflictPolicy::Replace && !taken) {
            used.insert(key_of(item.output));
            continue;
        }
        // Rename, or a duplicate inside the batch under Replace.
        std::string const stem = safe_file_name(item.name);
        for (int n = 2;; ++n) {
            fs::path const candidate = plan.output_dir / core::path_from_utf8(stem + " (" + std::to_string(n) + ").torrent");
            if (!used.contains(key_of(candidate)) && !fs::exists(candidate, ec)) {
                item.output = candidate;
                break;
            }
        }
        used.insert(key_of(item.output));
    }
}

nlohmann::json to_json(BatchPlan const& plan)
{
    nlohmann::json items = nlohmann::json::array();
    for (auto const& i : plan.items) {
        nlohmann::json roots = nlohmann::json::array();
        for (auto const& s : i.sources) roots.push_back(core::to_utf8(s.path));
        items.push_back({{"id", i.id}, {"name", i.name}, {"sourceRoots", std::move(roots)},
            {"plannedOutput", core::to_utf8(i.planned_output)}, {"output", core::to_utf8(i.output)},
            {"existsOnDisk", i.exists_on_disk}, {"duplicateInBatch", i.duplicate_in_batch},
            {"policy", std::string(to_string(i.policy))}, {"included", i.included}});
    }
    return {{"mode", std::string(to_string(plan.mode))}, {"format", std::string(core::to_string(plan.format))},
        {"outputDir", core::to_utf8(plan.output_dir)}, {"items", std::move(items)}, {"notes", plan.notes}};
}

} // namespace tc::service
