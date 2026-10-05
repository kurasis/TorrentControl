#include "tc/core/manifest.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace tc::core;
namespace fs = std::filesystem;

namespace {

Manifest synthetic(std::vector<std::vector<std::string>> paths, std::uint64_t length = 1)
{
    Manifest m;
    m.name = "Root";
    m.mode = LayoutMode::Directory;
    for (auto& p : paths) m.entries.push_back(ManifestEntry{"src", "/nonexistent", std::move(p), length});
    return m;
}

bool has_issue(Manifest const& m, ErrorCode code)
{
    auto const issues = validate_manifest(m);
    return std::any_of(issues.begin(), issues.end(), [&](ManifestIssue const& i) { return i.code == code; });
}

} // namespace

TEST_CASE("valid Unicode destination paths pass validation", "[manifest][W01]")
{
    Manifest const m = synthetic({{"Документы", "файл.txt"}, {"音楽", "🎵 song.flac"}});
    CHECK(validate_manifest(m).empty());
}

TEST_CASE("Windows-invalid names are reported", "[manifest][W01]")
{
    CHECK(has_issue(synthetic({{"CON"}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{"dir", "con.txt"}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{"LPT1"}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{"trailing."}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{"trailing "}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{"a:b"}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{"a\\b"}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{".."}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{""}}), ErrorCode::InvalidPath));
    CHECK(has_issue(synthetic({{"bad\xff" "utf8"}}), ErrorCode::InvalidPath));
    CHECK_FALSE(has_issue(synthetic({{"CONSOLE.txt"}}), ErrorCode::InvalidPath));
}

TEST_CASE("destination collisions are reported before hashing", "[manifest][W01]")
{
    CHECK(has_issue(synthetic({{"a.txt"}, {"a.txt"}}), ErrorCode::PathCollision));
    CHECK(has_issue(synthetic({{"Readme.TXT"}, {"readme.txt"}}), ErrorCode::PathCollision));
    CHECK(has_issue(synthetic({{"x"}, {"x", "y"}}), ErrorCode::PathCollision));
    CHECK(has_issue(synthetic({{"X"}, {"x", "y"}}), ErrorCode::PathCollision));
}

TEST_CASE("empty selections and all-empty payloads are rejected", "[manifest][F08]")
{
    CHECK(has_issue(synthetic({}), ErrorCode::EmptyPayload));
    CHECK(has_issue(synthetic({{"a"}, {"b"}}, 0), ErrorCode::EmptyPayload));
}

TEST_CASE("folder scan maps descendants below the folder name", "[manifest]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Release";
    tc::test::write_file(root / "bin" / "app.exe", 10);
    tc::test::write_file(root / "readme.txt", 3);
    tc::test::write_file(root / "empty.dat", 0);

    Manifest const m = scan_source(root);
    CHECK(m.name == "Release");
    CHECK(m.mode == LayoutMode::Directory);
    REQUIRE(m.entries.size() == 3);
    CHECK(m.torrent_path_string(m.entries[0]) == "Release/bin/app.exe");
    CHECK(m.total_length() == 13);

    Manifest const flat = scan_source(root, ScanOptions{.recursive = false});
    CHECK(flat.entries.size() == 2);
    REQUIRE(flat.skipped.size() == 1);
    CHECK(flat.skipped[0].reason.find("non-recursive") != std::string::npos);
}

TEST_CASE("a selected file produces single-file mode", "[manifest][F09]")
{
    tc::test::TempDir dir;
    tc::test::write_file(dir.path() / "movie.mkv", 7);
    Manifest const m = scan_source(dir.path() / "movie.mkv");
    CHECK(m.mode == LayoutMode::SingleFile);
    CHECK(m.name == "movie.mkv");
    CHECK(m.torrent_path_string(m.entries.at(0)) == "movie.mkv");
}

TEST_CASE("symbolic links are listed as skipped and not followed", "[manifest][W02]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "data";
    tc::test::write_file(root / "real.bin", 4);
    tc::test::write_file(dir.path() / "outside" / "secret.bin", 4);
    std::error_code ec;
    fs::create_directory_symlink(dir.path() / "outside", root / "link", ec);
    if (ec) SKIP("cannot create symbolic links here: " << ec.message());

    Manifest const m = scan_source(root);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].torrent_path == std::vector<std::string>{"real.bin"});
    REQUIRE(m.skipped.size() == 1);
    CHECK(m.skipped[0].path.filename() == "link");
}
