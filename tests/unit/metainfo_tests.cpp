#include "tc/core/error.hpp"
#include "tc/core/metainfo.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <string>

using namespace tc::core;
using namespace std::string_literals;

namespace {

// A v1 info dictionary with an unknown binary key and deliberately
// non-canonical key order: re-encoding it would change the infohash.
std::string const v1_info = "d4:name5:a.txt6:lengthi5e12:piece lengthi16384e6:pieces20:AAAAAAAAAAAAAAAAAAAA7:x-\xff\xfe" "bin2:\x00\x01" "e"s;

std::string sample_torrent()
{
    return "d8:announce14:http://t/a/ann7:comment3:old4:info" + v1_info + "8:x-vendori99999999999999999999999ee";
}

std::optional<ErrorCode> metainfo_error(std::string bytes)
{
    try {
        Metainfo::parse(std::move(bytes));
    } catch (CoreError const& e) {
        return e.code();
    }
    return std::nullopt; // no error: compares unequal to every expected code
}

} // namespace

TEST_CASE("identifiers are computed over the raw info bytes", "[metainfo]")
{
    Metainfo const m = Metainfo::parse(sample_torrent());
    CHECK(m.format() == MetainfoFormat::V1);
    CHECK(m.raw_info() == v1_info);
    REQUIRE(m.info_hashes().v1.has_value());
    CHECK(*m.info_hashes().v1 == sha1(v1_info));
    CHECK_FALSE(m.info_hashes().v2.has_value());
}

TEST_CASE("unknown meta version is reported as unsupported rather than corrupt", "[metainfo][E07]")
{
    std::string const t = "d4:infod9:file treede12:meta versioni3e4:name1:x12:piece lengthi16384eee";
    CHECK(metainfo_error(t) == ErrorCode::UnsupportedFormat);
}

TEST_CASE("structural problems are invalid metainfo", "[metainfo][E03]")
{
    CHECK(metainfo_error("le") == ErrorCode::InvalidMetainfo);
    CHECK(metainfo_error("d3:fooi1ee") == ErrorCode::InvalidMetainfo);
    CHECK(metainfo_error("d4:infoi1ee") == ErrorCode::InvalidMetainfo);
    CHECK(metainfo_error("d4:infod4:name1:x12:piece lengthi16384e6:pieces3:abcee") == ErrorCode::InvalidMetainfo);
}

TEST_CASE("oversized metainfo is rejected before parsing", "[metainfo]")
{
    MetainfoLimits limits;
    limits.max_bytes = 10;
    CHECK_THROWS_AS(Metainfo::parse(sample_torrent(), limits), CoreError);
}

TEST_CASE("outer-only edit preserves raw info and unknown fields", "[metainfo][E01]")
{
    Metainfo const original = Metainfo::parse(sample_torrent());

    OuterEdit edit;
    edit["comment"] = bencode::Value::string("new comment");
    edit["announce"] = std::nullopt;
    edit["url-list"] = bencode::Value::list({bencode::Value::string("https://example.org/dl/")});
    Metainfo const edited = Metainfo::parse(apply_outer_edit(original, edit));

    CHECK(edited.raw_info() == original.raw_info());
    CHECK(edited.info_hashes().v1 == original.info_hashes().v1);
    CHECK(edited.root().find("announce") == nullptr);
    CHECK(edited.root().find("comment")->text() == "new comment");
    CHECK(edited.root().find("x-vendor")->text() == "99999999999999999999999");
    CHECK(edited.root().canonical_order());
}

TEST_CASE("outer-only edit refuses structural keys", "[metainfo][E05]")
{
    Metainfo const original = Metainfo::parse(sample_torrent());
    CHECK_THROWS_AS(apply_outer_edit(original, {{"info", bencode::Value::dictionary()}}), CoreError);
    CHECK_THROWS_AS(apply_outer_edit(original, {{"piece layers", bencode::Value::dictionary()}}), CoreError);
}

TEST_CASE("magnet encodes parameters once and lists v1 topic", "[metainfo][E09]")
{
    std::string const info = "d6:lengthi1e4:name5:a b&c12:piece lengthi16384e6:pieces20:AAAAAAAAAAAAAAAAAAAAe";
    Metainfo const m = Metainfo::parse("d8:announce17:udp://t.example:14:info" + info + "e");
    std::string const magnet = make_magnet(m);
    CHECK(magnet == "magnet:?xt=urn:btih:" + to_hex(*m.info_hashes().v1) + "&dn=a%20b%26c&tr=udp%3A%2F%2Ft.example%3A1");
}
