// The full workflow through the JSON bridge with a fake native host
// (specification section 13; fixtures U01, U03).

#include "tc/bridge/app_operations.hpp"
#include "tc/bridge/bencode_json.hpp"
#include "tc/bridge/protocol.hpp"
#include "tc/core/metainfo.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>

using namespace tc;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view origin = "https://torrentcontrol.example/index.html";

struct FakeHost final : bridge::HostServices {
    std::deque<std::vector<fs::path>> opens;
    std::deque<std::optional<fs::path>> saves;
    std::vector<fs::path> shown;
    std::vector<fs::path> launched;
    std::vector<std::string> urls;
    std::vector<std::string> save_names;

    std::vector<fs::path> pick_open(OpenKind) override
    {
        if (opens.empty()) return {};
        auto r = opens.front();
        opens.pop_front();
        return r;
    }
    std::optional<fs::path> pick_save(SaveKind, std::string const& name, fs::path const&) override
    {
        save_names.push_back(name);
        if (saves.empty()) return std::nullopt;
        auto r = saves.front();
        saves.pop_front();
        return r;
    }
    void show_in_folder(fs::path const& f) override { shown.push_back(f); }
    bool open_with_default_app(fs::path const& f) override
    {
        launched.push_back(f);
        return true;
    }
    bool open_url(std::string const& u) override
    {
        urls.push_back(u);
        return true;
    }
};

struct Bridge {
    tc::test::TempDir dir;
    FakeHost host;
    bridge::EventChannel channel;
    std::mutex m;
    std::vector<json> events;
    std::unique_ptr<service::AppService> app;
    bridge::Dispatcher d;
    int next = 1;

    explicit Bridge(bool persistent = false)
    {
        service::AppService::Options o;
        if (persistent) o.settings_path = dir.path() / "settings.json";
        o.jobs.progress_interval = std::chrono::milliseconds(0);
        app = std::make_unique<service::AppService>(std::move(o), [this](json const& e) {
            std::lock_guard l(m);
            events.push_back(json::parse(channel.wrap(e)));
        });
        bridge::register_core_operations(d, "test");
        bridge::register_app_operations(d, *app, host);
    }

    json call(std::string const& op, json payload = json::object(), std::optional<std::string> revision = std::nullopt,
        std::vector<fs::path> const& attached = {})
    {
        json msg{{"protocolVersion", 1}, {"requestId", "r" + std::to_string(next++)}, {"operation", op}, {"payload", payload}};
        if (revision) msg["draftRevision"] = *revision;
        return json::parse(d.handle(msg.dump(), origin, attached));
    }
    json ok(std::string const& op, json payload = json::object(), std::optional<std::string> revision = std::nullopt)
    {
        json r = call(op, std::move(payload), std::move(revision));
        INFO(op << ": " << r.dump());
        REQUIRE(r["ok"] == true);
        return r["result"];
    }
};

} // namespace

TEST_CASE("client launch refuses executable extensions even for valid metainfo", "[bridge][app][security]")
{
    Bridge b;
    auto const source = b.dir.path() / "payload.bin";
    test::write_file(source, 1024);
    auto const torrent = test::make_torrent(source, core::TorrentFormat::V1, 16 * 1024);
    for (auto const* name : {"payload.cmd", "payload.BAT", "payload.exe", "payload.lnk", "payload.url", "payload"}) {
        auto const path = b.dir.path() / name;
        test::write_bytes(path, torrent);
        auto const opened = b.app->open_torrent(path);
        auto const response = b.call("openInClient", {{"id", opened["id"]}});
        CHECK(response["ok"] == false);
        if (response["ok"] == false) CHECK(response["error"]["code"] == "UNSUPPORTED_FILE_TYPE");
        CHECK(b.host.launched.empty());
    }
    for (auto const* name : {"payload.torrent", "payload.TORRENT"}) {
        auto const path = b.dir.path() / name;
        test::write_bytes(path, torrent);
        auto const opened = b.app->open_torrent(path);
        CHECK(b.ok("openInClient", {{"id", opened["id"]}})["launched"] == true);
        REQUIRE_FALSE(b.host.launched.empty());
        CHECK(b.host.launched.back() == fs::absolute(path).lexically_normal());
    }
}

