#include "tc/service/diagnostics.hpp"
#include "tc/service/app_service.hpp"
#include "tc/core/field_registry.hpp"
#include "test_support.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace tc;
using nlohmann::json;
using core::bencode::Value;

TEST_CASE("BEP19 paths encode components once and preserve signed query", "[diagnostics][S01][F09]")
{
    test::TempDir dir;
    auto source = dir.path() / "Release";
    test::write_file(source / "bin" / core::path_from_utf8("файл #1.bin"), 20, 3);
    test::write_file(source / "second.bin", 30, 4);
    for (auto format : {core::TorrentFormat::V1, core::TorrentFormat::V2, core::TorrentFormat::Hybrid}) {
        auto meta = core::Metainfo::parse(test::make_torrent(source, format, 16384));
        auto files = core::metainfo_files(meta);
        auto nested = std::find_if(files.begin(), files.end(), [](auto const& f) { return f.path.size() == 2; });
        REQUIRE(nested != files.end());
        auto resolved = service::resolve_web_seed("https://example.org/base%20path/?token=a%2Fb", meta, *nested);
        CHECK(resolved == "https://example.org:443/base%20path/Release/bin/%D1%84%D0%B0%D0%B9%D0%BB%20%231.bin?token=a%2Fb");
        CHECK_THROWS_AS(service::resolve_web_seed("https://example.org/direct?token=x", meta, *nested), service::ServiceError);
    }
    auto single_path = dir.path() / "file.bin";
    test::write_file(single_path, 10, 1);
    auto single = core::Metainfo::parse(test::make_torrent(single_path, core::TorrentFormat::V2, 16384));
    auto file = core::metainfo_files(single).front();
    CHECK(service::resolve_web_seed("https://example.org/direct?token=a%2Fb", single, file) == "https://example.org:443/direct?token=a%2Fb");
    CHECK(service::resolve_web_seed("https://example.org/base/", single, file) == "https://example.org:443/base/file.bin");
}

TEST_CASE("seed targets sample at most three real files and never request hybrid pads", "[diagnostics][S03][S04][S05]")
{
    test::TempDir dir;
    for (int i = 0; i < 10; ++i) test::write_file(dir.path() / "Release" / (std::to_string(i) + ".bin"), 20000, 1);
    auto meta = core::Metainfo::parse(test::make_torrent(dir.path() / "Release", core::TorrentFormat::Hybrid, 16384));
    auto patched = core::Metainfo::parse(core::apply_outer_edit(meta, {
        {"url-list",Value::string("https://example.org/files/")},
        {"httpseeds",Value::list({Value::string("https://example.org/pieces")})}}));
    auto targets = service::torrent_probe_targets(patched, "web-seeds");
    CHECK(targets.size() <= 4);
    for (auto const& target : targets) if (target.kind == "bep19") {
        CHECK(target.url.find(".pad") == std::string::npos);
        CHECK(target.total_files == 10);
    }
    CHECK(targets.back().kind == "bep17-transport");
    auto v2 = core::Metainfo::parse(test::make_torrent(dir.path() / "Release", core::TorrentFormat::V2, 16384));
    CHECK_THROWS_AS(core::preview_metadata_edit(v2, {{"httpseeds",Value::list({Value::string("https://example.org/pieces")})}}, {}), core::CoreError);
    auto imported = core::Metainfo::parse(core::apply_outer_edit(v2, {{"httpseeds",Value::list({Value::string("https://example.org/pieces")})}}));
    CHECK(service::torrent_probe_targets(imported, "web-seeds").front().kind == "bep17-unsupported-v2");
}

TEST_CASE("opening and planning endpoints performs no probes or real swarm announcement", "[diagnostics][N06]")
{
    test::TempDir dir;
    test::write_file(dir.path() / "data.bin", 20, 3);
    auto bytes = test::make_torrent(dir.path() / "data.bin", core::TorrentFormat::V1, 16384);
    auto meta = core::Metainfo::parse(bytes);
    service::write_file_atomic(dir.path() / "original.torrent", core::apply_outer_edit(meta, {{"announce",Value::string("http://uncontacted.invalid/secret/announce?passkey=private-secret")}}));
    service::AppService app({}, {});
    auto tor = app.open_torrent(dir.path() / "original.torrent");
    CHECK(app.snapshot()["diagnostics"].empty());
    CHECK(app.diagnostic_targets("trackers", tor["id"])["targets"].size() == 1);
    CHECK(app.snapshot()["diagnostics"].empty());
    CHECK(app.diagnostics().catalog()["locallyCheckedAt"].is_null());
    CHECK(app.plan_catalog_apply()["checksum"].is_null());
    CHECK_THROWS_AS(app.apply_catalog("unreviewed", std::nullopt), service::ServiceError);
    app.update_draft({{"private", true}}, std::nullopt);
    CHECK(app.plan_catalog_apply()["privateBlocked"] == true);
}

TEST_CASE("diagnostic runs retain monotonic order bounded history and redact unsupported proxy endpoints", "[diagnostics][N05][U03][U05]")
{
    service::DiagnosticsService service;
    service::NetworkPolicy policy;
    policy.http_proxy = "http://user:private-password@127.0.0.1:9999/";
    std::string last;
    for (int i = 0; i < 12; ++i) {
        last = service.start({{"probe","udp://private-dns.invalid:80/private-secret","tracker",{},0}}, policy);
        for (int tries = 0; tries < 100 && service.page(last, 0, 50)["run"]["state"] == "running"; ++tries)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(service.page(last, 0, 50)["run"]["state"] == "completed");
    }
    auto runs = service.snapshot();
    CHECK(runs.size() == 8);
    CHECK(runs.back()["id"] == last);
    CHECK(runs.back()["sequence"] == "12");
    CHECK(service.page(last, 0, 50).dump().find("private-secret") == std::string::npos);
    CHECK(service.page(last, 0, 50).dump().find("private-password") == std::string::npos);
}
