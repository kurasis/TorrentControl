// Output commit (section 14.1) and output-versus-source conflicts (W06, W07).

#include "tc/core/error.hpp"
#include "tc/core/manifest.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/output.hpp"
#include "tc/core/torrent_engine.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace tc::core;
namespace fs = std::filesystem;

namespace {

std::string const valid = "d4:infod6:lengthi1e4:name1:a12:piece lengthi16384e6:pieces20:aaaaaaaaaaaaaaaaaaaaee";
std::string const other = "d4:infod6:lengthi2e4:name1:b12:piece lengthi16384e6:pieces20:bbbbbbbbbbbbbbbbbbbbee";

std::optional<CoreError> commit_failure(fs::path const& out, std::string_view bytes, CommitOptions const& o = {})
{
    try {
        commit_output(out, bytes, o);
    } catch (CoreError const& e) {
        return e;
    }
    return std::nullopt;
}

std::size_t temp_files(fs::path const& dir)
{
    std::size_t n = 0;
    for (auto const& e : fs::directory_iterator(dir))
        if (glob_match(".*.tc-*.tmp", to_utf8(e.path().filename()))) ++n;
    return n;
}

} // namespace

TEST_CASE("output is written through a validated temporary file", "[output]")
{
    tc::test::TempDir dir;
    fs::path const out = dir.path() / "new.torrent";
    std::vector<CommitStage> stages;
    CommitOptions o;
    o.on_stage = [&](CommitStage s) {
        stages.push_back(s);
        CHECK_FALSE(fs::exists(out)); // nothing is published before the rename
        CHECK(temp_files(dir.path()) == 1);
    };
    CommitResult const r = commit_output(out, valid, o);
    CHECK(r.path == fs::absolute(out).lexically_normal());
    CHECK_FALSE(r.replaced_existing);
    CHECK(tc::test::read_all(out) == valid);
    CHECK(stages == std::vector<CommitStage>{CommitStage::TempWritten, CommitStage::BeforeCommit});
    CHECK(temp_files(dir.path()) == 0);
}

TEST_CASE("an existing output is replaced only by explicit choice", "[output][W07]")
{
    tc::test::TempDir dir;
    fs::path const out = dir.path() / "release.torrent";
    tc::test::write_bytes(out, other);

    auto const e = commit_failure(out, valid);
    REQUIRE(e);
    CHECK(e->code() == ErrorCode::OutputConflict);
    CHECK(e->phase() == Phase::Committing);
    CHECK(tc::test::read_all(out) == other);

    CommitOptions replace;
    replace.replace_existing = true;
    CHECK(commit_output(out, valid, replace).replaced_existing);
    CHECK(tc::test::read_all(out) == valid);
    CHECK(temp_files(dir.path()) == 0);
}

TEST_CASE("an output created by another process during the job is not overwritten", "[output][W07]")
{
    tc::test::TempDir dir;
    fs::path const out = dir.path() / "race.torrent";
    CommitOptions o;
    o.on_stage = [&](CommitStage s) {
        if (s == CommitStage::BeforeCommit) tc::test::write_bytes(out, other);
    };
    auto const e = commit_failure(out, valid, o);
    REQUIRE(e);
    CHECK(e->code() == ErrorCode::OutputConflict);
    CHECK(tc::test::read_all(out) == other);
    CHECK(temp_files(dir.path()) == 0);
}

TEST_CASE("a write failure keeps the previous output and removes the temporary file", "[output][W07]")
{
    tc::test::TempDir dir;
    fs::path const out = dir.path() / "keep.torrent";
    tc::test::write_bytes(out, other);
    CommitOptions o;
    o.replace_existing = true;
    o.on_stage = [](CommitStage s) {
        // Simulates ERROR_DISK_FULL / ENOSPC after the temporary file exists.
        if (s == CommitStage::TempWritten) throw CoreError(ErrorCode::OutputWriteFailed, "disk full", 28);
    };
    auto const e = commit_failure(out, valid, o);
    REQUIRE(e);
    CHECK(e->code() == ErrorCode::OutputWriteFailed);
    CHECK(e->os_error() == 28);
    CHECK(tc::test::read_all(out) == other);
    CHECK(temp_files(dir.path()) == 0);
}

TEST_CASE("invalid metainfo is never written", "[output]")
{
    tc::test::TempDir dir;
    fs::path const out = dir.path() / "bad.torrent";
    auto const e = commit_failure(out, "d4:infoi1ee");
    REQUIRE(e);
    CHECK(e->code() == ErrorCode::InvalidMetainfo);
    CHECK_FALSE(fs::exists(out));
    CHECK(temp_files(dir.path()) == 0);
}

TEST_CASE("a missing destination folder fails cleanly", "[output][W07]")
{
    tc::test::TempDir dir;
    auto const e = commit_failure(dir.path() / "no" / "such" / "x.torrent", valid);
    REQUIRE(e);
    CHECK(e->code() == ErrorCode::OutputWriteFailed);
}

TEST_CASE("access denied is a write failure, not a success", "[output][W07]")
{
#ifdef _WIN32
    SKIP("permission bits are POSIX-specific");
#else
    if (tc::test::permissions_are_bypassed()) SKIP("running with permission bypass");
    tc::test::TempDir dir;
    fs::path const locked = dir.path() / "locked";
    fs::create_directories(locked);
    ::chmod(locked.c_str(), 0555);
    auto const e = commit_failure(locked / "x.torrent", valid);
    ::chmod(locked.c_str(), 0755);
    REQUIRE(e);
    CHECK(e->code() == ErrorCode::OutputWriteFailed);
    CHECK(e->os_error().has_value());
    CHECK_FALSE(fs::exists(locked / "x.torrent"));
#endif
}

TEST_CASE("the output may not be a source file or its hard-link alias", "[output][W06]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "a.bin", 100, 1);
    tc::test::write_file(root / "b.bin", 100, 2);
    Manifest const m = scan_source(root);

    SECTION("same path")
    {
        try {
            check_output_target(root / "b.bin", m);
            FAIL_CHECK("expected a conflict");
        } catch (CoreError const& e) {
            CHECK(e.code() == ErrorCode::OutputConflict);
            CHECK(e.source_id() == m.entries[1].source_id);
        }
        CommitOptions o;
        o.replace_existing = true;
        o.manifest = &m;
        CHECK(commit_failure(root / "b.bin", valid, o)->code() == ErrorCode::OutputConflict);
        CHECK(tc::test::read_all(root / "b.bin").size() == 100);
    }
    SECTION("hard-link alias")
    {
        fs::path const alias = dir.path() / "alias.torrent";
        std::error_code ec;
        fs::create_hard_link(root / "a.bin", alias, ec);
        if (ec) SKIP("hard links are not supported here: " << ec.message());
        CHECK_THROWS_AS(check_output_target(alias, m), CoreError);
    }
    SECTION("unrelated output")
    {
        CHECK_NOTHROW(check_output_target(dir.path() / "out.torrent", m));
    }
    SECTION("single-file selection of the output itself")
    {
        Manifest const single = scan_source(root / "a.bin", ScanOptions{.excluded_paths = {root / "a.bin"}});
        REQUIRE(single.entries.size() == 1); // explicitly selected, so not silently excluded
        CHECK_THROWS_AS(check_output_target(root / "a.bin", single), CoreError);
    }
}