TEST_CASE("imported projects cannot restore prior write hydration or resource consent", "[bridge][app][security]")
{
    Bridge b;
    auto const source = b.dir.path() / "payload.bin";
    auto const output = b.dir.path() / "existing.torrent";
    test::write_file(source, 1024);
    test::write_bytes(output, "preserve existing data");
    b.app->add_sources({source});
    b.app->wait_for_scan();
    b.app->set_output(output);
    b.app->update_draft({{"replaceExisting", true}, {"allowHydration", true}, {"acceptLargeResourceUse", true}}, std::nullopt);
    auto const project = b.dir.path() / "import.tcproject";
    b.app->save_project(project);
    b.app->new_draft();
    b.host.opens.push_back({project});
    auto const draft = b.ok("openProject")["draft"];
    CHECK(draft["output"] == core::to_utf8(fs::absolute(output).lexically_normal()));
    CHECK(draft["replaceExisting"] == false);
    CHECK(draft["allowHydration"] == false);
    CHECK(draft["acceptLargeResourceUse"] == false);
    b.app->wait_for_scan();
    auto const started = b.call("startCreate");
    b.app->jobs().wait_idle();
    CHECK(started["ok"] == false);
    CHECK(test::read_all(output) == "preserve existing data");
}

TEST_CASE("create through the bridge from native selection to result actions", "[bridge][app]")
{
    Bridge b;
    fs::path const root = b.dir.path() / "Holiday photos";
    tc::test::write_file(root / "a.jpg", 300 * 1024, 1);
    tc::test::write_file(root / "b.jpg", 200 * 1024, 2);

    CHECK(b.ok("selectSources", {{"kind", "folder"}})["cancelled"] == true); // dialog cancelled
    b.host.opens.push_back({root});
    json draft = b.ok("selectSources", {{"kind", "folder"}})["draft"];
    CHECK(draft["effectiveName"] == "Holiday photos");
    b.app->wait_for_scan();

    json const page = b.ok("getManifestPage", {{"offset", 0}, {"limit", 10}});
    CHECK(page["total"] == 2);
    CHECK(page["entries"][0]["torrentPath"] == "Holiday photos/a.jpg");
    CHECK(page["entries"][0]["length"] == "307200"); // decimal string across the bridge

    fs::path const out = b.dir.path() / "photos.torrent";
    b.host.saves.push_back(out);
    draft = b.ok("chooseOutput")["draft"];
    CHECK(b.host.save_names.back() == "Holiday photos.torrent");
    CHECK(draft["output"] == core::to_utf8(fs::absolute(out).lexically_normal()));
    CHECK(draft["outputAuto"] == false);

    draft = b.ok("updateDraft", {{"patch", {{"format", "v1"}, {"comment", "<img src=x onerror=alert(1)>"}}}},
        draft["revision"].get<std::string>())["draft"];
    json const v = b.ok("validateDraft");
    REQUIRE(v["canCreate"] == true);

    std::string const job = b.ok("startCreate")["jobId"];
    b.app->jobs().wait_idle();
    json const snap = b.ok("getSnapshot");
    REQUIRE(snap["jobs"].size() == 1);
    json const result = snap["jobs"][0]["result"];
    CHECK(snap["jobs"][0]["state"] == "Succeeded");
    CHECK(result["format"] == "v1");
    core::Metainfo const m = core::Metainfo::parse(tc::test::read_all(out));
    CHECK(m.root().find("comment")->text() == "<img src=x onerror=alert(1)>"); // stored as data

    CHECK(b.ok("exportMagnet", {{"id", job}})["magnet"].get<std::string>().starts_with("magnet:?xt=urn:btih:"));
    b.ok("showInFolder", {{"id", job}});
    CHECK(b.host.shown.back() == fs::absolute(out).lexically_normal());
    CHECK(b.ok("openInClient", {{"id", job}})["launched"] == true);

    // Events are wrapped with monotonic sequence numbers.
    std::lock_guard l(b.m);
    REQUIRE(!b.events.empty());
    std::uint64_t last = 0;
    for (auto const& e : b.events) {
        CHECK(e["protocolVersion"] == 1);
        std::uint64_t const seq = std::stoull(e["sequence"].get<std::string>());
        CHECK(seq > last);
        last = seq;
    }
}

