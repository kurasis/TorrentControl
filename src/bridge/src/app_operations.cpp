#include "tc/bridge/app_operations.hpp"
#include "tc/bridge/bencode_json.hpp"

#include "tc/core/error.hpp"
#include "tc/service/sources.hpp"
#include "tc/service/storage.hpp"
#include <set>

namespace tc::bridge {

namespace {

using nlohmann::json;
using service::AppService;

std::string str(json const& p, char const* key, std::size_t max = 4096, bool required = true)
{
    auto it = p.find(key);
    if (it == p.end()) {
        if (required) throw BridgeError("INVALID_PAYLOAD", std::string("missing ") + key);
        return {};
    }
    if (!it->is_string() || it->get<std::string>().size() > max)
        throw BridgeError("INVALID_PAYLOAD", std::string(key) + " must be a string");
    return it->get<std::string>();
}

std::size_t count(json const& p, char const* key, std::size_t fallback, std::size_t max)
{
    auto it = p.find(key);
    if (it == p.end()) return fallback;
    if (!it->is_number_unsigned() || it->get<std::uint64_t>() > max)
        throw BridgeError("INVALID_PAYLOAD", std::string(key) + " must be a non-negative integer");
    return static_cast<std::size_t>(it->get<std::uint64_t>());
}

bool flag(json const& p, char const* key, bool fallback = false)
{
    auto it = p.find(key);
    if (it == p.end()) return fallback;
    if (!it->is_boolean()) throw BridgeError("INVALID_PAYLOAD", std::string(key) + " must be true or false");
    return it->get<bool>();
}

json object(json const& p, char const* key)
{
    auto it = p.find(key);
    if (it == p.end() || !it->is_object()) throw BridgeError("INVALID_PAYLOAD", std::string(key) + " must be an object");
    return *it;
}

json cancelled()
{
    return json{{"cancelled", true}};
}

json descriptor(core::FieldDescriptor const& f)
{
    return {{"scope", f.scope}, {"key", f.key}, {"type", f.type}, {"formats", f.formats},
        {"support", f.support}, {"editable", f.editable}, {"affectsHash", f.affects_hash},
        {"reference", f.reference}, {"validation", f.validation}};
}

std::string field_key(json const& j)
{
    auto value = bencode_from_json(j);
    if (!value.is_string() || value.text().empty() || value.text().size() > 4096)
        throw BridgeError("INVALID_PAYLOAD", "Field key must contain 1 to 4096 bytes");
    return value.text();
}

std::string scope(json const& p)
{
    auto s = str(p, "scope", 8);
    if (s != "top" && s != "info") throw BridgeError("INVALID_PAYLOAD", "scope must be top or info");
    return s;
}

core::OuterEdit edit_patch(json const& p, char const* key)
{
    auto it = p.find(key);
    if (it == p.end() || !it->is_array() || it->size() > 100)
        throw BridgeError("INVALID_PAYLOAD", "Edits must be an array of at most 100 fields");
    core::OuterEdit edit;
    for (auto const& entry : *it) {
        auto k = field_key(entry.at("key"));
        if (edit.contains(k)) throw BridgeError("INVALID_PAYLOAD", "Duplicate field edit");
        auto const& v = entry.at("value");
        edit.emplace(std::move(k), v.is_null() ? std::nullopt : std::optional(bencode_from_json(v)));
    }
    return edit;
}

bool complete(json const& j)
{
    if (j.is_object()) {
        if (j.value("t", "") == "elided" || j.value("truncated", false)) return false;
        for (auto const& v : j) if (!complete(v)) return false;
    } else if (j.is_array()) for (auto const& v : j) if (!complete(v)) return false;
    return true;
}

service::NetworkPolicy network_policy(json const& p)
{
    service::NetworkPolicy policy;
    policy.http_proxy = str(p, "httpProxy", 8192, false);
    auto mode = str(p, "networkMode", 16, false);
    if (mode.empty()) mode = "direct";
    if ((mode != "direct" && mode != "http-proxy") || (mode == "http-proxy" && policy.http_proxy.empty())
        || (mode == "direct" && !policy.http_proxy.empty())) throw BridgeError("INVALID_PROXY", "Select direct or provide an explicit HTTP proxy");
    policy.refresh = flag(p, "refresh");
    policy.udp_retry = flag(p, "udpRetry");
    return policy;
}

} // namespace

void register_app_operations(Dispatcher& d, AppService& app, HostServices& host)
{
    using R = Request const&;
    using OK = HostServices::OpenKind;
    using SK = HostServices::SaveKind;

    d.register_operation("getSnapshot", [&app](json const&) { return app.snapshot(); });
    d.register_operation("getModelPage", [&app](json const& p) {
        return app.model_page(str(p, "model", 16), str(p, "key", 256), str(p, "owner", 64, false),
            count(p, "offset", 0, 100'000'000), count(p, "limit", 50, 50), str(p, "revision", 20, false));
    });
    d.register_operation("getModelText", [&app](json const& p) {
        return app.model_text(str(p, "model", 16), str(p, "key", 256), str(p, "owner", 64, false),
            count(p, "offset", 0, 100'000'000), str(p, "revision", 20, false));
    });
    d.register_request_operation("editDraftRow", [&app](R r) {
        if (!r.draft_revision) throw BridgeError("INVALID_PAYLOAD", "draftRevision is required");
        return json{{"draft", app.edit_draft_row(str(r.payload, "key", 16), str(r.payload, "owner", 64, false),
            count(r.payload, "index", 0, 100'000'000), str(r.payload, "action", 16),
            r.payload.value("value", json(nullptr)), r.draft_revision)}};
    });
    d.register_operation("getDiagnosticTargets", [&app](json const& p) {
        return app.diagnostic_targets(str(p, "kind", 16), str(p, "torrentId", 64, false));
    });
    d.register_operation("startDiagnostics", [&app](json const& p) {
        return json{{"runId",app.start_diagnostics(str(p, "kind", 16), str(p, "torrentId", 64, false), network_policy(p))}};
    });
    d.register_operation("cancelDiagnostics", [&app](json const& p) {
        app.diagnostics().cancel(str(p, "runId", 64)); return json::object();
    });
    d.register_operation("getDiagnosticPage", [&app](json const& p) {
        return app.diagnostics().page(str(p, "runId", 64), count(p, "offset", 0, 256), count(p, "limit", 50, 50));
    });
    d.register_operation("getTrackerCatalog", [&app](json const&) { return app.diagnostics().catalog(); });
    d.register_operation("updateTrackerCatalog", [&app](json const& p) {
        return json{{"runId", app.update_tracker_catalog(network_policy(p))}};
    });
    d.register_operation("planCatalogApply", [&app](json const&) { return app.plan_catalog_apply(); });
    d.register_request_operation("applyTrackerCatalog", [&app](R r) {
        return json{{"draft", app.apply_catalog(str(r.payload, "checksum", 64), r.draft_revision)}};
    });

    // ---- Sources ----------------------------------------------------------
    d.register_operation("selectSources", [&app, &host](json const& p) {
        std::string const kind = str(p, "kind", 16);
        if (kind != "files" && kind != "folder") throw BridgeError("INVALID_PAYLOAD", "kind must be files or folder");
        auto paths = host.pick_open(kind == "files" ? OK::Files : OK::Folder);
        if (paths.empty()) return cancelled();
        return json{{"draft", app.add_sources(paths)}};
    });
    d.register_request_operation("addDroppedSources", [&app](R r) {
        if (r.attached_paths.empty()) throw BridgeError("NO_FILES", "Nothing that can be shared was dropped");
        return json{{"draft", app.add_sources(r.attached_paths)}};
    });
    d.register_request_operation("removeSource", [&app](R r) {
        return json{{"draft", app.remove_source(str(r.payload, "sourceId", 64), r.draft_revision)}};
    });
    d.register_request_operation("setSourceOptions", [&app](R r) {
        return json{{"draft", app.set_source_options(str(r.payload, "sourceId", 64), object(r.payload, "options"), r.draft_revision)}};
    });

    // ---- Draft ---------------------------------------------------------------
    d.register_request_operation("updateDraft", [&app](R r) {
        return json{{"draft", app.update_draft(object(r.payload, "patch"), r.draft_revision)}};
    });
    d.register_operation("newDraft", [&app](json const&) { return json{{"draft", app.new_draft()}}; });
    d.register_operation("chooseOutput", [&app, &host](json const&) {
        auto path = host.pick_save(SK::Torrent, app.suggested_name() + ".torrent", app.suggested_folder());
        if (!path) return cancelled();
        return json{{"draft", app.set_output(*path)}};
    });
    d.register_operation("planProfile", [&app](json const& p) { return app.plan_profile(str(p, "profileId", 64)); });
    d.register_request_operation("applyProfile", [&app](R r) {
        return json{{"draft", app.apply_profile(str(r.payload, "profileId", 64), r.draft_revision)}};
    });
    d.register_operation("undoDraft", [&app](json const&) { return json{{"draft", app.undo()}}; });
    d.register_operation("getManifestPage", [&app](json const& p) {
        return app.manifest_page(count(p, "offset", 0, 100'000'000), count(p, "limit", 250, 1000), str(p, "filter", 1024, false));
    });
    d.register_operation("getSkippedPage", [&app](json const& p) {
        return app.skipped_page(count(p, "offset", 0, 100'000'000), count(p, "limit", 250, 1000));
    });
    d.register_operation("validateDraft", [&app](json const&) { return app.validate_draft(); });
    d.register_operation("startCreate", [&app](json const&) { return json{{"jobId", app.start_create()}}; });

    // ---- Batch -----------------------------------------------------------------
    d.register_operation("planBatch", [&app, &host](json const& p) {
        std::filesystem::path folder;
        if (flag(p, "chooseFolder")) {
            auto picked = host.pick_open(OK::OutputFolder);
            if (picked.empty()) return cancelled();
            folder = picked.front();
        }
        return app.plan_batch(str(p, "mode", 32), str(p, "policy", 16), folder);
    });
    d.register_operation("updateBatch", [&app](json const& p) { return app.update_batch(object(p, "overrides"), str(p, "revision", 20)); });
    d.register_operation("startBatch", [&app](json const& p) {
        app.start_batch(str(p, "revision", 20)); return app.started_batch();
    });

    // ---- Jobs --------------------------------------------------------------------
    d.register_operation("getBatchStatus", [&app](json const& p) { return app.jobs().batch_status(str(p, "batchId", 64)); });
    d.register_operation("getJobsPage", [&app](json const& p) {
        return app.jobs().bridge_page(count(p, "offset", 0, 100'000'000), count(p, "limit", 50, 50));
    });
    d.register_operation("getJobTextPage", [&app](json const& p) {
        return app.jobs().text_page(str(p, "jobId", 64), str(p, "kind", 16), count(p, "offset", 0, 100'000'000), count(p, "limit", 20, 50));
    });
    d.register_operation("getJobText", [&app](json const& p) {
        return app.jobs().text_detail(str(p, "jobId", 64), str(p, "kind", 16), count(p, "index", 0, 100'000'000), str(p, "version", 20, false));
    });
    d.register_operation("getVerifyFilesPage", [&app](json const& p) {
        return app.jobs().verification_page(str(p, "jobId", 64), count(p, "offset", 0, 100'000'000),
            count(p, "limit", 250, 250), flag(p, "errorsOnly", true));
    });
    d.register_operation("getVerifyFile", [&app](json const& p) {
        return app.jobs().verification_file(str(p, "jobId", 64), count(p, "index", 0, 100'000'000));
    });
    d.register_operation("getJobLayoutPage", [&app](json const& p) {
        return app.jobs().layout_page(str(p, "jobId", 64), count(p, "offset", 0, 100'000'000), count(p, "limit", 50, 50));
    });
    d.register_operation("getJobLayoutRow", [&app](json const& p) {
        return app.jobs().layout_row(str(p, "jobId", 64), count(p, "index", 0, 100'000'000));
    });
    d.register_operation("pauseJob", [&app](json const& p) {
        app.jobs().pause(str(p, "jobId", 64));
        return json::object();
    });
    d.register_operation("resumeJob", [&app](json const& p) {
        app.jobs().resume(str(p, "jobId", 64));
        return json::object();
    });
    d.register_operation("cancelJob", [&app](json const& p) {
        app.jobs().cancel(str(p, "jobId", 64));
        return json::object();
    });
    d.register_operation("clearFinishedJobs", [&app](json const&) {
        app.jobs().clear_finished();
        return json::object();
    });

    // ---- Existing torrents and results ------------------------------------------------
    d.register_operation("getFieldRegistry", [](json const&) {
        json fields = json::array();
        for (auto const& f : core::field_registry()) fields.push_back(descriptor(f));
        return json{{"fields", std::move(fields)}};
    });
    d.register_operation("getTorrentFields", [&app](json const& p) {
        auto meta = app.torrent_metainfo(str(p, "torrentId", 64));
        auto s = scope(p);
        auto const& dict = s == "info" ? meta->info() : meta->root();
        auto offset = count(p, "offset", 0, 2'000'000);
        auto limit = count(p, "limit", 50, 50);
        auto const& entries = dict.entries();
        json rows = json::array();
        std::size_t bytes = 0;
        for (std::size_t i = offset; i < entries.size() && rows.size() < limit; ++i) {
            auto const& e = entries[i];
            auto const* f = core::find_field(s, e.key);
            json row{{"key", bencode_to_json(core::bencode::Value::string(e.key))},
                {"value", bencode_to_json(e.value, {8, 128})}, {"descriptor", f ? descriptor(*f) : json(nullptr)},
                {"editable", e.key.size() <= 4096 && (!f || f->editable)}};
            auto const size = row.dump().size();
            if (!rows.empty() && size > 256 * 1024 - bytes) break;
            bytes += size;
            rows.push_back(std::move(row));
        }
        auto const next = offset + rows.size();
        return json{{"rows", std::move(rows)}, {"total", entries.size()},
            {"nextOffset", next < entries.size() ? json(next) : json(nullptr)}};
    });
    d.register_operation("getTorrentField", [&app](json const& p) {
        auto meta = app.torrent_metainfo(str(p, "torrentId", 64));
        auto s = scope(p);
        auto k = field_key(p.at("key"));
        auto const* f = core::find_field(s, k);
        auto const* value = (s == "info" ? meta->info() : meta->root()).find(k);
        json shown = nullptr;
        bool editable = !f || f->editable;
        if (value) {
            bool const small = core::bencode::encode(*value).size() <= 65536;
            shown = bencode_to_json(*value, small ? DisplayBudget{10000, 65536, 768 * 1024} : DisplayBudget{64, 1024});
            if (shown.dump().size() > 256 * 1024) shown = bencode_to_json(*value, {64, 1024});
            editable = editable && small && complete(shown);
        }
        return json{{"key", p.at("key")}, {"value", std::move(shown)}, {"present", value != nullptr},
            {"editable", editable}, {"descriptor", f ? descriptor(*f) : json(nullptr)},
            {"signed", meta->root().find("signatures") != nullptr}};
    });
    d.register_operation("previewTorrentEdit", [&app](json const& p) {
        auto id = str(p, "torrentId", 64);
        auto outer = edit_patch(p, "outer");
        auto info = edit_patch(p, "info");
        auto meta = app.torrent_metainfo(id);
        json changes = json::array();
        auto describe = [&](core::OuterEdit const& patch, core::bencode::Value const& dict, char const* placement) {
            for (auto const& [key, value] : patch) {
                auto const* old = dict.find(key);
                if ((!old && !value) || (old && value && core::metadata_values_equal(*old, *value))) continue;
                changes.push_back({{"scope", placement}, {"key", bencode_to_json(core::bencode::Value::string(key))},
                    {"before", old ? bencode_to_json(*old, {8, 256}) : json(nullptr)},
                    {"after", value ? bencode_to_json(*value, {8, 256}) : json(nullptr)}});
            }
        };
        describe(outer, meta->root(), "top");
        describe(info, meta->info(), "info");
        return app.preview_torrent_edit(id, outer, info, flag(p, "removeSignatures"), changes);
    });
    d.register_operation("chooseEditorOutput", [&app, &host](json const& p) {
        auto token = str(p, "token", 64);
        auto preview = app.editor_preview(token);
        auto source = app.torrent_path(preview.at("torrentId").get<std::string>());
        auto path = host.pick_save(SK::Torrent, core::to_utf8(source.stem()) + ".edited.torrent", source.parent_path());
        if (!path) return cancelled();
        return app.choose_editor_output(token, *path);
    });
    d.register_operation("saveTorrentEdit", [&app](json const& p) {
        return app.save_torrent_edit(str(p, "token", 64), flag(p, "replaceExisting"));
    });
    d.register_operation("openTorrent", [&app, &host](json const&) {
        auto paths = host.pick_open(OK::Torrent);
        if (paths.empty()) return cancelled();
        return json{{"torrent", app.open_torrent(paths.front())}};
    });
    d.register_operation("openJobResult", [&app](json const& p) {
        return json{{"torrent", app.open_torrent(app.torrent_path(str(p, "jobId", 64)))}};
    });
    d.register_operation("getTorrentFiles", [&app](json const& p) {
        return app.torrent_files_page(str(p, "torrentId", 64), count(p, "offset", 0, 100'000'000), count(p, "limit", 250, 1000));
    });
    d.register_operation("verifyPayload", [&app, &host](json const& p) {
        std::string const id = str(p, "torrentId", 64);
        auto paths = host.pick_open(OK::PayloadFolder);
        if (paths.empty()) return cancelled();
        return json{{"jobId", app.verify_torrent(id, paths.front())}};
    });
    d.register_operation("exportMagnet", [&app](json const& p) { return json{{"magnet", app.magnet_for(str(p, "id", 64))}}; });
    d.register_operation("saveMagnet", [&app, &host](json const& p) {
        std::string const magnet = app.magnet_for(str(p, "id", 64));
        auto path = host.pick_save(SK::Magnet, app.suggested_name() + ".magnet.txt", app.suggested_folder());
        if (!path) return cancelled();
        service::write_file_atomic(*path, magnet + "\n");
        return json{{"saved", core::to_utf8(*path)}};
    });
    d.register_operation("showInFolder", [&app, &host](json const& p) {
        host.show_in_folder(app.torrent_path(str(p, "id", 64)));
        return json::object();
    });
    d.register_operation("openInClient", [&app, &host](json const& p) {
        // Launching a client is not proof that it found the payload.
        return json{{"launched", host.open_with_default_app(app.torrent_path(str(p, "id", 64)))}};
    });
    d.register_operation("openExternalLink", [&host](json const& p) {
        std::string const url = str(p, "url", 2048);
        if (!url.starts_with("https://") && !url.starts_with("http://"))
            throw BridgeError("INVALID_PAYLOAD", "Only web links can be opened");
        for (char c : url)
            if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '<' || c == '>' || c == '\\')
                throw BridgeError("INVALID_PAYLOAD", "The link contains characters that are not allowed");
        return json{{"opened", host.open_url(url)}};
    });

