#include "tc/core/manifest.hpp"
#include "tc/core/native_fs.hpp"
#include "tc/core/torrent_engine.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace tc::core;
namespace fs = std::filesystem;

namespace {

SkippedItem const* skipped(Manifest const& m, std::string const& filename)
{
    for (auto const& s : m.skipped)
        if (to_utf8(s.path.filename()) == filename) return &s;
    return nullptr;
}

bool included(Manifest const& m, std::string const& relative)
{
    return std::any_of(m.entries.begin(), m.entries.end(), [&](ManifestEntry const& e) {
        std::string path;
        for (auto const& c : e.torrent_path) path += (path.empty() ? "" : "/") + c;
        return path == relative;
    });
}

std::vector<ManifestIssue> errors_of(Manifest const& m, ManifestLimits const& limits = {})
{
    std::vector<ManifestIssue> out;
    for (auto const& i : validate_manifest(m, limits))
        if (i.severity == Severity::Error) out.push_back(i);
    return out;
}

} // namespace

TEST_CASE("glob rules match names and relative paths", "[manifest]")
{
    CHECK(glob_match("*.tmp", "a/b/file.tmp"));
    CHECK(glob_match("*.TMP", "file.tmp"));
    CHECK_FALSE(glob_match("*.tmp", "file.tmp.bak"));
    CHECK(glob_match("cache/*", "cache/x.bin"));
    CHECK_FALSE(glob_match("cache/*", "cache/sub/x.bin"));
    CHECK(glob_match("**/cache/**", "a/b/cache/c/d"));
    CHECK(glob_match("**/x.bin", "x.bin"));
    CHECK(glob_match("f?le", "file"));
    CHECK_FALSE(glob_match("a?b", "a/b"));
}

TEST_CASE("default exclusions are visible and do not hide ordinary torrents", "[manifest]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "keep.bin", 10);
    tc::test::write_file(root / "release.torrent", 10);
    tc::test::write_file(root / ".release.torrent.tc-0123abcd.tmp", 10);
    tc::test::write_file(root / "draft.tcproject", 10);
    tc::test::write_file(root / ".hidden", 3);

    Manifest const m = scan_source(root);
    CHECK(included(m, "keep.bin"));
    CHECK(included(m, "release.torrent"));
    CHECK(included(m, ".hidden")); // hidden files are included by default
    REQUIRE(skipped(m, ".release.torrent.tc-0123abcd.tmp") != nullptr);
    CHECK(skipped(m, ".release.torrent.tc-0123abcd.tmp")->kind == SkipKind::Excluded);
    REQUIRE(skipped(m, "draft.tcproject") != nullptr);
    CHECK(skipped(m, "draft.tcproject")->reason == "TorrentControl project file");

    ScanOptions no_defaults;
    no_defaults.use_default_exclusions = false;
    CHECK(scan_source(root, no_defaults).entries.size() == 5);
}

TEST_CASE("user exclusion rules skip files and whole folders", "[manifest]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "a.bin", 1);
    tc::test::write_file(root / "logs" / "x.log", 1);
    tc::test::write_file(root / "cache" / "deep" / "y.bin", 1);

    ScanOptions o;
    o.exclusions = {{"*.log", "log files"}, {"cache", "cache folder"}};
    Manifest const m = scan_source(root, o);
    CHECK(m.entries.size() == 1);
    REQUIRE(skipped(m, "x.log") != nullptr);
    CHECK(skipped(m, "x.log")->reason == "log files");
    REQUIRE(skipped(m, "cache") != nullptr);
    CHECK(skipped(m, "y.bin") == nullptr); // the folder was not entered
}

TEST_CASE("the current output path is excluded from a folder scan", "[manifest][W06]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "a.bin", 1);
    tc::test::write_file(root / "out.torrent", 1);

    ScanOptions o;
    o.excluded_paths = {root / "out.torrent"};
    Manifest const m = scan_source(root, o);
    CHECK(m.entries.size() == 1);
    REQUIRE(skipped(m, "out.torrent") != nullptr);
    CHECK(skipped(m, "out.torrent")->kind == SkipKind::OutputFile);
}

TEST_CASE("non-recursive selections list subfolders as skipped", "[manifest]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "top.bin", 1);
    tc::test::write_file(root / "sub" / "inner.bin", 1);

    ScanOptions o;
    o.recursive = false;
    Manifest const m = scan_source(root, o);
    CHECK(included(m, "top.bin"));
    CHECK_FALSE(included(m, "sub/inner.bin"));
    REQUIRE(skipped(m, "sub") != nullptr);
    CHECK(skipped(m, "sub")->kind == SkipKind::NotRecursive);
}

