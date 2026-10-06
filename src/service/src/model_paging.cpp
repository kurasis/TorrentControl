#include "tc/service/app_service.hpp"
#include "tc/core/error.hpp"
#include "view_paging.hpp"
#include <algorithm>
#include <charconv>

namespace tc::service {
using nlohmann::json;
namespace {
void current(std::string const& actual, std::string const& requested)
{
    if (requested != actual) throw ServiceError("STALE_REVISION", "This collection changed. Reload it before continuing.", true);
}
[[noreturn]] void missing() { throw ServiceError("NOT_FOUND", "No such collection or row"); }
}

json AppService::model_page(std::string const& model, std::string const& key, std::string const& owner,
    std::size_t offset, std::size_t limit, std::string const& revision) const
{
    std::lock_guard lock(mutex_);
    return model_page_locked(model, key, owner, offset, limit, revision);
}

json AppService::model_page_locked(std::string const& model, std::string const& key, std::string const& owner,
    std::size_t offset, std::size_t limit, std::string const& revision) const
{
    if (limit == 0) throw ServiceError("INVALID_ARGUMENT", "Page size must be positive");
    if (model == "draft") {
        current(std::to_string(draft_.revision), revision);
        if (key == "sources") return view::page(draft_.sources.size(), offset, limit, revision,
            [&](auto i) { return view::source(draft_.sources[i]); });
        if (key == "source") {
            auto row = std::find_if(draft_.sources.begin(), draft_.sources.end(), [&](auto const& s) { return s.id == owner; });
            if (row == draft_.sources.end()) missing();
            return view::page(1, offset, limit, revision, [&](auto) { return view::source(*row); });
        }
        if (key == "trackers") return view::page(draft_.trackers.size(), offset, limit, revision, [&](auto i) {
            auto const& row = draft_.trackers[i]; return json{{"url", row.url}, {"tier", row.tier}, {"enabled", row.enabled}};
        });
        if (key == "webSeeds") return view::page(draft_.web_seeds.size(), offset, limit, revision,
            [&](auto i) { return json(draft_.web_seeds[i]); });
        if (key == "exclusions") {
            auto s = std::find_if(draft_.sources.begin(), draft_.sources.end(), [&](auto const& row) { return row.id == owner; });
            if (s == draft_.sources.end()) missing();
            return view::page(s->exclusions.size(), offset, limit, revision, [&](auto i) { return json(s->exclusions[i]); });
        }
    } else if (model == "profiles") {
        current(std::to_string(profiles_revision_), revision);
        if (key == "items") {
            auto const builtin = builtin_profiles();
            return view::page(builtin.size() + settings_.custom_profiles.size(), offset, limit, revision,
                [&](auto i) { return view::profile(i < builtin.size() ? builtin[i] : settings_.custom_profiles[i - builtin.size()]); });
        }
    } else if (model == "batch") {
        current(std::to_string(batch_revision_), revision);
        if (!batch_) missing();
        if (key == "items") return view::page(batch_->items.size(), offset, limit, revision, [&](auto i) {
            auto const& row = batch_->items[i];
            return json{{"id", row.id}, {"name", view::preview(row.name)}, {"output", core::to_utf8(row.output)},
                {"plannedOutput", core::to_utf8(row.planned_output)}, {"sourceRootsTotal", row.sources.size()},
                {"existsOnDisk", row.exists_on_disk}, {"duplicateInBatch", row.duplicate_in_batch},
                {"policy", std::string(to_string(row.policy))}, {"included", row.included}};
        });
        if (key == "notes") return view::page(batch_->notes.size(), offset, limit, revision,
            [&](auto i) { return json(view::preview(batch_->notes[i], 4096)); });
        if (key == "sourceRoots") {
            auto item = std::find_if(batch_->items.begin(), batch_->items.end(), [&](auto const& row) { return row.id == owner; });
            if (item == batch_->items.end()) missing();
            return view::page(item->sources.size(), offset, limit, revision,
                [&](auto i) { return json(core::to_utf8(item->sources[i].path)); });
        }
    } else if (model == "batchJobs") {
        auto found = batch_job_ids_.find(owner);
        if (found == batch_job_ids_.end() || key != "items") missing();
        return view::page(found->second.size(), offset, limit, owner, [&](auto i) { return json(found->second[i]); });
    } else if (model == "torrent") {
        auto found = torrents_.find(owner);
        if (found == torrents_.end()) missing();
        auto const& value = found->second.overview;
        auto rows = value.find(key);
        if (rows != value.end() && rows->is_array()) return view::page(rows->size(), offset, limit, owner,
            [&](auto i) { return view::compact((*rows)[i]); });
    } else if (model == "profilePlan") {
        current(std::to_string(draft_.revision), revision);
        auto p = find_profile_locked(owner);
        if (!p) missing();
        auto plan = service::plan_profile(draft_, *p);
        for (auto const& change : plan.changes) for (auto const& side : {"before", "after"}) {
            if (key != change.field + "." + side) continue;
            auto const& value = std::string_view(side) == "before" ? change.before : change.after;
            if (!value.is_array()) missing();
            return view::page(value.size(), offset, limit, revision, [&](auto i) {
                return value[i].dump().size() <= 24 * 1024 ? value[i] : view::compact(value[i]);
            });
        }
    } else if (model == "review") {
        current(std::to_string(draft_.revision), revision);
        auto full = validate_draft_locked();
        auto rows = full.find(key);
        if (rows != full.end() && rows->is_array()) return view::page(rows->size(), offset, limit, revision,
            [&](auto i) { return view::compact((*rows)[i]); });
    }
    missing();
}

json AppService::batch_json_locked() const
{
    if (!batch_) return nullptr;
    auto revision = std::to_string(batch_revision_);
    auto page = model_page_locked("batch", "items", "", 0, 50, revision);
    auto notes = model_page_locked("batch", "notes", "", 0, 50, revision);
    auto included = std::count_if(batch_->items.begin(), batch_->items.end(), [](auto const& row) { return row.included; });
    return {{"mode", std::string(to_string(batch_->mode))}, {"format", std::string(core::to_string(batch_->format))},
        {"outputDir", core::to_utf8(batch_->output_dir)}, {"items", std::move(page["items"])},
        {"total", batch_->items.size()}, {"includedTotal", included}, {"revision", revision},
        {"nextOffset", page["nextOffset"]}, {"notes", std::move(notes["items"])}, {"notesTotal", notes["total"]}};
}

json AppService::edit_draft_row(std::string const& key, std::string const& owner, std::size_t index,
    std::string const& action, json const& value, std::optional<std::uint64_t> revision)
{
    std::lock_guard lock(mutex_);
    check_revision(revision);
    if (action != "set" && action != "append" && action != "remove" && action != "up" && action != "down")
        throw ServiceError("INVALID_ARGUMENT", "Unknown row action");
    // Validate a single replacement with the same validators used by full draft patches.
    Draft validated;
    if (key == "trackers" || key == "webSeeds") {
        if (action == "set" || action == "append") apply_patch(validated, {{key, json::array({value})}});
    } else if (key != "exclusions") missing();
    auto edit = [&](auto& rows, auto replacement, std::size_t maximum) {
        if (action == "append") {
            if (rows.size() >= maximum) throw ServiceError("RESOURCE_LIMIT", "The collection is full");
            rows.push_back(std::move(replacement));
        } else {
            if (index >= rows.size()) missing();
            if (action == "set") rows[index] = std::move(replacement);
            else if (action == "remove") rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(index));
            else if (action == "up" && index > 0) std::swap(rows[index], rows[index - 1]);
            else if (action == "down" && index + 1 < rows.size()) std::swap(rows[index], rows[index + 1]);
        }
    };
    if (key == "trackers") edit(draft_.trackers, validated.trackers.empty() ? TrackerRow{} : validated.trackers.front(), 1000);
    else if (key == "webSeeds") edit(draft_.web_seeds, validated.web_seeds.empty() ? std::string{} : validated.web_seeds.front(), 1000);
    else {
        auto source = std::find_if(draft_.sources.begin(), draft_.sources.end(), [&](auto const& s) { return s.id == owner; });
        if (source == draft_.sources.end()) missing();
        std::string replacement;
        if (action == "set" || action == "append") {
            if (!value.is_string()) throw ServiceError("INVALID_ARGUMENT", "A pattern must be text");
            replacement = value.get<std::string>();
            if (replacement.empty() || replacement.size() > 4096) throw ServiceError("INVALID_ARGUMENT", "Invalid exclusion pattern");
        }
        edit(source->exclusions, std::move(replacement), 1000);
    }
    bump_locked(key == "exclusions");
    return draft_json_locked();
}

