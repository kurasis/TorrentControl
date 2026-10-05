#include "tc/core/bencode.hpp"
#include "tc/core/error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <string>

using namespace tc::core;
using namespace std::string_literals;

namespace {

std::optional<ErrorCode> parse_error(std::string const& input, bencode::Limits const& limits = {})
{
    try {
        bencode::parse(input, limits);
    } catch (CoreError const& e) {
        return e.code();
    }
    return std::nullopt; // no error: compares unequal to every expected code
}

} // namespace

TEST_CASE("bencode round-trips binary keys and strings and huge integers", "[bencode][E02]")
{
    std::string const input = "d3:\x00\xff\x01" "i123456789012345678901234567890e" "1:al1:\x00i-5ee" "1:bi0ee"s;
    bencode::Value const v = bencode::parse(input);
    REQUIRE(v.is_dictionary());
    REQUIRE(v.entries().size() == 3);

    bencode::Value const* huge = v.find("\x00\xff\x01"s);
    REQUIRE(huge != nullptr);
    CHECK(huge->text() == "123456789012345678901234567890");
    CHECK_FALSE(huge->as_int64().has_value());

    CHECK(v.find("a")->items().at(0).text() == "\x00"s);
    CHECK(v.find("a")->items().at(1).as_int64() == -5);
    CHECK(bencode::encode(v) == input);
}

TEST_CASE("bencode records raw byte ranges", "[bencode]")
{
    std::string const input = "d4:infod1:xi1eee";
    bencode::Value const v = bencode::parse(input);
    bencode::ByteRange const r = v.find("info")->raw();
    CHECK(input.substr(r.offset, r.size) == "d1:xi1ee");
}

TEST_CASE("bencode rejects malformed input without repair", "[bencode][E03]")
{
    CHECK(parse_error("i-0e") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("i03e") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("ie") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("i-e") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("i1x2e") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("i12") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("5:abc") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("03:abc") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("99999999999999999999999:x") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("l") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("d1:a") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("di1ei2ee") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("i1ei2e") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("x") == ErrorCode::InvalidMetainfo);
}

TEST_CASE("bencode rejects duplicate keys in sorted and unsorted dictionaries", "[bencode][E03]")
{
    CHECK(parse_error("d1:ai1e1:ai2ee") == ErrorCode::InvalidMetainfo);
    CHECK(parse_error("d1:bi1e1:ai2e1:bi3ee") == ErrorCode::InvalidMetainfo);
}

TEST_CASE("bencode enforces nesting and node limits", "[bencode][E03]")
{
    std::string deep(200, 'l');
    deep += std::string(200, 'e');
    CHECK(parse_error(deep) == ErrorCode::ResourceLimit);

    bencode::Limits small;
    small.max_nodes = 3;
    CHECK(parse_error("li1ei2ei3ee", small) == ErrorCode::ResourceLimit);
    CHECK_NOTHROW(bencode::parse("li1ei2ee", small));
}

TEST_CASE("unsorted dictionaries are flagged and preserved byte-for-byte", "[bencode]")
{
    std::string const input = "d1:bi1e1:ai2ee";
    bencode::Value const v = bencode::parse(input);
    CHECK_FALSE(v.canonical_order());
    CHECK(bencode::encode(v) == input);
}

TEST_CASE("constructed dictionaries sort keys by unsigned bytes", "[bencode]")
{
    bencode::Value d = bencode::Value::dictionary({{"\xff"s, bencode::Value::integer(1)}, {"a", bencode::Value::integer(2)}});
    d.set("b", bencode::Value::string("x"));
    CHECK(bencode::encode(d) == "d1:ai2e1:b1:x1:\xffi1ee"s);
    CHECK_THROWS_AS(bencode::Value::dictionary({{"a", {}}, {"a", {}}}), CoreError);
    CHECK_THROWS_AS(bencode::Value::integer_text("007"), CoreError);
}