TEST_CASE("drag and drop uses natively attached files only", "[bridge][app][U01]")
{
    Bridge b;
    fs::path const file = b.dir.path() / "dropped.bin";
    tc::test::write_file(file, 1000, 1);

    // A page cannot smuggle a path in the payload.
    json r = b.call("addDroppedSources", {{"paths", json::array({"/etc"})}});
    CHECK(r["ok"] == false);
    CHECK(r["error"]["code"] == "NO_FILES");
    CHECK(b.app->draft_json()["sources"].empty());

    r = b.call("addDroppedSources", json::object(), std::nullopt, {file});
    REQUIRE(r["ok"] == true);
    CHECK(r["result"]["draft"]["sources"].size() == 1);
    CHECK(r["result"]["draft"]["sources"][0]["isDirectory"] == false);
}

TEST_CASE("malformed and hostile bridge messages are rejected", "[bridge][app][U01]")
{
    Bridge b;
    CHECK(b.call("updateDraft", {{"patch", "x"}})["error"]["code"] == "INVALID_PAYLOAD");
    CHECK(b.call("updateDraft", {{"patch", {{"output", "C:/Windows/evil.torrent"}}}})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(b.call("getManifestPage", {{"limit", -1}})["error"]["code"] == "INVALID_PAYLOAD");
    CHECK(b.call("pauseJob", {{"jobId", "job-999"}})["error"]["code"] == "JOB_NOT_FOUND");
    CHECK(b.call("showInFolder", {{"id", "../../etc/passwd"}})["error"]["code"] == "NOT_FOUND");
    CHECK(b.host.shown.empty());

    for (std::string const url : {"javascript:alert(1)", "file:///C:/Windows", "ms-settings:", "https://x.example/\" --flag"})
        CHECK(b.call("openExternalLink", {{"url", url}})["error"]["code"] == "INVALID_PAYLOAD");
    CHECK(b.host.urls.empty());
    CHECK(b.ok("openExternalLink", {{"url", "https://github.com/qenuternis2/TorrentControl"}})["opened"] == true);

    // Stale revisions are retryable: the page refreshes and tries again.
    json const r = b.call("updateDraft", {{"patch", {{"comment", "x"}}}}, "999");
    CHECK(r["error"]["code"] == "STALE_REVISION");
    CHECK(r["error"]["retryable"] == true);
}

TEST_CASE("profile export is redacted by default", "[bridge][app][U05]")
{
    Bridge b;
    json draft = b.ok("applyProfile", {{"profileId", "private"}})["draft"];
    b.ok("updateDraft", {{"patch", {{"trackers", json::array({{{"url", "https://p.example/announce?passkey=TOPSECRET"}}})}}}});
    auto saved = b.ok("saveProfile", {{"name", "Mine: <b>bold</b>"}});
    std::string const id = saved["profileId"];
    CHECK(saved["profilesTotal"] == 5);
    CHECK(saved["profilesRevision"] == "1");
    CHECK(saved["draft"]["profileMeta"]["id"] == id);
    CHECK(b.ok("listProfiles").dump().find("TOPSECRET") == std::string::npos);

    fs::path const out = b.dir.path() / "export.json";
    b.host.saves.push_back(out);
    json const r = b.ok("exportProfile", {{"profileId", id}});
    CHECK(r["redacted"] == true);
    CHECK(b.host.save_names.back() == "Mine_ _b_bold__b_.tcprofile.json");
    CHECK(tc::test::read_all(out).find("TOPSECRET") == std::string::npos);
    auto deleted = b.ok("deleteProfile", {{"profileId", id}});
    CHECK(deleted["profilesTotal"] == 4);
    CHECK(deleted["profilesRevision"] == "2");
    CHECK(deleted["draft"]["profileMeta"].is_null());
}

TEST_CASE("an opened torrent is described and verified through the bridge", "[bridge][app]")
{
    Bridge b;
    fs::path const root = b.dir.path() / "Data";
    tc::test::write_file(root / "x.bin", 70 * 1024, 3);
    fs::path const torrent = b.dir.path() / "data.torrent";
    tc::test::write_bytes(torrent, tc::test::make_torrent(root, core::TorrentFormat::Hybrid, 16384));

    b.host.opens.push_back({torrent});
    json const t = b.ok("openTorrent")["torrent"];
    CHECK(t["format"] == "hybrid");
    CHECK(t["problems"].empty());
    CHECK(b.ok("getTorrentFiles", {{"torrentId", t["id"]}})["total"].get<int>() >= 1);

    b.host.opens.push_back({root});
    std::string const job = b.ok("verifyPayload", {{"torrentId", t["id"]}})["jobId"];
    b.app->jobs().wait_idle();
    CHECK(b.app->jobs().find(job)->state == service::JobState::Succeeded);
}

TEST_CASE("metadata editing through the bridge previews and commits only a native selected destination", "[bridge][editor][E01]")
{
    Bridge b;
    auto payload = b.dir.path() / "data.bin";
    auto input = b.dir.path() / "original.torrent";
    auto output = b.dir.path() / "edited.torrent";
    test::write_file(payload, 100003, 7);
    auto bytes = test::make_torrent(payload, core::TorrentFormat::Hybrid, 16384);
    service::write_file_atomic(input, bytes);
    b.host.opens.push_back({input});
    auto torrent = b.ok("openTorrent")["torrent"];
    auto id = torrent["id"];
    CHECK(b.ok("getSnapshot")["torrent"] == torrent);
    auto fields = b.ok("getTorrentFields", {{"torrentId", id}, {"scope", "info"}, {"limit", 2}});
    CHECK(fields["rows"].size() == 2);
    CHECK(fields["total"].get<int>() > 2);
    auto typed_key = [](std::string const& key) { return json{{"t", "str"}, {"utf8", key}}; };
    auto patch = json::array({{{"key", typed_key("comment")}, {"value", {{"t", "str"}, {"utf8", "Edited through the bridge"}}}}});
    auto p = b.ok("previewTorrentEdit", {{"torrentId", id}, {"outer", patch}, {"info", json::array()}});
    CHECK(p["rawInfoPreserved"] == true);
    CHECK(p["newHashes"] == p["oldHashes"]);
    CHECK(b.ok("getSnapshot")["editorPreview"] == p);
    CHECK(b.ok("chooseEditorOutput", {{"token", p["token"]}, {"path", core::to_utf8(output)}})["cancelled"] == true);
    CHECK_FALSE(fs::exists(output));
    b.host.saves.push_back(output);
    auto chosen = b.ok("chooseEditorOutput", {{"token", p["token"]}});
    CHECK(b.host.save_names.back() == "original.edited.torrent");
    CHECK(chosen["requiresReplace"] == false);
    auto saved = b.ok("saveTorrentEdit", {{"token", p["token"]}});
    CHECK(saved["torrent"]["infohashV1"] == torrent["infohashV1"]);
    CHECK(saved["torrent"]["infohashV2"] == torrent["infohashV2"]);
    CHECK(service::read_small_file(input) == bytes);
    auto edited = core::Metainfo::parse(service::read_small_file(output));
    CHECK(edited.raw_info() == core::Metainfo::parse(bytes).raw_info());
    CHECK(edited.root().find("comment")->text() == "Edited through the bridge");
    CHECK(b.call("saveTorrentEdit", {{"token", p["token"]}})["error"]["code"] == "STALE_PREVIEW");
    CHECK(b.ok("getSnapshot")["editorPreview"].is_null());
    b.ok("newDraft");
    CHECK(b.ok("getSnapshot")["torrent"].is_null());
}

TEST_CASE("metadata save refuses stale tokens external changes and implicit overwrites", "[bridge][editor]")
{
    Bridge b;
    auto payload = b.dir.path() / "data.bin";
    auto input = b.dir.path() / "original.torrent";
    test::write_file(payload, 100, 7);
    auto bytes = test::make_torrent(payload, core::TorrentFormat::V1, 16384);
    service::write_file_atomic(input, bytes);
    b.host.opens.push_back({input});
    auto id = b.ok("openTorrent")["torrent"]["id"];
    json change{{"torrentId", id}, {"outer", json::array()}, {"info", json::array({
        {{"key", {{"t", "str"}, {"utf8", "source"}}}, {"value", {{"t", "str"}, {"utf8", "new"}}}}})}};
    auto first = b.ok("previewTorrentEdit", change);
    auto p = b.ok("previewTorrentEdit", change);
    CHECK(b.call("chooseEditorOutput", {{"token", first["token"]}})["error"]["code"] == "STALE_PREVIEW");
    CHECK(p["infoChanged"] == true);
    CHECK(p["oldHashes"] != p["newHashes"]);
    CHECK(b.call("saveTorrentEdit", {{"token", p["token"]}})["error"]["code"] == "NO_OUTPUT");
    b.host.saves.push_back(input);
    CHECK(b.ok("chooseEditorOutput", {{"token", p["token"]}})["requiresReplace"] == true);
    CHECK(b.call("saveTorrentEdit", {{"token", p["token"]}})["error"]["code"] == "OUTPUT_CONFLICT");
    CHECK(service::read_small_file(input) == bytes);
    service::write_file_atomic(input, bytes + "modified externally");
    CHECK(b.call("saveTorrentEdit", {{"token", p["token"]}, {"replaceExisting", true}})["error"]["code"] == "SOURCE_CHANGED");
    CHECK(service::read_small_file(input) == bytes + "modified externally");
    service::write_file_atomic(input, bytes);
    auto saved = b.ok("saveTorrentEdit", {{"token", p["token"]}, {"replaceExisting", true}});
    CHECK(saved["torrent"]["infohashV1"] == p["newHashes"]["v1"]);
    CHECK(core::Metainfo::parse(service::read_small_file(input)).info().find("source")->text() == "new");
}

TEST_CASE("editor bridge preserves binary extensions and rejects ambiguous or incomplete patches", "[bridge][editor][E02]")
{
    Bridge b;
    auto payload = b.dir.path() / "data.bin";
    auto input = b.dir.path() / "original.torrent";
    test::write_file(payload, 100, 7);
    auto meta = core::Metainfo::parse(test::make_torrent(payload, core::TorrentFormat::V1, 16384));
    service::write_file_atomic(input, core::apply_outer_edit(meta, {{"large", core::bencode::Value::string(std::string(100000, 'x'))}}));
    b.host.opens.push_back({input});
    auto id = b.ok("openTorrent")["torrent"]["id"];
    auto large = b.ok("getTorrentField", {{"torrentId", id}, {"scope", "top"}, {"key", {{"t", "str"}, {"utf8", "large"}}}});
    CHECK(large["editable"] == false);
    CHECK(large["value"]["truncated"] == true);
    auto change = json{{"key", {{"t", "bytes"}, {"hex", "00ff"}}}, {"value", {{"t", "int"}, {"v", "900719925474099312345"}}}};
    auto p = b.ok("previewTorrentEdit", {{"torrentId", id}, {"outer", json::array({change})}, {"info", json::array()}});
    b.host.saves.push_back(b.dir.path() / "copy.torrent");
    b.ok("chooseEditorOutput", {{"token", p["token"]}});
    auto new_id = b.ok("saveTorrentEdit", {{"token", p["token"]}})["torrent"]["id"];
    auto value = b.ok("getTorrentField", {{"torrentId", new_id}, {"scope", "top"}, {"key", change["key"]}});
    CHECK(value["value"]["v"] == "900719925474099312345");
    CHECK(b.call("previewTorrentEdit", {{"torrentId", id}, {"outer", json::array({change, change})}, {"info", json::array()}})["ok"] == false);
    change["value"] = large["value"];
    CHECK(b.call("previewTorrentEdit", {{"torrentId", id}, {"outer", json::array({change})}, {"info", json::array()}})["ok"] == false);
    CHECK(b.call("getTorrentFields", {{"torrentId", id}, {"scope", "file"}})["ok"] == false);
}

TEST_CASE("binary imported comments stay in the native model instead of breaking JSON summaries", "[bridge][editor][E02]")
{
    Bridge b;
    auto payload = b.dir.path() / "data.bin";
    auto input = b.dir.path() / "original.torrent";
    test::write_file(payload, 100, 7);
    auto meta = core::Metainfo::parse(test::make_torrent(payload, core::TorrentFormat::V1, 16384));
    service::write_file_atomic(input, core::apply_outer_edit(meta, {{"comment", core::bencode::Value::string(std::string("\xff\x00", 2))}}));
    b.host.opens.push_back({input});
    auto tor = b.ok("openTorrent")["torrent"];
    CHECK(tor["comment"].is_null());
    auto field = b.ok("getTorrentField", {{"torrentId", tor["id"]}, {"scope", "top"}, {"key", {{"t", "str"}, {"utf8", "comment"}}}});
    CHECK(field["value"]["t"] == "bytes");
    CHECK(field["value"]["hex"] == "ff00");
}

TEST_CASE("diagnostics bridge requires native targets and an explicit valid connection policy", "[bridge][diagnostics][N05]")
{
    Bridge b;
    b.ok("updateDraft", {{"patch",{{"trackers",json::array()}}}}, "1");
    CHECK(b.ok("getDiagnosticTargets", {{"kind","trackers"}})["targets"].empty());
    CHECK(b.call("startDiagnostics", {{"kind","trackers"},{"url","http://page-chosen.invalid/announce"}})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(b.call("startDiagnostics", {{"kind","trackers"},{"torrentId","page-owned-id"}})["error"]["code"] == "NOT_FOUND");
    CHECK(b.call("startDiagnostics", {{"kind","trackers"},{"networkMode","http-proxy"}})["error"]["code"] == "INVALID_PROXY");
    CHECK(b.call("startDiagnostics", {{"kind","trackers"},{"networkMode","direct"},{"httpProxy","http://127.0.0.1:1"}})["error"]["code"] == "INVALID_PROXY");
    CHECK(b.call("getDiagnosticPage", {{"runId","not-native"},{"offset",0},{"limit",51}})["ok"] == false);
    CHECK(b.app->snapshot()["diagnostics"].empty());
}

TEST_CASE("metadata field cursors page large binary keys without losing editable native bytes", "[bridge][editor][bounds]")
{
    Bridge b;
    auto payload = b.dir.path() / "payload.bin";
    tc::test::write_file(payload, 32, 1);
    b.app->add_sources({payload}); b.app->wait_for_scan();
    b.app->update_draft({{"trackers", json::array()}}, std::nullopt);
    auto job = b.app->start_create(); b.app->jobs().wait_idle();
    auto original = b.app->open_torrent(b.app->jobs().find(job)->result->output);
    auto meta = b.app->torrent_metainfo(original["id"]);
    core::OuterEdit patch;
    for (int i = 0; i < 100; ++i) {
        auto prefix = "extension-" + std::to_string(1000 + i);
        patch[prefix + std::string(4096 - prefix.size(), '\0')] = core::bencode::Value::string("preserved");
    }
    auto extended = b.dir.path() / "extended.torrent";
    service::write_file_atomic(extended, core::apply_outer_edit(*meta, patch));
    b.host.opens.push_back({extended});
    auto torrent = b.ok("openTorrent")["torrent"];
    std::size_t offset = 0, seen = 0, pages = 0;
    for (;;) {
        auto page = b.ok("getTorrentFields", {{"torrentId", torrent["id"]}, {"scope", "top"}, {"offset", offset}, {"limit", 50}});
        CHECK(page.dump().size() < 256 * 1024 + 128);
        ++pages;
        for (auto const& row : page["rows"]) {
            auto key = bridge::bencode_from_json(row["key"]).text();
            if (!key.starts_with("extension-")) continue;
            CHECK(key.size() == 4096);
            auto value = b.ok("getTorrentField", {{"torrentId", torrent["id"]}, {"scope", "top"}, {"key", row["key"]}});
            CHECK(value["value"]["utf8"] == "preserved");
            CHECK(value["editable"] == true);
            ++seen;
        }
        if (page["nextOffset"].is_null()) break;
        auto next = page["nextOffset"].get<std::size_t>();
        REQUIRE(next > offset);
        offset = next;
    }
    CHECK(seen == 100);
    CHECK(pages > 2); // byte budget, rather than only a row-count budget
    CHECK(b.app->torrent_metainfo(torrent["id"])->raw_info() == meta->raw_info());
}

TEST_CASE("settings IO failures cross the bridge without committing a change", "[bridge][settings]")
{
    Bridge b(true);
    auto const before = b.ok("updateSettings", {{"patch", {{"theme", "light"}}}});
    auto const file = b.dir.path() / "settings.json";
    auto const backup = b.dir.path() / "backup.json";
    fs::rename(file, backup);
    fs::create_directory(file);
    tc::test::write_file(file / "keep", 1);
    auto const failed = b.call("updateSettings", {{"patch", {{"theme", "dark"}}}});
    CHECK(failed["ok"] == false);
    CHECK(failed["error"]["code"] == "SETTINGS_WRITE_FAILED");
    CHECK(failed["error"]["retryable"] == true);
    CHECK(b.ok("getSettings") == before);
    fs::remove_all(file);
    fs::rename(backup, file);
    CHECK(b.ok("updateSettings", {{"patch", {{"theme", "dark"}}}})["theme"] == "dark");
    CHECK(service::load_settings(file).theme == "dark");
}

TEST_CASE("large draft collections cross the bridge as bounded pages and row mutations", "[bridge][paging]")
{
    Bridge b;
    json rows = json::array();
    for (int i = 0; i < 1000; ++i) rows.push_back({{"url", "https://tracker.example/" + std::string(1000, '\1') + std::to_string(i)}, {"tier", i % 999}, {"enabled", true}});
    b.app->update_draft({{"trackers", rows}}, std::nullopt);
    auto snapshot = b.ok("getSnapshot");
    auto revision = snapshot["draft"]["revision"].get<std::string>();
    CHECK(snapshot["draft"]["pages"]["trackers"]["total"] == 1000);
    CHECK(snapshot.dump().size() < bridge::max_message_bytes);
    auto page = b.ok("getModelPage", {{"model", "draft"}, {"key", "trackers"}, {"offset", 999u}, {"revision", revision}});
    CHECK(page["items"][0]["url"] == rows.back()["url"]);
    auto result = b.ok("editDraftRow", {{"key", "trackers"}, {"index", 999u}, {"action", "remove"}}, revision);
    CHECK(result["draft"]["pages"]["trackers"]["total"] == 999);
    auto rejected = b.call("editDraftRow", {{"key", "trackers"}, {"index", 0u}, {"action", "remove"}}, revision);
    CHECK(rejected["error"]["code"] == "STALE_REVISION");
    CHECK(b.app->draft_json()["pages"]["trackers"]["total"] == 999);
    auto invalid = b.call("getModelPage", {{"model", "draft"}, {"key", "trackers"}, {"limit", 0u}, {"revision", result["draft"]["revision"]}});
    CHECK(invalid["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(b.call("editDraftRow", {{"key", "trackers"}, {"action", "remove"}})["error"]["code"] == "INVALID_PAYLOAD");
}
