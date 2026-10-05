#include "tc/core/field_registry.hpp"
#include "tc/core/error.hpp"
#include "test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <set>

using namespace tc::core;
using bencode::Value;
using namespace std::string_literals;

namespace {
Metainfo imported(bool signed_torrent = false)
{
    // Valid v1 layout with deliberately noncanonical info key order.
    std::string info = "d4:name5:a.txt6:lengthi5e12:piece lengthi16384e6:pieces20:AAAAAAAAAAAAAAAAAAAA7:privatei0e6:source3:old5:x-bin2:\x00\xff" "e"s;
    return Metainfo::parse("d7:comment3:old4:info" + info
        + (signed_torrent ? "10:signaturesd1:xd9:signature3:abcee" : "")
        + "8:x-vendori99999999999999999999999ee");
}
}

TEST_CASE("field registry is unique and describes protected payload fields", "[editor][registry]")
{
    std::set<std::pair<std::string_view, std::string_view>> keys;
    for (auto const& f : field_registry()) {
        CHECK(keys.emplace(f.scope, f.key).second);
        CHECK_FALSE(f.reference.empty());
        CHECK_FALSE(f.validation.empty());
        CHECK(f.affects_hash == (f.scope != "top" || f.key == "info"));
    }
    CHECK(find_field("info", "source")->editable);
    CHECK_FALSE(find_field("info", "files")->editable);
    CHECK_FALSE(find_field("file", "path")->editable);
    CHECK(find_field("top", "x-unknown") == nullptr);
}

TEST_CASE("metadata preview drops no-ops without canonicalizing info or removing signatures", "[editor][E01][E06]")
{
    auto original = imported(true);
    auto preview = preview_metadata_edit(original, {}, {{"source", Value::string("old")}, {"private", Value::integer(0)}, {"missing", std::nullopt}}, true);
    CHECK(preview.bytes == original.bytes());
    CHECK_FALSE(preview.info_changed);
    CHECK_FALSE(preview.removed_signatures);
    auto outer = Metainfo::parse(preview_metadata_edit(original, {{"comment", Value::string("new")}}, {}).bytes);
    CHECK(outer.raw_info() == original.raw_info());
    CHECK(outer.root().find("signatures") != nullptr);
    CHECK(outer.root().find("x-vendor")->text() == "99999999999999999999999");
}

TEST_CASE("equivalent nested dictionaries are no-ops even when a tagged round trip sorts their keys", "[editor][E02][E06]")
{
    auto root = imported(true).root();
    root.find("info")->set("x-nested", bencode::parse("d1:bi2e1:ai1ee"));
    auto original = Metainfo::parse(bencode::encode(root));
    auto sorted = Value::dictionary({{"a", Value::integer(1)}, {"b", Value::integer(2)}});
    auto preview = preview_metadata_edit(original, {}, {{"x-nested", sorted}});
    CHECK(preview.bytes == original.bytes());
    CHECK_FALSE(preview.removed_signatures);
    CHECK_FALSE(preview.info_changed);
}

TEST_CASE("combined metadata preview requires an explicit signature decision and reuses payload hashes", "[editor][E04][E06]")
{
    auto original = imported(true);
    CHECK_THROWS_AS(preview_metadata_edit(original, {}, {{"source", Value::string("new")}}), CoreError);
    auto preview = preview_metadata_edit(original, {{"comment", Value::string("new")}}, {{"source", Value::string("new")}}, true);
    auto edited = Metainfo::parse(preview.bytes);
    CHECK(preview.info_changed);
    CHECK(preview.removed_signatures);
    CHECK(preview.new_hashes.v1 != preview.old_hashes.v1);
    CHECK(edited.info().find("pieces")->text() == original.info().find("pieces")->text());
    CHECK(edited.info().find("x-bin")->text() == original.info().find("x-bin")->text());
    CHECK(edited.root().find("signatures") == nullptr);
    CHECK(edited.root().find("comment")->text() == "new");
}

TEST_CASE("registry validation rejects structural edits and malformed known fields", "[editor][E05]")
{
    auto original = imported();
    for (auto const* key : {"name", "pieces", "attr", "symlink path", "pieces root", "name.utf-8", "root hash"})
        CHECK_THROWS_AS(preview_metadata_edit(original, {}, {{key, Value::string("modified")}}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"piece layers", Value::dictionary()}}, {}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"info", Value::dictionary()}}, {}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"comment", Value::integer(2)}}, {}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"comment", Value::string("\xff")}}, {}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"creation date", Value::integer(-1)}}, {}), CoreError);
    // Setting existing private=0 is a no-op; it must not force a repair.
    CHECK_THROWS_AS(preview_metadata_edit(original, {}, {{"private", Value::integer(2)}}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"announce", Value::string("https:///missing-host")}}, {}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"announce-list", Value::list({Value::list()})}}, {}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"httpseeds", Value::string("https://example.org/")}}, {}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"url-list", Value::list({Value::string("udp://example.org:1")})}}, {}), CoreError);
    CHECK_THROWS_AS(preview_metadata_edit(original, {{"nodes", Value::list({Value::list({Value::string("host"), Value::integer(65536)})})}}, {}), CoreError);
}

TEST_CASE("typed extensions and BEP 17 seeds remain independent of BEP 19 shape", "[editor][E02]")
{
    auto original = imported();
    auto key = "\xff\x00"s;
    auto preview = preview_metadata_edit(original, {{key, Value::integer_text("900719925474099312345")},
        {"httpseeds", Value::list({Value::string("https://example.org/bep17")})},
        {"url-list", Value::string("https://example.org/bep19/")}}, {});
    auto edited = Metainfo::parse(preview.bytes);
    CHECK(edited.root().find(key)->text() == "900719925474099312345");
    CHECK(edited.root().find("url-list")->is_string());
    CHECK(edited.root().find("httpseeds")->is_list());
    CHECK(edited.raw_info() == original.raw_info());
}

TEST_CASE("preview of v1 v2 and hybrid metadata works without the original payload", "[editor][E04]")
{
    tc::test::TempDir dir;
    auto path = dir.path() / "payload.bin";
    tc::test::write_file(path, 100003, 7);
    for (auto format : {TorrentFormat::V1, TorrentFormat::V2, TorrentFormat::Hybrid}) {
        auto original = Metainfo::parse(tc::test::make_torrent(path, format, 16384));
        auto preview = preview_metadata_edit(original, {{"comment", Value::string("new")}}, {{"source", Value::string("new")}});
        auto edited = Metainfo::parse(preview.bytes);
        CHECK(validate_metainfo(edited).empty());
        for (auto key : {"pieces", "file tree"}) {
            auto const* old = original.info().find(key);
            if (old) CHECK(bencode::encode(*old) == bencode::encode(*edited.info().find(key)));
        }
        auto const* layers = original.root().find("piece layers");
        if (layers) CHECK(bencode::encode(*layers) == bencode::encode(*edited.root().find("piece layers")));
        std::filesystem::remove(path);
        CHECK_NOTHROW(preview_metadata_edit(edited, {{"comment", Value::string("without payload")}}, {}));
        tc::test::write_file(path, 100003, 7);
    }
}
