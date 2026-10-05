#include "tc/bridge/app_operations.hpp"

#include "tc/core/error.hpp"
#include "tc/service/sources.hpp"
#include "tc/service/storage.hpp"

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

} // namespace

void register_app_operations(Dispatcher& d, AppService& app, HostServices& host)
{
    using R = Request const&;
    using OK = HostServices::OpenKind;
    using SK = HostServices::SaveKind;

    d.register_operation("getSnapshot", [&app](json const&) { return app.snapshot(); });

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
    d.register_operation("updateBatch", [&app](json const& p) { return app.update_batch(object(p, "overrides")); });
    d.register_operation("startBatch", [&app](json const&) { return json{{"jobIds", app.start_batch()}}; });

    // ---- Jobs --------------------------------------------------------------------
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
