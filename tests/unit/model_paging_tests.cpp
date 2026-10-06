#include "tc/service/app_service.hpp"
#include "tc/service/storage.hpp"
#include "tc/bridge/app_operations.hpp"
#include "tc/bridge/protocol.hpp"
#include "tc/core/error.hpp"
#include "test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>

using namespace tc;
using namespace tc::service;
using nlohmann::json;

TEST_CASE("large draft pages retain unseen rows and enforce revisions", "[paging][service]")
{
    test::TempDir temp;
    AppService app({}, {});
    Draft draft;
    for (int i = 0; i < 100000; ++i) {
        SourceSpec source;
        source.id = "s-" + std::to_string(i);
        source.path = temp.path() / "missing" / (std::to_string(i) + ".bin");
        draft.sources.push_back(std::move(source));
    }
    // Escaping, not only row count, determines the next cursor.
    std::string const long_url = "https://tracker.example/" + std::string(1000, '\1');
    for (int i = 0; i < 1000; ++i) {
        draft.trackers.push_back({long_url + std::to_string(i), i % 999, true});
        draft.web_seeds.push_back("https://seed.example/" + std::to_string(i));
        draft.sources.front().exclusions.push_back("pattern-" + std::to_string(i));
    }
    auto project = temp.path() / "large.tcproject";
    save_project(project, draft, 0);
    CHECK(std::filesystem::file_size(project) > 16 * 1024 * 1024);
    CHECK(std::filesystem::file_size(project) <= max_project_bytes);
    auto opened = app.load_project(project);
    auto revision = opened["revision"].get<std::string>();
    auto source_id = opened["sources"][0]["id"].get<std::string>();
    REQUIRE(opened["pages"]["sources"]["total"] == 100000);
    CHECK(opened["sources"].size() <= 50);
    CHECK(opened["sources"][0]["exclusionsPaged"] == true);
    CHECK(opened.dump().size() < 256 * 1024);
    auto last = app.model_page("draft", "sources", "", 99999, 50, revision);
    REQUIRE(last["items"].size() == 1);
    CHECK(last["items"][0]["id"] == "src-100000");
    std::size_t offset = 0, seen = 0;
    do {
        auto page = app.model_page("draft", "trackers", "", offset, 50, revision);
        CHECK(page.dump().size() < 33 * 1024);
        for (auto const& row : page["items"]) {
            CHECK(row["url"] == long_url + std::to_string(seen));
            ++seen;
        }
        if (page["nextOffset"].is_null()) break;
        auto next = page["nextOffset"].get<std::size_t>();
        REQUIRE(next > offset);
        offset = next;
    } while (seen < 1000);
    CHECK(seen == 1000);
    auto changed = app.edit_draft_row("trackers", "", 999, "set",
        {{"url", "https://replacement.example/"}, {"tier", 5}, {"enabled", false}}, std::stoull(revision));
    try {
        app.edit_draft_row("trackers", "", 998, "remove", nullptr, std::stoull(revision));
        FAIL("Stale row mutation was accepted");
    } catch (ServiceError const& error) { CHECK(error.code() == "STALE_REVISION"); }
    CHECK_THROWS_AS(app.model_page("draft", "sources", "", 0, 50, revision), ServiceError);
    revision = changed["revision"].get<std::string>();
    changed = app.edit_draft_row("exclusions", source_id, 999, "set", "updated-pattern", std::stoull(revision));
    app.save_project(project);
    auto preserved = load_project(project);
    CHECK(preserved.sources.size() == 100000);
    CHECK(preserved.trackers.size() == 1000);
    CHECK(preserved.trackers.front().url == long_url + "0");
    CHECK(preserved.trackers.back().url == "https://replacement.example/");
    CHECK(preserved.sources.front().exclusions.front() == "pattern-0");
    CHECK(preserved.sources.front().exclusions.back() == "updated-pattern");
    CHECK(preserved.web_seeds.size() == 1000);
    auto recovery = app.snapshot();
    CHECK(recovery.dump().size() < bridge::max_message_bytes);
    CHECK(recovery["draft"]["pages"]["sources"]["total"] == 100000);
}

