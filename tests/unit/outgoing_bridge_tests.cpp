#include "tc/bridge/protocol.hpp"
#include "tc/bridge/bencode_json.hpp"
#include "tc/core/error.hpp"
#include <catch2/catch_test_macros.hpp>

using nlohmann::json;
using tc::core::bencode::Value;

TEST_CASE("outgoing bridge bounds escaped bytes and preserves request correlation", "[bridge][bounds]")
{
    tc::bridge::Dispatcher d;
    int mutations = 0;
    d.register_operation("large", [&](json const&) { ++mutations; return json{{"text", std::string(200000, '\0')}}; });
    auto response = d.handle(R"({"protocolVersion":1,"requestId":"large-1","operation":"large"})",
        "https://torrentcontrol.example/index.html");
    CHECK(response.size() <= tc::bridge::max_message_bytes);
    auto value = json::parse(response);
    CHECK(value["requestId"] == "large-1");
    CHECK(value["ok"] == false);
    CHECK(value["error"]["code"] == "RESPONSE_TOO_LARGE");
    CHECK(value["error"]["retryable"] == false);
    CHECK(mutations == 1); // an oversized reply never replays its mutation
}

TEST_CASE("large allowed responses retain bytes and invalid UTF8 still serializes", "[bridge][bounds]")
{
    tc::bridge::Dispatcher d;
    auto text = std::string(tc::bridge::max_message_bytes - 512, 'a');
    d.register_operation("nearLimit", [&](json const&) { return json{{"text", text}}; });
    auto response = d.handle(R"({"protocolVersion":1,"requestId":"r","operation":"nearLimit"})",
        "https://torrentcontrol.example/index.html");
    CHECK(response.size() < tc::bridge::max_message_bytes);
    CHECK(json::parse(response)["result"]["text"] == text);
    d.register_operation("utf8", [](json const&) { return json{{"text", std::string("a\xff" "b", 3)}}; });
    auto repaired = json::parse(d.handle(R"({"protocolVersion":1,"requestId":"u","operation":"utf8"})",
        "https://torrentcontrol.example/index.html"));
    CHECK(repaired["ok"] == true);
    CHECK(repaired["result"]["text"] == "a\xef\xbf\xbd" "b");
}

TEST_CASE("oversized events request native resynchronization with monotonic sequence", "[bridge][bounds]")
{
    tc::bridge::EventChannel channel;
    auto bytes = channel.wrap({{"type", "job"}, {"data", std::string(2 * tc::bridge::max_message_bytes, 'x')}});
    CHECK(bytes.size() < 1024);
    auto event = json::parse(bytes);
    CHECK(event["event"] == "resyncRequired");
    CHECK(event["sequence"] == "1");
    CHECK(json::parse(channel.wrap({{"type", "scan"}, {"state", "ready"}}))["sequence"] == "2");
}

TEST_CASE("metadata display has an aggregate escaped byte budget", "[bridge][bounds][E02]")
{
    Value::List values;
    for (int i = 0; i < 1000; ++i) values.push_back(Value::string(std::string(8192, '\0')));
    auto native = Value::list(std::move(values));
    auto display = tc::bridge::bencode_to_json(native);
    CHECK(display.dump().size() <= 256 * 1024);
    CHECK(display["items"].size() < 1000);
    CHECK_THROWS_AS(tc::bridge::bencode_from_json(display), tc::core::CoreError);
    CHECK(native.items().size() == 1000);
    CHECK(native.items().back().text().size() == 8192);
}

TEST_CASE("omitted metadata subtrees expose direct children without expanding the tail", "[bridge][bounds][E02]")
{
    Value::List children;
    for (int i = 0; i < 100000; ++i) children.push_back(Value::integer(i));
    auto native = Value::list({Value::list(std::move(children))});
    auto display = tc::bridge::bencode_to_json(native, {1, 4096});
    REQUIRE(display["items"].size() == 1);
    CHECK(display["items"][0]["t"] == "elided");
    CHECK(display["items"][0]["children"] == 100000);
    CHECK(display.dump().size() < 128);
    CHECK_THROWS_AS(tc::bridge::bencode_from_json(display), tc::core::CoreError);
}

TEST_CASE("nested binary keys and strings obey small aggregate display budgets", "[bridge][bounds][E02]")
{
    Value::Dictionary entries;
    for (int i = 0; i < 500; ++i)
        entries.push_back({std::to_string(i) + std::string(1024, '\0'), Value::list({Value::string(std::string(4096, '\xff')), Value::integer(i)})});
    auto native = Value::dictionary(std::move(entries));
    for (std::size_t bytes : {128u, 256u, 1024u, 4096u, 262144u}) {
        auto display = tc::bridge::bencode_to_json(native, {10000, 4096, bytes});
        CHECK(display.dump().size() <= bytes);
        CHECK_THROWS_AS(tc::bridge::bencode_from_json(display), tc::core::CoreError);
    }
    CHECK_THROWS_AS(tc::bridge::bencode_to_json(native, {10000, 4096, 0}), tc::core::CoreError);
}