json AppService::model_text(std::string const& model, std::string const& key, std::string const& owner,
    std::size_t offset, std::string const& revision) const
{
    std::lock_guard lock(mutex_);
    std::string temporary;
    json held;
    std::string_view value;
    auto text_field = [&](json const& object) -> std::string_view {
        try {
            auto const& field = key.starts_with('/') ? object.at(json::json_pointer(key)) : object.at(key);
            if (!field.is_string()) missing();
            return field.get_ref<std::string const&>();
        } catch (json::exception const&) { missing(); }
    };
    if (model == "torrent") {
        auto found = torrents_.find(owner);
        if (found == torrents_.end()) missing();
        value = text_field(found->second.overview);
    } else if (model == "draft") {
        current(std::to_string(draft_.revision), revision);
        if (key == "name") value = draft_.name;
        else if (key == "effectiveName") { temporary = effective_name(draft_); value = temporary; }
        else if (key == "comment") value = draft_.comment;
        else if (key == "creator") value = draft_.creator;
        else if (key == "source") value = draft_.source_tag;
        else if (key == "output") { temporary = core::to_utf8(draft_.output); value = temporary; }
        else if (key.starts_with("/trackers/") && key.ends_with("/url")) {
            auto digits = std::string_view(key).substr(10, key.size() - 14);
            std::size_t index = 0;
            auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (error != std::errc{} || end != digits.data() + digits.size() || index >= draft_.trackers.size()) missing();
            value = draft_.trackers[index].url;
        } else missing();
    } else if (model == "source") {
        current(std::to_string(draft_.revision), revision);
        auto found = std::find_if(draft_.sources.begin(), draft_.sources.end(), [&](auto const& row) { return row.id == owner; });
        if (found == draft_.sources.end()) missing();
        if (key == "path") temporary = core::to_utf8(found->path);
        else missing();
        value = temporary;
    } else if (model == "review") {
        current(std::to_string(draft_.revision), revision);
        held = validate_draft_locked();
        value = text_field(held);
    } else if (model == "batch") {
        current(std::to_string(batch_revision_), revision);
        if (!batch_) missing();
        auto index_from = [&](std::string_view text) {
            std::size_t index = 0;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), index);
            if (error != std::errc{} || end != text.data() + text.size()) missing();
            return index;
        };
        if (key.starts_with("/notes/")) {
            auto index = index_from(std::string_view(key).substr(7));
            if (index >= batch_->notes.size()) missing();
            value = batch_->notes[index];
        } else if (key.starts_with("/sourceRoots/")) {
            auto index = index_from(std::string_view(key).substr(13));
            auto item = std::find_if(batch_->items.begin(), batch_->items.end(), [&](auto const& row) { return row.id == owner; });
            if (item == batch_->items.end() || index >= item->sources.size()) missing();
            temporary = core::to_utf8(item->sources[index].path); value = temporary;
        } else if (key.starts_with("/items/")) {
            auto slash = key.find('/', 7);
            if (slash == std::string::npos) missing();
            auto index = index_from(std::string_view(key).substr(7, slash - 7));
            if (index >= batch_->items.size()) missing();
            auto const& row = batch_->items[index];
            auto field = key.substr(slash + 1);
            if (field == "name") value = row.name;
            else if (field == "output") { temporary = core::to_utf8(row.output); value = temporary; }
            else if (field == "plannedOutput") { temporary = core::to_utf8(row.planned_output); value = temporary; }
            else missing();
        } else missing();
    } else if (model == "profilePlan") {
        current(std::to_string(draft_.revision), revision);
        auto p = find_profile_locked(owner);
        if (!p) missing();
        auto plan = service::plan_profile(draft_, *p);
        held = json::object();
        for (auto& change : plan.changes) held[change.field] = {{"before", std::move(change.before)}, {"after", std::move(change.after)}};
        value = text_field(held);
    } else missing();
    if (offset > value.size() || (offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80))
        throw ServiceError("INVALID_ARGUMENT", "Invalid text offset");
    auto text = view::preview(value.substr(offset), 8192);
    auto next = offset + text.size();
    return {{"text", std::move(text)}, {"totalBytes", value.size()}, {"offset", offset},
        {"nextOffset", next < value.size() ? json(next) : json(nullptr)}, {"revision", revision}};
}

json AppService::started_batch() const
{
    std::lock_guard lock(mutex_);
    if (last_started_batch_.empty()) missing();
    auto page = model_page_locked("batchJobs", "items", last_started_batch_, 0, 50, "");
    return {{"batchId", last_started_batch_}, {"jobIds", std::move(page["items"])},
        {"total", page["total"]}, {"nextOffset", page["nextOffset"]}};
}
} // namespace tc::service
