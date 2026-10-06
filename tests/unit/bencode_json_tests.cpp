// Binary-safe bencode representation for the frontend (section 7.4, E02).

#include "tc/bridge/bencode_json.hpp"
#include "tc/core/bencode.hpp"
#include "tc/core/error.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace tc;
using core::bencode::Value;

TEST_CASE("integers travel as decimal text without rounding", "[bridge][E02]")
{
    std::string const huge = "123456789012345678901234567890";
    nlohmann::json const j = bridge::bencode_to_json(Value::integer_text(huge));
    CHECK(j == nlohmann::json{{"t", "int"}, {"v", huge}});
    CHECK(bridge::bencode_from_json(j).text() == huge);
    // 2^53 + 1 is not representable as a JavaScript number.
    CHECK(bridge::bencode_to_json(Value::integer(9007199254740993))["v"] == "9007199254740993");
}

TEST_CASE("byte strings and keys keep their exact bytes", "[bridge][E02]")
{
    std::string const binary("\x00\xff\x10 key", 7);
    Value const dict = Value::dictionary({{binary, Value::string("Привет")}, {"plain", Value::string(std::string("\x80\x81", 2))}});
    nlohmann::json const j = bridge::bencode_to_json(dict);
    REQUIRE(j["t"] == "dict");
    CHECK(j["entries"][0]["key"] == nlohmann::json{{"t", "bytes"}, {"hex", "00ff10206b6579"}, {"len", 7}});
    CHECK(j["entries"][0]["value"]["utf8"] == "Привет");
    CHECK(j["entries"][1]["value"]["t"] == "bytes");
    CHECK(core::bencode::encode(bridge::bencode_from_json(j)) == core::bencode::encode(dict));
}

TEST_CASE("large structures are displayed lazily and cannot be saved back", "[bridge][E02][U02]")
{
    Value::List items;
    for (int i = 0; i < 100; ++i) items.push_back(Value::integer(i));
    Value const big = Value::list(std::move(items));

    nlohmann::json const j = bridge::bencode_to_json(big, {.max_nodes = 10, .max_string_bytes = 4});
    CHECK(j["items"][8]["t"] == "int");
    CHECK(j["items"][9] == nlohmann::json{{"t", "elided"}, {"nodes", 1}});
    CHECK(j["items"].size() == 10); // the omitted tail must not expand JSON
    CHECK_THROWS_AS(bridge::bencode_from_json(j), core::CoreError);

    nlohmann::json const s = bridge::bencode_to_json(Value::string("abcdefgh"), {.max_nodes = 10, .max_string_bytes = 4});
    CHECK(s["utf8"] == "abcd");
    CHECK(s["len"] == 8);
    CHECK(s["truncated"] == true);
    CHECK_THROWS_AS(bridge::bencode_from_json(s), core::CoreError);
}

TEST_CASE("integer strings and dictionary keys obey the outgoing display budget", "[bridge][editor][U02]")
{
    Value::Dictionary entries;
    for (int i = 0; i < 10000; ++i) entries.push_back({std::to_string(i), Value::string("value")});
    auto const j = bridge::bencode_to_json(Value::dictionary(std::move(entries)), {10, 4});
    CHECK(j.dump().size() < 1500);
    CHECK_THROWS_AS(bridge::bencode_from_json(j), core::CoreError);
    auto huge = bridge::bencode_to_json(Value::integer_text(std::string(10000, '9')), {10, 4});
    CHECK(huge.dump().size() < 100);
    CHECK_THROWS_AS(bridge::bencode_from_json(huge), core::CoreError);
}

TEST_CASE("malformed tagged values are rejected", "[bridge][E02][U01]")
{
    using nlohmann::json;
    CHECK_THROWS_AS(bridge::bencode_from_json(json{{"t", "int"}, {"v", "01"}}), core::CoreError);
    CHECK_THROWS_AS(bridge::bencode_from_json(json{{"t", "int"}, {"v", "-0"}}), core::CoreError);
    CHECK_THROWS_AS(bridge::bencode_from_json(json{{"t", "int"}, {"v", 5}}), core::CoreError);
    CHECK_THROWS_AS(bridge::bencode_from_json(json{{"t", "bytes"}, {"hex", "abc"}}), core::CoreError);
    CHECK_THROWS_AS(bridge::bencode_from_json(json{{"t", "float"}}), core::CoreError);
    json const dup = {{"t", "dict"},
        {"entries", json::array({{{"key", {{"t", "str"}, {"utf8", "a"}}}, {"value", {{"t", "int"}, {"v", "1"}}}},
                        {{"key", {{"t", "str"}, {"utf8", "a"}}}, {"value", {{"t", "int"}, {"v", "2"}}}}})}};
    CHECK_THROWS_AS(bridge::bencode_from_json(dup), core::CoreError);
}

TEST_CASE("nonboolean truncation flags return a domain validation error", "[bridge][E02][fuzz]")
{
    using nlohmann::json;
    for (auto const& flag : json::array({"wrong", 1, nullptr, json::array(), json::object()})) {
        json const scalar = {{"t", "int"}, {"v", "1"}, {"truncated", flag}};
        json const key = {{"t", "str"}, {"utf8", "a"}, {"truncated", flag}};
        json const dictionary = {{"t", "dict"}, {"entries", json::array({
            {{"key", key}, {"value", {{"t", "int"}, {"v", "1"}}}}})}};
        for (auto const& value : {scalar, dictionary}) {
            try {
                (void)bridge::bencode_from_json(value);
                FAIL("malformed truncation flag was accepted");
            } catch (core::CoreError const& error) {
                CHECK(error.code() == core::ErrorCode::InvalidArgument);
            }
        }
    }
}
