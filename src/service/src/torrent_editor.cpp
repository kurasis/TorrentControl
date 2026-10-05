#include "tc/service/app_service.hpp"
#include "tc/core/error.hpp"
#include "tc/core/output.hpp"

namespace tc::service {
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
json hashes(core::InfoHashes const& h)
{
    json j = json::object();
    if (h.v1) j["v1"] = core::to_hex(*h.v1);
    if (h.v2) j["v2"] = core::to_hex(*h.v2);
    return j;
}
}

std::shared_ptr<core::Metainfo const> AppService::torrent_metainfo(std::string const& id) const
{
    std::lock_guard lock(mutex_);
    auto it = torrents_.find(id);
    if (it == torrents_.end()) throw ServiceError("NOT_FOUND", "No such torrent");
    return it->second.meta;
}

json AppService::preview_torrent_edit(std::string const& id, core::OuterEdit const& outer, core::InfoEdit const& info, bool remove_signatures, json const& display_changes)
{
    auto meta = torrent_metainfo(id);
    auto candidate = std::make_shared<core::MetadataPreview const>(core::preview_metadata_edit(*meta, outer, info, remove_signatures));
    std::lock_guard lock(mutex_);
    edits_.clear(); // bound memory to one candidate and invalidate older tokens
    auto token = "edit-" + std::to_string(next_edit_++);
    json summary{{"token", token}, {"torrentId", id}, {"infoChanged", candidate->info_changed},
        {"rawInfoPreserved", !candidate->info_changed}, {"signaturesRemoved", candidate->removed_signatures},
        {"oldHashes", hashes(candidate->old_hashes)}, {"newHashes", hashes(candidate->new_hashes)},
        {"metainfoBytes", std::to_string(candidate->bytes.size())}, {"outputChosen", false}, {"requiresReplace", false},
        {"changes", display_changes}};
    edits_.emplace(token, PendingEdit{id, std::move(candidate), summary, {}});
    return summary;
}

json AppService::editor_preview(std::string const& token) const
{
    std::lock_guard lock(mutex_);
    auto it = edits_.find(token);
    if (it == edits_.end()) throw ServiceError("STALE_PREVIEW", "Prepare a fresh preview before saving");
    return it->second.summary;
}

json AppService::choose_editor_output(std::string const& token, fs::path const& path)
{
    std::lock_guard lock(mutex_);
    auto it = edits_.find(token);
    if (it == edits_.end()) throw ServiceError("STALE_PREVIEW", "Prepare a fresh preview before saving");
    it->second.output = fs::absolute(path).lexically_normal();
    it->second.summary["output"] = core::to_utf8(it->second.output);
    it->second.summary["outputChosen"] = true;
    it->second.summary["requiresReplace"] = fs::exists(it->second.output);
    return it->second.summary;
}

json AppService::save_torrent_edit(std::string const& token, bool replace_existing)
{
    PendingEdit edit;
    OpenedTorrent original;
    {
        std::lock_guard lock(mutex_);
        auto it = edits_.find(token);
        if (it == edits_.end()) throw ServiceError("STALE_PREVIEW", "Prepare a fresh preview before saving");
        edit = it->second;
        original = torrents_.at(edit.torrent_id);
    }
    if (edit.output.empty()) throw ServiceError("NO_OUTPUT", "Select the destination in the Save As dialog");
    if (read_small_file(original.path, 64u * 1024 * 1024) != original.meta->bytes())
        throw core::CoreError(core::ErrorCode::SourceChanged, "The opened torrent changed on disk; reopen it before saving");
    core::CommitOptions options;
    options.replace_existing = replace_existing;
    if (!edit.candidate->info_changed) options.preserved_info = original.meta.get();
    auto committed = core::commit_output(edit.output, edit.candidate->bytes, options);
    auto reopened = core::Metainfo::parse(read_small_file(committed.path, 64u * 1024 * 1024));
    if (reopened.bytes() != edit.candidate->bytes)
        throw core::CoreError(core::ErrorCode::SourceChanged, "The saved torrent changed before it could be reopened");
    auto summary = open_torrent(committed.path);
    {
        std::lock_guard lock(mutex_);
        edits_.erase(token);
    }
    return {{"torrent", std::move(summary)}, {"guaranteeNote", committed.guarantee_note}};
}
} // namespace tc::service
