// The full workflow through the JSON bridge with a fake native host
// (specification section 13; fixtures U01, U03).

#include "tc/bridge/app_operations.hpp"
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

    Bridge()
    {
        service::AppService::Options o;
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
    std::string const id = b.ok("saveProfile", {{"name", "Mine: <b>bold</b>"}})["profileId"];
    CHECK(b.ok("listProfiles").dump().find("TOPSECRET") == std::string::npos);

    fs::path const out = b.dir.path() / "export.json";
    b.host.saves.push_back(out);
    json const r = b.ok("exportProfile", {{"profileId", id}});
    CHECK(r["redacted"] == true);
    CHECK(b.host.save_names.back() == "Mine_ _b_bold__b_.tcprofile.json");
    CHECK(tc::test::read_all(out).find("TOPSECRET") == std::string::npos);
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
