#include "tc/service/app_service.hpp"
#include "test_support.hpp"
#include <catch2/catch_test_macros.hpp>

using nlohmann::json;
namespace fs = std::filesystem;

TEST_CASE("verification pages retain every real file and index errors separately", "[service][verify][paging]")
{
    tc::test::TempDir dir;
    auto root = dir.path() / "Files";
    // One v1 piece per file isolates the missing-file negative control from
    // otherwise valid files sharing a piece boundary.
    for (int i = 0; i < 261; ++i) tc::test::write_file(root / ("file-" + std::to_string(100000 + i) + ".bin"), 16384, 1);
    tc::service::AppService app({}, {});
    app.add_sources({root}); app.wait_for_scan();
    app.update_draft({{"format", "v1"}, {"pieceLength", 16384}, {"trackers", json::array()}}, std::nullopt);
    auto created = app.start_create(); app.jobs().wait_idle();
    auto opened = app.open_torrent(app.jobs().find(created)->result->output);
    auto layout = app.jobs().layout_page(created, 250, 50);
    CHECK(layout["rows"].size() == 11);
    CHECK(layout["total"] == 261);
    CHECK(layout["complete"] == true);
    CHECK(app.jobs().layout_row(created, 260)["torrentPath"] == "Files/file-100260.bin");
    auto log = app.jobs().text_page(created, "log", 0, 50);
    REQUIRE_FALSE(log["rows"].empty());
    CHECK(app.jobs().text_detail(created, "log", 0, log["version"])["text"] == app.jobs().find(created)->log.front());
    CHECK_THROWS_AS(app.jobs().text_detail(created, "log", 0, "0"), tc::service::ServiceError);
    CHECK_THROWS_AS(app.jobs().text_page(created, "unknown", 0, 50), tc::service::ServiceError);
    auto verified = app.verify_torrent(opened["id"], root); app.jobs().wait_idle();
    auto retained = app.retention_summary();
    CHECK(retained["createSpecs"] == 0);
    CHECK(retained["verifySpecs"] == 0);
    CHECK(retained["inputManifestEntries"] == 0);
    CHECK(retained["verifyInputBytes"] == 0);
    CHECK(retained["workers"] == 1);
    CHECK(app.jobs().bridge_job(verified)["verify"]["filesTotal"] == 261);
    auto first = app.jobs().verification_page(verified, 0, 250, false);
    auto last = app.jobs().verification_page(verified, 250, 250, false);
    CHECK(first["total"] == 261);
    CHECK(first["rows"].size() == 250);
    REQUIRE(last["rows"].size() == 11);
    CHECK(last["rows"][10]["index"] == 260);
    CHECK(last["rows"][10]["path"] == "Files/file-100260.bin");
    CHECK(app.jobs().verification_page(verified, 0, 250, true)["total"] == 0);
    auto native = app.jobs().find(verified);
    REQUIRE(native->verify["files"].size() == 261);
    auto preview = tc::service::to_json(*native, true);
    CHECK(preview["verify"]["files"].size() == 2);
    CHECK(preview["verify"]["filesTruncated"] == true);
    CHECK(preview["verify"]["filesTotal"] == 261);

    fs::remove(root / "file-100260.bin");
    auto failed = app.verify_torrent(opened["id"], root); app.jobs().wait_idle();
    auto errors = app.jobs().verification_page(failed, 0, 250, true);
    REQUIRE(errors["rows"].size() == 1);
    CHECK(errors["rows"][0]["index"] == 260);
    CHECK(errors["rows"][0]["status"] == "missing");
    auto detail = app.jobs().verification_file(failed, 260);
    CHECK(detail["file"]["path"] == "Files/file-100260.bin");
    CHECK(app.jobs().verification_page(failed, 261, 250, false)["rows"].empty());
    CHECK_THROWS_AS(app.jobs().verification_file(failed, 261), tc::service::ServiceError);
}

TEST_CASE("job history snapshots recover beyond the first page with a collection revision", "[service][paging][U03]")
{
    tc::service::AppService app({}, {});
    for (int i = 0; i < 73; ++i)
        app.jobs().enqueue_verify({"Invalid-" + std::to_string(i), "not bencode", {}});
    app.jobs().wait_idle();
    auto snapshot = app.snapshot();
    CHECK(snapshot["jobsTotal"] == 73);
    CHECK(snapshot["jobs"].size() == 50);
    CHECK(snapshot["nextJobsOffset"] == 50);
    auto page = app.jobs().bridge_page(50, 50);
    CHECK(page["jobs"].size() == 23);
    CHECK(page["nextOffset"].is_null());
    CHECK(page["collectionRevision"] == snapshot["jobsRevision"]);
    app.jobs().clear_finished();
    auto cleared = app.jobs().bridge_page(0, 50);
    CHECK(cleared["total"] == 0);
    CHECK(cleared["collectionRevision"] != page["collectionRevision"]);
}

TEST_CASE("100000 verification results remain native while the bridge preview stays small", "[service][paging][U02]")
{
    tc::service::JobSnapshot job;
    job.id = "large";
    job.kind = tc::service::JobKind::Verify;
    job.verify = {{"ok", false}, {"metainfoProblems", json::array()}, {"files", json::array()}};
    for (int i = 0; i < 100000; ++i)
        job.verify["files"].push_back({{"path", "file-" + std::to_string(i)}, {"status", "missing"}, {"message", "missing"}});
    job.verify["files"][0]["path"] = std::string(5000, 'x');
    auto summary = tc::service::to_json(job, true);
    CHECK(summary.dump().size() < 8192);
    CHECK(summary["verify"]["filesTotal"] == 100000);
    CHECK(summary["verify"]["files"][0]["displayTruncated"] == true);
    CHECK(job.verify["files"][0]["path"].get<std::string>().size() == 5000);
    CHECK(job.verify["files"].back()["path"] == "file-99999");
}