TEST_CASE("persistence limits reject oversized data before replacing previous files", "[paging][storage]")
{
    test::TempDir temp;
    auto project = temp.path() / "project.tcproject";
    test::write_bytes(project, "original-project");
    {
        Draft oversized;
        oversized.comment.assign(max_project_bytes + 1, 'x');
        CHECK_THROWS_AS(save_project(project, oversized, 0), core::CoreError);
        CHECK(test::read_all(project) == "original-project");
    }
    auto settings_file = temp.path() / "settings.json";
    test::write_bytes(settings_file, "original-settings");
    AppSettings settings;
    Profile profile; profile.id = "custom"; profile.name = "Large";
    profile.source_tag.assign(max_settings_bytes + 1, 'x');
    settings.custom_profiles.push_back(std::move(profile));
    CHECK_THROWS_AS(save_settings(settings_file, settings), core::CoreError);
    CHECK(test::read_all(settings_file) == "original-settings");
    auto sparse = temp.path() / "too-large.tcproject";
    { std::ofstream out(sparse, std::ios::binary); out.seekp(static_cast<std::streamoff>(max_project_bytes)); out.put('x'); }
    CHECK_THROWS_AS(load_project(sparse), core::CoreError);
}

TEST_CASE("profile summaries avoid copying complete profile collections", "[paging][profiles]")
{
    test::TempDir temp;
    auto path = temp.path() / "settings.json";
    AppSettings settings;
    settings.last_profile = "custom-74";
    for (int i = 0; i < 75; ++i) {
        Profile profile;
        profile.id = "custom-" + std::to_string(i);
        profile.name = "Profile " + std::to_string(i);
        for (int row = 0; row < 200; ++row) profile.trackers.push_back({"https://tracker.example/" + std::string(100, 'x') + std::to_string(row), row, true});
        settings.custom_profiles.push_back(std::move(profile));
    }
    save_settings(path, settings);
    AppService::Options options; options.settings_path = path;
    AppService app(options, {});
    auto snapshot = app.snapshot();
    CHECK(snapshot.dump().size() < 256 * 1024);
    CHECK(snapshot["profilesTotal"] == 79);
    REQUIRE(snapshot["profiles"].size() <= 51);
    CHECK(snapshot["profiles"].back()["id"] == "custom-74");
    CHECK_FALSE(app.settings_json().contains("customProfiles"));
    auto page = app.model_page("profiles", "items", "", 78, 50, snapshot["profilesRevision"]);
    CHECK(page["items"][0]["id"] == "custom-74");
    auto plan = app.plan_profile("trackerless");
    CHECK(plan.dump().size() < 32 * 1024);
    auto removed = app.model_page("profilePlan", "trackers.before", "trackerless", 199, 50, plan["draftRevision"]);
    CHECK(removed["items"][0]["url"] == settings.custom_profiles.back().trackers.back().url);
    app.save_custom_profile("New profile");
    CHECK_THROWS_AS(app.model_page("profiles", "items", "", 0, 50, snapshot["profilesRevision"]), ServiceError);
}

TEST_CASE("batch pages preserve earlier exclusions and reject stale plans", "[paging][batch]")
{
    test::TempDir temp;
    AppService app({}, {});
    Draft draft;
    for (int i = 0; i < 123; ++i) {
        auto path = temp.path() / (std::to_string(i) + ".bin");
        test::write_bytes(path, "x");
        SourceSpec source; source.id = "s-" + std::to_string(i); source.path = path;
        draft.sources.push_back(std::move(source));
    }
    auto project = temp.path() / "batch.tcproject";
    save_project(project, draft, 0); app.load_project(project);
    auto single = app.plan_batch("single", "rename", temp.path());
    CHECK(single["items"][0]["sourceRootsTotal"] == 123);
    CHECK_FALSE(single["items"][0].contains("sourceRoots"));
    auto roots = app.model_page("batch", "sourceRoots", "item-1", 122, 50, single["revision"]);
    CHECK(roots["items"][0] == core::to_utf8(draft.sources.back().path));
    auto root_text = app.model_text("batch", "/sourceRoots/122", "item-1", 0, single["revision"]);
    CHECK(root_text["text"] == core::to_utf8(draft.sources.back().path));
    auto plan = app.plan_batch("perFile", "rename", temp.path());
    CHECK(plan["total"] == 123);
    CHECK(plan["items"].size() <= 50);
    auto revision = plan["revision"].get<std::string>();
    auto last = app.model_page("batch", "items", "", 122, 50, revision);
    CHECK(last["items"][0]["id"] == "item-123");
    plan = app.update_batch({{"item-1", {{"included", false}}}}, revision);
    CHECK_THROWS_AS(app.update_batch({{"item-2", {{"included", false}}}}, revision), ServiceError);
    plan = app.update_batch({{"item-123", {{"included", false}}}}, plan["revision"]);
    CHECK(plan["includedTotal"] == 121);
    CHECK(plan["items"][0]["included"] == false);
    auto ids = app.start_batch(plan["revision"]);
    CHECK(ids.size() == 121);
    auto started = app.started_batch();
    CHECK(started["total"] == 121);
    CHECK(started["jobIds"].size() <= 50);
    auto tail = app.model_page("batchJobs", "items", started["batchId"], 120, 50, "");
    CHECK(tail["items"][0] == ids.back());
    app.jobs().wait_idle();
    auto report = app.jobs().batch_status(started["batchId"]);
    CHECK(report["total"] == 121);
    CHECK(report["done"] == 121);
    CHECK(report["finished"] == true);
    app.jobs().clear_finished();
    CHECK(app.jobs().batch_status(started["batchId"]) == report);
}