TEST_CASE("frozen entries record identity and inclusion reason", "[manifest][W03]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "a.bin", 100);
    std::error_code ec;
    fs::create_hard_link(root / "a.bin", root / "b.bin", ec);
    if (ec) SKIP("hard links are not supported here: " << ec.message());

    Manifest const m = scan_source(root);
    REQUIRE(m.entries.size() == 2); // hard links stay separate entries
    CHECK(m.entries[0].observed.identity.valid);
    CHECK(m.entries[0].observed.identity == m.entries[1].observed.identity);
    CHECK(m.entries[0].observed.link_count == 2);
    CHECK(m.entries[0].inclusion_reason == "in selected folder");
    CHECK(m.entries[0].length == 100);
}

TEST_CASE("Unicode case-insensitive collisions are reported", "[manifest][W01]")
{
    CHECK(fold_case("Файл.TXT") == fold_case("файл.txt"));
    CHECK(fold_case("ÉTÉ") == fold_case("été"));
    CHECK(fold_case("Straße") != fold_case("STRASSE")); // no full case folding or normalization
    CHECK(fold_case("e\xcc\x81") != fold_case("\xc3\xa9")); // NFD and NFC stay distinct

    Manifest m;
    m.name = "Root";
    m.entries.push_back(ManifestEntry{"a", "/x", {"Папка", "Файл.txt"}, 1});
    m.entries.push_back(ManifestEntry{"b", "/y", {"папка", "файл.TXT"}, 1});
    auto const errors = errors_of(m);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == ErrorCode::PathCollision);
    CHECK(errors[0].source_id == "b");
}

TEST_CASE("manifest size limits warn and block", "[manifest][P02]")
{
    Manifest m;
    m.name = "Root";
    for (int i = 0; i < 4; ++i) m.entries.push_back(ManifestEntry{"s", "/x", {"f" + std::to_string(i)}, 1});

    auto const warn = validate_manifest(m, ManifestLimits{2, 10});
    REQUIRE(warn.size() == 1);
    CHECK(warn[0].severity == Severity::Warning);
    CHECK(errors_of(m, ManifestLimits{2, 10}).empty());

    auto const block = errors_of(m, ManifestLimits{2, 3});
    REQUIRE(block.size() == 1);
    CHECK(block[0].code == ErrorCode::ResourceLimit);
}

TEST_CASE("destination edits rename torrent paths only", "[manifest]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "b.bin", 1);
    tc::test::write_file(root / "c.bin", 1);
    Manifest m = scan_source(root);
    std::string const id = m.entries[1].source_id; // c.bin

    set_destination(m, id, {"a", "renamed.bin"});
    CHECK(m.revision == 2);
    CHECK(m.entries[0].torrent_path == std::vector<std::string>{"a", "renamed.bin"}); // re-sorted
    CHECK(fs::exists(root / "c.bin")); // the source file is untouched
    CHECK_THROWS_AS(set_destination(m, "nope", {"x"}), CoreError);
}

TEST_CASE("recheck detects modified, replaced and missing sources", "[manifest][W03]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "a.bin", 100, 1);
    tc::test::write_file(root / "b.bin", 100, 2);
    tc::test::write_file(root / "c.bin", 100, 3);
    Manifest const m = scan_source(root);
    CHECK(recheck_sources(m).empty());

    tc::test::touch_later(root / "a.bin");             // same size, new time
    tc::test::write_file(root / "b.tmp", 100, 9);        // same size, new object
    fs::rename(root / "b.tmp", root / "b.bin");
    fs::remove(root / "c.bin");

    auto const issues = recheck_sources(m);
    REQUIRE(issues.size() == 3);
    CHECK(issues[0].code == ErrorCode::SourceChanged);
    CHECK(issues[0].message.find("last write time") != std::string::npos);
    CHECK(issues[1].code == ErrorCode::SourceChanged);
    CHECK(issues[1].message.find("replaced") != std::string::npos);
    CHECK(issues[2].code == ErrorCode::SourceMissing);
}