    // ---- Projects, settings, profiles -----------------------------------------------
    d.register_operation("saveProject", [&app, &host](json const&) {
        auto path = host.pick_save(SK::Project, app.suggested_name() + ".tcproject", app.suggested_folder());
        if (!path) return cancelled();
        app.save_project(*path);
        return json{{"saved", core::to_utf8(*path)}};
    });
    d.register_operation("openProject", [&app, &host](json const&) {
        auto paths = host.pick_open(OK::Project);
        if (paths.empty()) return cancelled();
        return json{{"draft", app.load_project(paths.front())}};
    });
    d.register_operation("getSettings", [&app](json const&) { return app.settings_json(); });
    d.register_operation("updateSettings", [&app](json const& p) { return app.update_settings(object(p, "patch")); });
    d.register_operation("listProfiles", [&app](json const&) { return json{{"profiles", app.profiles_json()}}; });
    d.register_operation("saveProfile", [&app](json const& p) {
        json const saved = app.save_custom_profile(str(p, "name", 200));
        return json{{"profileId", saved["id"]}, {"profiles", app.profiles_json()}, {"draft", app.draft_json()}};
    });
    d.register_operation("deleteProfile", [&app](json const& p) {
        app.delete_custom_profile(str(p, "profileId", 64));
        return json{{"profiles", app.profiles_json()}};
    });
    d.register_operation("exportProfile", [&app, &host](json const& p) {
        json const exported = app.export_profile(str(p, "profileId", 64), flag(p, "includeSecrets"));
        auto path = host.pick_save(SK::Profile, service::safe_file_name(exported.value("name", "profile")) + ".tcprofile.json", app.suggested_folder());
        if (!path) return cancelled();
        service::write_file_atomic(*path, exported.dump(2));
        return json{{"saved", core::to_utf8(*path)}, {"redacted", !flag(p, "includeSecrets")}};
    });
}

} // namespace tc::bridge
