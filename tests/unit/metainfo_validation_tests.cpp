// Metainfo validation, info edits, signatures and legacy formats (E04-E07).

#include "tc/core/bencode.hpp"
#include "tc/core/error.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/torrent_engine.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>

using namespace tc::core;
namespace fs = std::filesystem;

namespace {

constexpr int kib = 1024;

std::string sample_torrent(TorrentFormat format, tc::test::TempDir const& dir)
{
    fs::path const root = dir.path() / "Sample";
    tc::test::write_file(root / "big.bin", 200 * kib, 1);
    tc::test::write_file(root / "small.txt", 1000, 2);
    tc::test::write_file(root / "copy.bin", 200 * kib, 1); // identical to big.bin
    return tc::test::make_torrent(root, format, 32 * kib);
}

// Re-encodes `bytes` after `mutate` changes the decoded root.
template <typename F>
std::string mutated(std::string const& bytes, F mutate)
{
    bencode::Value root = bencode::parse(bytes);
    mutate(root);
    return bencode::encode(root);
}

bencode::Value& child(bencode::Value& dict, std::string_view key)
{
    bencode::Value* v = dict.find(key);
    if (v == nullptr) throw std::runtime_error("missing key");
    return *v;
}

bool mentions(std::vector<std::string> const& problems, std::string_view text)
{
    return std::any_of(problems.begin(), problems.end(), [&](std::string const& p) { return p.find(text) != std::string::npos; });
}

std::optional<ErrorCode> parse_error(std::string bytes)
{
    try {
        Metainfo::parse(std::move(bytes));
    } catch (CoreError const& e) {
        return e.code();
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("generated torrents validate in every format", "[metainfo][F06]")
{
    tc::test::TempDir dir;
    for (auto const format : {TorrentFormat::V1, TorrentFormat::V2, TorrentFormat::Hybrid}) {
        INFO(to_string(format));
        Metainfo const m = Metainfo::parse(sample_torrent(format, dir));
        CHECK(validate_metainfo(m).empty());
        auto const files = metainfo_files(m);
        auto const real = std::count_if(files.begin(), files.end(), [](auto const& f) { return !f.pad; });
        CHECK(real == 3);
        if (format != TorrentFormat::V1) {
            // Identical files share one piece-layer entry; no duplicate keys.
            CHECK(m.root().find("piece layers")->entries().size() == 1);
        }
    }
}

TEST_CASE("a mutated piece layer is rejected although the infohash is unchanged", "[metainfo][E05]")
{
    tc::test::TempDir dir;
    std::string const original = sample_torrent(TorrentFormat::Hybrid, dir);
    std::string const broken = mutated(original, [](bencode::Value& root) {
        auto& layers = child(root, "piece layers");
        auto& layer = child(layers, layers.entries().front().key);
        layer = bencode::Value::string(std::string(layer.text().size(), '\x42'));
    });
    Metainfo const a = Metainfo::parse(original);
    Metainfo const b = Metainfo::parse(broken);
    CHECK(a.info_hashes().v1 == b.info_hashes().v1);
    CHECK(a.info_hashes().v2 == b.info_hashes().v2);
    CHECK(mentions(validate_metainfo(b), "does not match the pieces root"));

    std::string const missing = mutated(original, [](bencode::Value& root) { root.erase("piece layers"); });
    CHECK(mentions(validate_metainfo(Metainfo::parse(missing)), "piece layer missing"));

    std::string const extra = mutated(original, [](bencode::Value& root) {
        child(root, "piece layers").set(std::string(32, '\x01'), bencode::Value::string(std::string(64, '\x02')));
    });
    CHECK(mentions(validate_metainfo(Metainfo::parse(extra)), "matches no file"));
}

TEST_CASE("inconsistent or unsafe layouts are invalid", "[metainfo][E03]")
{
    tc::test::TempDir dir;
    std::string const hybrid = sample_torrent(TorrentFormat::Hybrid, dir);

    std::string const mismatch = mutated(hybrid, [](bencode::Value& root) {
        auto& files = child(child(root, "info"), "files");
        files.items().front().set("length", bencode::Value::integer(1));
    });
    CHECK(mentions(validate_metainfo(Metainfo::parse(mismatch)), "hybrid v1 and v2 file lists"));

    std::string const v1 = sample_torrent(TorrentFormat::V1, dir);
    std::string const traversal = mutated(v1, [](bencode::Value& root) {
        auto& files = child(child(root, "info"), "files");
        files.items().front().set("path", bencode::Value::list({bencode::Value::string(".."), bencode::Value::string("x")}));
    });
    CHECK(mentions(validate_metainfo(Metainfo::parse(traversal)), "unsafe path"));

    std::string const short_pieces = mutated(v1, [](bencode::Value& root) {
        auto& info = child(root, "info");
        info.set("pieces", bencode::Value::string(info.find("pieces")->text().substr(20)));
    });
    CHECK(mentions(validate_metainfo(Metainfo::parse(short_pieces)), "the layout needs"));
}

TEST_CASE("info edits change identifiers but reuse payload hashes", "[metainfo][E04]")
{
    tc::test::TempDir dir;
    std::string const bytes = sample_torrent(TorrentFormat::Hybrid, dir);
    Metainfo const original = Metainfo::parse(bytes);

    InfoEditResult const r = apply_info_edit(original, {{"source", bencode::Value::string("TRACKER-X")},
                                                           {"private", bencode::Value::integer(1)}});
    Metainfo const edited = Metainfo::parse(r.bytes);
    CHECK(r.old_hashes.v1 == original.info_hashes().v1);
    CHECK(r.new_hashes.v1 != r.old_hashes.v1);
    CHECK(r.new_hashes.v2 != r.old_hashes.v2);
    CHECK(edited.info().find("pieces")->text() == original.info().find("pieces")->text());
    CHECK(edited.info().find("file tree")->raw().size == original.info().find("file tree")->raw().size);
    CHECK(edited.info().find("source")->text() == "TRACKER-X");
    CHECK(validate_metainfo(edited).empty());

    InfoEditResult const back = apply_info_edit(edited, {{"source", std::nullopt}, {"private", std::nullopt}});
    CHECK(back.new_hashes.v1 == original.info_hashes().v1); // canonical info round-trips

    CHECK_THROWS_AS(apply_info_edit(original, {{"name", bencode::Value::string("x")}}), CoreError);
    CHECK_THROWS_AS(apply_info_edit(original, {{"piece length", bencode::Value::integer(1)}}), CoreError);
    CHECK_THROWS_AS(apply_info_edit(original, {{"private", bencode::Value::integer(0)}}), CoreError);
}

TEST_CASE("signatures and vendor fields are preserved or explicitly removed", "[metainfo][E06]")
{
    tc::test::TempDir dir;
    std::string const plain = sample_torrent(TorrentFormat::V1, dir);
    std::string const signed_bytes = mutated(plain, [](bencode::Value& root) {
        bencode::Value signer = bencode::Value::dictionary(
            {{"certificate", bencode::Value::string(std::string("\x30\x82\x01\x0a", 4))},
                {"signature", bencode::Value::string(std::string(64, '\x07'))}});
        root.set("signatures", bencode::Value::dictionary({{"example.org", std::move(signer)}}));
        root.set("azureus_properties", bencode::Value::dictionary({{"dht_backup_enable", bencode::Value::integer(1)}}));
        root.set("comment.utf-8", bencode::Value::string("Комментарий"));
        child(root, "info").set("name.utf-8", bencode::Value::string("Sample"));
    });
    Metainfo const original = Metainfo::parse(signed_bytes);
    auto raw_value = [](Metainfo const& m, std::string_view key) {
        auto const r = m.root().find(key)->raw();
        return m.bytes().substr(r.offset, r.size);
    };

    // Outer edit: info is untouched, so signatures and vendor fields stay byte-identical.
    Metainfo const outer = Metainfo::parse(apply_outer_edit(original, {{"comment", bencode::Value::string("new")}}));
    CHECK(outer.raw_info() == original.raw_info());
    CHECK(raw_value(outer, "signatures") == raw_value(original, "signatures"));
    CHECK(raw_value(outer, "azureus_properties") == raw_value(original, "azureus_properties"));
    CHECK(raw_value(outer, "comment.utf-8") == raw_value(original, "comment.utf-8"));

    // Info edit: signatures cover info and are never silently kept or dropped.
    CHECK_THROWS_AS(apply_info_edit(original, {{"source", bencode::Value::string("x")}}), CoreError);
    InfoEditOptions remove;
    remove.remove_invalidated_signatures = true;
    InfoEditResult const r = apply_info_edit(original, {{"source", bencode::Value::string("x")}}, remove);
    Metainfo const edited = Metainfo::parse(r.bytes);
    CHECK(r.removed_signatures);
    CHECK(edited.root().find("signatures") == nullptr);
    CHECK(raw_value(edited, "azureus_properties") == raw_value(original, "azureus_properties"));
    CHECK(edited.info().find("name.utf-8")->text() == "Sample");
}

TEST_CASE("future and legacy formats are labelled, not misreported", "[metainfo][E07]")
{
    std::string const bep30 = "d4:infod6:lengthi10e4:name1:a12:piece lengthi16384e9:root hash20:aaaaaaaaaaaaaaaaaaaaee";
    CHECK(parse_error(bep30) == ErrorCode::UnsupportedFormat);

    std::string const with_pieces =
        "d4:infod6:lengthi10e4:name1:a12:piece lengthi16384e6:pieces20:bbbbbbbbbbbbbbbbbbbb9:root hash20:aaaaaaaaaaaaaaaaaaaaee";
    Metainfo const m = Metainfo::parse(with_pieces);
    CHECK(m.format() == MetainfoFormat::V1);
    CHECK(m.has_legacy_root_hash());

    std::string const v3 = "d4:infod9:file treede12:meta versioni3e4:name1:a12:piece lengthi16384eee";
    CHECK(parse_error(v3) == ErrorCode::UnsupportedFormat);
}
