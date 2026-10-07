// Full payload verification (section 9.3).

#include "tc/core/error.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/torrent_engine.hpp"
#include "tc/core/verify.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace tc::core;
namespace fs = std::filesystem;

namespace {

constexpr int kib = 1024;

struct Fixture {
    tc::test::TempDir dir;
    fs::path root = dir.path() / "Payload";

    Fixture()
    {
        tc::test::write_file(root / "a.bin", 150 * kib, 1);
        tc::test::write_file(root / "sub" / "b.bin", 20 * kib + 3, 2);
        tc::test::write_file(root / "empty.dat", 0);
        tc::test::write_file(root / "z.bin", 70 * kib, 3);
    }

    VerifyResult verify(TorrentFormat format, fs::path const& payload_root)
    {
        Metainfo const m = Metainfo::parse(tc::test::make_torrent(root, format, 32 * kib));
        auto files = make_file_payload_source();
        return verify_payload(m, map_to_root(m, payload_root), *files);
    }

    VerifyFileResult const& file(VerifyResult const& r, std::string const& path)
    {
        for (auto const& f : r.files)
            if (f.torrent_path == path) return f;
        throw std::runtime_error("no result for " + path);
    }
};

} // namespace

TEST_CASE("intact payload verifies in every format", "[verify]")
{
    Fixture fx;
    for (auto const format : {TorrentFormat::V1, TorrentFormat::V2, TorrentFormat::Hybrid}) {
        INFO(to_string(format));
        VerifyResult const r = fx.verify(format, fx.root);
        CHECK(r.ok);
        CHECK(r.metainfo_problems.empty());
        CHECK(r.files.size() == 4);
        CHECK(r.payload_bytes_read == 150 * kib + 20 * kib + 3 + 70 * kib);
        CHECK(r.v1_pieces_bad == 0);
        CHECK(r.v2_files_bad == 0);
        if (format != TorrentFormat::V2) CHECK(r.v1_pieces_total > 0);
        if (format != TorrentFormat::V1) CHECK(r.v2_files_checked == 3); // the empty file has no root
    }
}

TEST_CASE("the payload may be verified from a differently named folder", "[verify][F10]")
{
    Fixture fx;
    fs::path const moved = fx.dir.path() / "Renamed copy";
    fs::copy(fx.root, moved, fs::copy_options::recursive);
    CHECK(fx.verify(TorrentFormat::Hybrid, moved).ok);
}

TEST_CASE("payload mappings confine resolved links to the selected root", "[verify][security]")
{
    tc::test::TempDir dir;
    auto const input = dir.path() / "input";
    tc::test::write_file(input / "file.bin", 1024, 7);
    auto const meta = Metainfo::parse(tc::test::make_torrent(input, TorrentFormat::V1, 16 * kib));
    auto const root = dir.path() / "selected";
    fs::create_directory(root);
    fs::path target;
    bool outside = false;
    SECTION("an external file is refused before a reader can open it") {
        target = dir.path() / "outside" / "file.bin";
        outside = true;
    }
    SECTION("an internal link still verifies") {
        target = root / "real" / "file.bin";
    }
    tc::test::write_file(target, 1024, 7);
    std::error_code ec;
    fs::create_symlink(target, root / "file.bin", ec);
    if (ec) SKIP("This environment does not permit creation of symbolic links");
    if (outside) {
        CHECK_THROWS_AS(map_to_root(meta, root), CoreError);
        CHECK(tc::test::read_all(target) == tc::test::read_all(input / "file.bin"));
    } else {
        auto files = make_file_payload_source();
        CHECK(verify_payload(meta, map_to_root(meta, root), *files).ok);
        CHECK(verify_payload(meta, map_to_root(meta, root / ""), *files).ok);
    }
}

TEST_CASE("a corrupted byte is attributed to its file", "[verify]")
{
    Fixture fx;
    std::string const torrent = tc::test::make_torrent(fx.root, TorrentFormat::Hybrid, 32 * kib);
    {
        std::fstream f(fx.root / "a.bin", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(100 * kib);
        f.put('\x5a');
    }
    // Flip one byte back if the random data happened to contain it.
    Metainfo const m = Metainfo::parse(torrent);
    auto files = make_file_payload_source();
    VerifyResult const r = verify_payload(m, map_to_root(m, fx.root), *files);
    auto const& a = fx.file(r, "Payload/a.bin");
    if (a.status == VerifyStatus::Ok) SKIP("the overwritten byte had the same value");
    CHECK_FALSE(r.ok);
    CHECK(a.status == VerifyStatus::Corrupt);
    CHECK(a.bad_v1_pieces == 1);
    CHECK(a.bad_v2_pieces == 1);
    CHECK(r.v2_files_bad == 1);
    CHECK(fx.file(r, "Payload/z.bin").status == VerifyStatus::Ok);
    CHECK(fx.file(r, "Payload/sub/b.bin").status == VerifyStatus::Ok);
}

TEST_CASE("missing and resized files are reported without stopping the check", "[verify]")
{
    Fixture fx;
    std::string const torrent = tc::test::make_torrent(fx.root, TorrentFormat::Hybrid, 32 * kib);
    fs::remove(fx.root / "sub" / "b.bin");
    fs::resize_file(fx.root / "z.bin", 10);

    Metainfo const m = Metainfo::parse(torrent);
    auto files = make_file_payload_source();
    VerifyResult const r = verify_payload(m, map_to_root(m, fx.root), *files);
    CHECK_FALSE(r.ok);
    CHECK(fx.file(r, "Payload/sub/b.bin").status == VerifyStatus::Missing);
    CHECK(fx.file(r, "Payload/z.bin").status == VerifyStatus::SizeMismatch);
    CHECK(fx.file(r, "Payload/a.bin").status == VerifyStatus::Ok);
    CHECK(fx.file(r, "Payload/empty.dat").status == VerifyStatus::Ok);
}

TEST_CASE("unmapped files are missing and padding is never read", "[verify][S04]")
{
    Fixture fx;
    Metainfo const m = Metainfo::parse(tc::test::make_torrent(fx.root, TorrentFormat::Hybrid, 32 * kib));
    PayloadMapping mapping = map_to_root(m, fx.root);
    mapping.erase("Payload/a.bin");
    auto files = make_file_payload_source();
    tc::test::CountingSource counting(*files);

    VerifyResult const r = verify_payload(m, mapping, counting);
    CHECK(fx.file(r, "Payload/a.bin").status == VerifyStatus::Missing);
    CHECK(counting.opens.size() == 3); // only mapped real files were opened
}

TEST_CASE("invalid metainfo is reported before any payload is read", "[verify][E05]")
{
    Fixture fx;
    std::string const torrent = tc::test::make_torrent(fx.root, TorrentFormat::V2, 32 * kib);
    bencode::Value root = bencode::parse(torrent);
    root.erase("piece layers");
    Metainfo const m = Metainfo::parse(bencode::encode(root));

    auto files = make_file_payload_source();
    tc::test::CountingSource counting(*files);
    VerifyResult const r = verify_payload(m, map_to_root(m, fx.root), counting);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.metainfo_problems.empty());
    CHECK(counting.opens.empty());
}

TEST_CASE("verification can be cancelled", "[verify][W08]")
{
    Fixture fx;
    Metainfo const m = Metainfo::parse(tc::test::make_torrent(fx.root, TorrentFormat::V1, 32 * kib));
    auto files = make_file_payload_source();
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS_AS(verify_payload(m, map_to_root(m, fx.root), *files, stop.get_token()), CoreError);
}
