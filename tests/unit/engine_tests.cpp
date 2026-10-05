#include "tc/core/error.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/torrent_engine.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <fstream>

using namespace tc::core;
namespace fs = std::filesystem;

namespace {

constexpr int kib = 1024;

std::optional<ErrorCode> create_error(Manifest const& m, CreateOptions const& o, PayloadSource& src, std::stop_token stop = {},
    ProgressCallback const& cb = {})
{
    try {
        create_torrent(m, o, src, stop, cb);
    } catch (CoreError const& e) {
        return e.code();
    }
    return std::nullopt; // no error: compares unequal to every expected code
}

} // namespace

TEST_CASE("hybrid creation reads each payload byte exactly once", "[engine][P01][F05]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "Mixed";
    for (int i = 0; i < 20; ++i) tc::test::write_file(root / "tiny" / ("f" + std::to_string(i)), static_cast<std::uint64_t>(i + 1), static_cast<std::uint32_t>(i));
    tc::test::write_file(root / "large.bin", 3 * 1024 * 1024 + 123, 99);
    tc::test::write_file(root / "empty", 0);

    Manifest const m = scan_source(root);
    auto files = make_file_payload_source();
    tc::test::CountingSource counting(*files);

    CreateOptions o;
    o.format = TorrentFormat::Hybrid;
    o.piece_length = 64 * kib;
    o.read_buffer_size = 100 * kib; // deliberately not aligned to blocks or pieces

    CreateProgress last;
    int events = 0;
    CreateResult const r = create_torrent(m, o, counting, {}, [&](CreateProgress const& p) {
        last = p;
        ++events;
    });

    for (auto const& e : m.entries) {
        INFO(m.torrent_path_string(e));
        CHECK(counting.opens[e.source_id] == 1);
        CHECK(counting.bytes_read[e.source_id] == e.length);
    }
    CHECK(last.payload_bytes_read == m.total_length());
    CHECK(last.files_completed == m.entries.size());
    CHECK(last.padding_bytes_total > 0);
    CHECK(last.padding_bytes_processed == last.padding_bytes_total);
    CHECK(events > 2);
    CHECK(r.format == MetainfoFormat::Hybrid);
    CHECK(r.info_hashes.v1.has_value());
    CHECK(r.info_hashes.v2.has_value());
    CHECK(r.padding_bytes == last.padding_bytes_total);
}

TEST_CASE("pure v2 output has no padding and no v1 pieces", "[engine][F03]")
{
    tc::test::TempDir dir;
    fs::path const root = dir.path() / "v2";
    for (std::uint64_t size : {0ull, 1ull, 16383ull, 16384ull, 16385ull, 65535ull, 65536ull, 65537ull})
        tc::test::write_file(root / ("s" + std::to_string(size)), size, static_cast<std::uint32_t>(size));

    auto files = make_file_payload_source();
    CreateOptions o;
    o.format = TorrentFormat::V2;
    o.piece_length = 64 * kib;
    CreateResult const r = create_torrent(scan_source(root), o, *files);
    Metainfo const m = Metainfo::parse(r.torrent_bytes);
    CHECK(m.format() == MetainfoFormat::V2);
    CHECK(m.info().find("pieces") == nullptr);
    CHECK(r.padding_bytes == 0);
    CHECK(m.root().find("piece layers") != nullptr);
}

TEST_CASE("cancellation stops reading and produces no result", "[engine][W08]")
{
    tc::test::TempDir dir;
    tc::test::write_file(dir.path() / "big.bin", 8 * 1024 * 1024);
    Manifest const m = scan_source(dir.path() / "big.bin");
    auto files = make_file_payload_source();
    tc::test::CountingSource counting(*files);

    std::stop_source stop;
    CreateOptions o;
    o.read_buffer_size = 256 * kib;
    auto cb = [&](CreateProgress const& p) {
        if (p.payload_bytes_read >= 512 * kib) stop.request_stop();
    };
    CHECK(create_error(m, o, counting, stop.get_token(), cb) == ErrorCode::Cancelled);
    CHECK(counting.bytes_read[m.entries[0].source_id] < 8 * 1024 * 1024);
}

TEST_CASE("source changes after manifest freeze block success", "[engine][W03]")
{
    tc::test::TempDir dir;
    fs::path const file = dir.path() / "data.bin";
    tc::test::write_file(file, 100 * kib);
    Manifest const m = scan_source(file);
    auto files = make_file_payload_source();

    SECTION("file grew")
    {
        std::ofstream(file, std::ios::binary | std::ios::app).write("more", 4);
        CHECK(create_error(m, {}, *files) == ErrorCode::SourceChanged);
    }
    SECTION("file shrank")
    {
        fs::resize_file(file, 10);
        CHECK(create_error(m, {}, *files) == ErrorCode::SourceChanged);
    }
    SECTION("file removed")
    {
        fs::remove(file);
        CHECK(create_error(m, {}, *files) == ErrorCode::SourceMissing);
    }
}