TEST_CASE("large torrent overview pages preserve original metadata and UTF8 text", "[paging][metainfo]")
{
    test::TempDir temp;
    auto payload = temp.path() / "payload.bin";
    test::write_bytes(payload, "payload");
    auto original = core::Metainfo::parse(test::make_torrent(payload, core::TorrentFormat::Hybrid, 16384));
    using core::bencode::Value;
    Value::List tiers;
    for (int i = 0; i < 2000; ++i) tiers.push_back(Value::list({Value::string("https://tracker.example/" + std::string(700, 'x') + std::to_string(i))}));
    std::string const comment = std::string(8191, 'a') + "\xf0\x9f\x92\xbe" + std::string(100000, 'z');
    core::OuterEdit edit;
    edit["announce-list"] = Value::list(std::move(tiers));
    edit["comment"] = Value::string(comment);
    auto bytes = core::apply_outer_edit(original, edit);
    auto path = temp.path() / "large.torrent";
    test::write_bytes(path, bytes);
    AppService app({}, {});
    auto overview = app.open_torrent(path);
    auto id = overview["id"].get<std::string>();
    CHECK(overview.dump().size() < 128 * 1024);
    CHECK(overview["trackersTotal"] == 2000);
    CHECK(overview["textFields"]["comment"] == comment.size());
    CHECK(app.snapshot().dump().size() < bridge::max_message_bytes);
    auto last = app.model_page("torrent", "trackers", id, 1999, 50, "");
    CHECK(last["items"].size() == 1);
    auto url = app.model_text("torrent", "/trackers/1999/url", id, 0, "");
    CHECK(url["text"] == "https://tracker.example/" + std::string(700, 'x') + "1999");
    std::string joined;
    std::size_t offset = 0;
    for (;;) {
        auto page = app.model_text("torrent", "comment", id, offset, "");
        CHECK(page.dump().size() < 50 * 1024);
        joined += page["text"].get<std::string>();
        if (page["nextOffset"].is_null()) break;
        auto next = page["nextOffset"].get<std::size_t>();
        REQUIRE(next > offset); offset = next;
    }
    CHECK(joined == comment);
    CHECK(app.torrent_metainfo(id)->bytes() == bytes);
    CHECK(app.torrent_metainfo(id)->raw_info() == original.raw_info());
    CHECK(test::read_all(path) == bytes);
}

TEST_CASE("oversized legacy tracker previews remain explicitly replaceable", "[paging][profiles]")
{
    test::TempDir temp;
    AppSettings settings;
    Profile profile; profile.id = "legacy"; profile.name = "Legacy profile";
    std::string const url = "https://tracker.example/" + std::string(100000, 'x');
    profile.trackers.push_back({url, 0, true});
    settings.custom_profiles.push_back(profile); settings.last_profile = profile.id;
    auto path = temp.path() / "settings.json"; save_settings(path, settings);
    AppService::Options options; options.settings_path = path;
    AppService app(options, {});
    auto draft = app.draft_json();
    REQUIRE(draft["trackers"].size() == 1);
    CHECK(draft["trackers"][0]["displayTruncated"] == true);
    auto revision = draft["revision"].get<std::string>();
    auto first = app.model_text("draft", "/trackers/0/url", "", 0, revision);
    CHECK(first["text"] == url.substr(0, 8192));
    CHECK(first["totalBytes"] == url.size());
    app.edit_draft_row("trackers", "", 0, "set", {{"url", "https://replacement.example/"}}, std::stoull(revision));
    CHECK(app.draft_json()["trackers"][0]["url"] == "https://replacement.example/");
    CHECK(load_settings(path).custom_profiles[0].trackers[0].url == url);
}