TEST_CASE("Windows reparse points and cloud placeholders are classified", "[manifest][W02][W05]")
{
    using native::classify_windows;
    using native::EntryKind;
    constexpr std::uint32_t directory = 0x10, reparse = 0x400, offline = 0x1000, recall_on_open = 0x40000,
                            recall_on_data = 0x400000, archive = 0x20;

    CHECK(classify_windows(archive, 0).kind == EntryKind::Regular);
    CHECK(classify_windows(directory, 0).kind == EntryKind::Directory);
    CHECK(classify_windows(directory | reparse, 0xA0000003).kind == EntryKind::Junction);
    CHECK(classify_windows(reparse, 0xA000000C).kind == EntryKind::Symlink);
    CHECK(classify_windows(directory | reparse, 0xA000000C).kind == EntryKind::Symlink);
    CHECK(classify_windows(reparse, 0x80000013).kind == EntryKind::Regular); // deduplicated data is local
    CHECK(classify_windows(reparse, 0x8000001B).kind == EntryKind::OtherReparsePoint);

    // OneDrive placeholder whose data is in the cloud: needs hydration.
    auto const online_only = classify_windows(archive | reparse | recall_on_data, 0x9000101A);
    CHECK(online_only.kind == EntryKind::Regular);
    CHECK(online_only.cloud_placeholder);
    CHECK(online_only.requires_hydration);
    // Locally available cloud file: managed by the provider, readable without download.
    auto const local = classify_windows(archive | reparse, 0x9000001A);
    CHECK(local.cloud_placeholder);
    CHECK_FALSE(local.requires_hydration);
    CHECK(classify_windows(archive | offline, 0).requires_hydration);
    CHECK(classify_windows(directory | reparse | recall_on_open, 0x9000001A).kind == EntryKind::Directory);
}

TEST_CASE("cloud files that need hydration require explicit consent", "[manifest][engine][W05]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "local.bin", 100);
    tc::test::write_file(root / "online.bin", 100);
    Manifest m = scan_source(root);
    m.entries[1].flags.cloud_placeholder = true;
    m.entries[1].flags.requires_hydration = true;

    auto files = make_file_payload_source();
    tc::test::CountingSource counting(*files);
    CreateOptions o;
    try {
        create_torrent(m, o, counting);
        FAIL_CHECK("creation must not start without consent");
    } catch (CoreError const& e) {
        CHECK(e.code() == ErrorCode::HydrationRequired);
        CHECK(e.phase() == Phase::Preflight);
    }
    CHECK(counting.opens.empty()); // nothing was read, nothing was downloaded

    o.allow_hydration = true;
    CreateResult const r = create_torrent(m, o, counting);
    CHECK(r.preflight.files_requiring_hydration == 1);
    CHECK_FALSE(r.preflight.warnings.empty());
}

TEST_CASE("links are not followed by default and followed traversal stays bounded", "[manifest][W02]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "real" / "a.bin", 4);
    tc::test::write_file(dir.path() / "outside" / "secret.bin", 4);
    std::error_code ec;
    fs::create_directory_symlink(root, root / "real" / "loop", ec);
    if (ec) SKIP("cannot create symbolic links here: " << ec.message());
    fs::create_directory_symlink(dir.path() / "outside", root / "external", ec);
    fs::create_symlink(root / "real" / "a.bin", root / "alias.bin", ec);
    REQUIRE_FALSE(ec);

    Manifest const plain = scan_source(root);
    CHECK(plain.entries.size() == 1);
    REQUIRE(skipped(plain, "loop") != nullptr);
    CHECK(skipped(plain, "loop")->kind == SkipKind::SymbolicLink);
    CHECK(skipped(plain, "external") != nullptr);
    CHECK(skipped(plain, "alias.bin") != nullptr);

    ScanOptions follow;
    follow.follow_links = true;
    Manifest const followed = scan_source(root, follow);
    CHECK(included(followed, "real/a.bin"));
    CHECK(included(followed, "alias.bin")); // file link inside the root
    REQUIRE(skipped(followed, "loop") != nullptr);
    CHECK(skipped(followed, "loop")->kind == SkipKind::LinkCycle);
    REQUIRE(skipped(followed, "external") != nullptr);
    CHECK(skipped(followed, "external")->kind == SkipKind::OutsideRoot);
}

TEST_CASE("unreadable selected folders block creation until excluded", "[manifest][W07]")
{
#ifdef _WIN32
    SKIP("permission bits are POSIX-specific");
#else
    if (tc::test::permissions_are_bypassed()) SKIP("running with permission bypass");
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Data";
    tc::test::write_file(root / "ok.bin", 4);
    tc::test::write_file(root / "locked" / "x.bin", 4);
    ::chmod((root / "locked").c_str(), 0);

    Manifest const m = scan_source(root);
    ::chmod((root / "locked").c_str(), 0755);
    REQUIRE(m.unreadable.size() == 1);
    auto const errors = errors_of(m);
    REQUIRE_FALSE(errors.empty());
    CHECK(errors[0].code == ErrorCode::SourceUnreadable);

    ScanOptions o;
    o.exclusions = {{"locked", "excluded by the user"}};
    ::chmod((root / "locked").c_str(), 0);
    Manifest const excluded = scan_source(root, o);
    ::chmod((root / "locked").c_str(), 0755);
    CHECK(excluded.unreadable.empty());
    CHECK(errors_of(excluded).empty());
#endif
}