TEST_CASE("virtual collection maps unrelated sources without copying", "[engine][F10]")
{
    tc::test::TempDir dir;
    tc::test::write_file(dir.path() / "volume-a" / "x" / "photo.jpg", 70 * kib, 1);
    tc::test::write_file(dir.path() / "volume-b" / "deep" / "notes.txt", 5, 2);

    Manifest const m = ManifestBuilder("Collection")
                           .add_file(dir.path() / "volume-a" / "x" / "photo.jpg", {"Pictures", "photo.jpg"})
                           .add_file(dir.path() / "volume-b" / "deep" / "notes.txt", {"notes.txt"})
                           .build();
    auto files = make_file_payload_source();
    CreateResult const r = create_torrent(m, {}, *files);
    Metainfo const meta = Metainfo::parse(r.torrent_bytes);
    CHECK(meta.name() == "Collection");
    bencode::Value const* tree = meta.info().find("file tree");
    REQUIRE(tree != nullptr);
    CHECK(tree->find("Pictures") != nullptr);
    CHECK(tree->find("notes.txt") != nullptr);
    CHECK_FALSE(fs::exists(dir.path() / "Collection"));
}

TEST_CASE("Unicode names and paths longer than MAX_PATH", "[engine][W01]")
{
    tc::test::TempDir dir;
    fs::path deep = dir.path() / u8"Набор данных";
    for (int i = 0; i < 12; ++i) deep /= fs::path(u8"очень-длинное-имя-папки-") += std::to_string(i);
    fs::path const file = deep / u8"файл 🎵.bin";
    REQUIRE(file.native().size() > 260);
    tc::test::write_file(file, 40 * kib);

    auto files = make_file_payload_source();
    CreateResult const r = create_torrent(scan_source(dir.path() / u8"Набор данных"), {}, *files);
    Metainfo const meta = Metainfo::parse(r.torrent_bytes);
    CHECK(meta.name() == "Набор данных");
}

TEST_CASE("reproducible configuration yields identical bytes", "[engine][E08]")
{
    tc::test::TempDir dir;
    tc::test::write_file(dir.path() / "r" / "a", 33 * kib, 5);
    tc::test::write_file(dir.path() / "r" / "b", 1, 6);
    auto files = make_file_payload_source();
    CreateOptions o;
    o.creator = "TorrentControl test";
    o.creation_date = std::nullopt;
    std::string const first = create_torrent(scan_source(dir.path() / "r"), o, *files).torrent_bytes;
    std::string const second = create_torrent(scan_source(dir.path() / "r"), o, *files).torrent_bytes;
    CHECK(first == second);
    CHECK(Metainfo::parse(first).root().find("creation date") == nullptr);
}

TEST_CASE("metadata options are serialized", "[engine]")
{
    tc::test::TempDir dir;
    tc::test::write_file(dir.path() / "p.bin", 1000);
    auto files = make_file_payload_source();
    CreateOptions o;
    o.private_flag = true;
    o.tracker_tiers = {{"https://tracker.example/announce/SECRET"}, {}, {"udp://b.example:6969/announce"}};
    o.comment = "hello";
    o.creation_date = 1700000000;
    Metainfo const m = Metainfo::parse(create_torrent(scan_source(dir.path() / "p.bin"), o, *files).torrent_bytes);
    CHECK(m.info().find("private")->text() == "1");
    CHECK(m.root().find("announce")->text() == "https://tracker.example/announce/SECRET");
    auto const& tiers = m.root().find("announce-list")->items();
    REQUIRE(tiers.size() == 2);
    CHECK(tiers[1].items().at(0).text() == "udp://b.example:6969/announce");
    CHECK(m.root().find("comment")->text() == "hello");
    CHECK(m.root().find("creation date")->text() == "1700000000");
}

TEST_CASE("empty payloads are rejected with a specific error", "[engine][F08]")
{
    tc::test::TempDir dir;
    tc::test::write_file(dir.path() / "e" / "a", 0);
    auto files = make_file_payload_source();
    CHECK(create_error(scan_source(dir.path() / "e"), {}, *files) == ErrorCode::EmptyPayload);
}

TEST_CASE("automatic piece size follows policy version 1", "[engine][F11]")
{
    auto synthetic = [](std::size_t count, std::uint64_t length) {
        Manifest m;
        m.name = "Synthetic";
        for (std::size_t i = 0; i < count; ++i)
            m.entries.push_back(ManifestEntry{"s", "/none", {"f" + std::to_string(i)}, length});
        return m;
    };

    PieceSizeDecision const small = choose_piece_size(synthetic(1, 1024 * 1024), TorrentFormat::Hybrid);
    CHECK(small.piece_length == 256 * kib);
    CHECK_FALSE(small.exceeds_piece_count_target);

    PieceSizeDecision const big = choose_piece_size(synthetic(1, 64ull * 1024 * 1024 * 1024), TorrentFormat::V1);
    CHECK(big.piece_length == 2 * 1024 * 1024);
    CHECK(big.logical_pieces <= 32768);

    // Many tiny files: every candidate pads each file to a full piece, so the
    // policy terminates on the largest candidate and reports the padding.
    PieceSizeDecision const tiny = choose_piece_size(synthetic(40000, 10), TorrentFormat::Hybrid);
    CHECK(tiny.piece_length == 16 * 1024 * 1024);
    CHECK(tiny.exceeds_piece_count_target);
    CHECK(tiny.padding_warning);

    PieceSizeDecision const tiny_v1 = choose_piece_size(synthetic(40000, 10), TorrentFormat::V1);
    CHECK(tiny_v1.piece_length == 256 * kib);
    CHECK_FALSE(tiny_v1.padding_warning);
}
