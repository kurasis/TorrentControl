#include "tc/bridge/bencode_json.hpp"
#include "tc/bridge/protocol.hpp"
#include "tc/core/error.hpp"
#include "tc/service/json_input.hpp"
#include "tc/service/storage.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using nlohmann::json;

namespace {

constexpr auto page = "https://torrentcontrol.example/index.html";

std::string nested_request(std::size_t arrays)
{
    return R"({"protocolVersion":1,"requestId":"depth","operation":"echo","payload":{"value":)"
        + std::string(arrays, '[') + "0" + std::string(arrays, ']') + "}}";
}

void require_resource_limit(std::filesystem::path const& path, bool project)
{
    try {
        if (project) (void)tc::service::load_project(path);
        else (void)tc::service::load_settings(path);
        FAIL("Deep input was accepted");
    } catch (tc::core::CoreError const& e) {
        CHECK(e.code() == tc::core::ErrorCode::ResourceLimit);
    }
}

} // namespace

TEST_CASE("bridge rejects deep JSON before dispatch including queue refusals", "[bridge][security]")
{
    tc::bridge::Dispatcher dispatcher;
    int calls = 0;
    dispatcher.register_operation("echo", [&](json const& payload) { ++calls; return payload; });
    auto const accepted = nested_request(tc::service::max_json_input_depth - 2);
    CHECK(json::parse(dispatcher.handle(accepted, page))["ok"] == true);
    CHECK(calls == 1);
    for (auto depth : {tc::service::max_json_input_depth - 1, std::size_t{100'000}}) {
        auto const request = nested_request(depth);
        REQUIRE(request.size() < tc::bridge::max_message_bytes);
        auto const response = json::parse(dispatcher.handle(request, page));
        CHECK(response["error"]["code"] == "MESSAGE_TOO_DEEP");
        CHECK(response["requestId"].is_null());
        CHECK(calls == 1);
        auto const refusal = json::parse(tc::bridge::reject_request(request, "BUSY", "Queue full", true));
        CHECK(refusal["error"]["code"] == "BUSY");
        CHECK(refusal["error"]["retryable"] == true);
        CHECK(refusal["requestId"].is_null());
        CHECK(json::parse(dispatcher.handle(request, "https://evil.example/"))["error"]["code"] == "ORIGIN_REJECTED");
    }
    CHECK(json::parse(tc::bridge::reject_request(accepted, "BUSY", "Queue full"))["requestId"] == "depth");
    CHECK(json::parse(dispatcher.handle(nested_request(0), page))["ok"] == true);
    CHECK(calls == 2);
}

TEST_CASE("JSON depth scan respects strings escapes and the real syntax parser", "[security]")
{
    std::string const text = std::string(2000, '[') + "\"\\\\\"" + std::string(2000, '}');
    auto const encoded = json{{"text", text}, {"other", "\\"}}.dump();
    CHECK(tc::service::parse_json_input(encoded)["text"] == text);
    CHECK(tc::service::parse_json_input("{\"x\":\"\\\"[\",\"y\":0}")["y"] == 0);
    // An escaped backslash must not hide the following closing quote.
    auto const hidden = R"({"text":"\\","value":)" + std::string(513, '[') + "0" + std::string(513, ']') + "}";
    CHECK_THROWS_AS(tc::service::parse_json_input(hidden), tc::core::CoreError);
    for (auto const* invalid : {"[}", "][", "{\"x\":\"\\q\"}", "{\"x\":1e99999}", "{\"x\":\"unterminated"})
        CHECK(tc::service::parse_json_input(invalid).is_discarded());
    std::string invalid_utf8 = "{\"x\":\"";
    invalid_utf8 += static_cast<char>(0xff);
    invalid_utf8 += "\"}";
    CHECK(tc::service::parse_json_input(invalid_utf8).is_discarded());
}

TEST_CASE("projects and settings bound even ignored JSON fields", "[service][security]")
{
    tc::test::TempDir dir;
    auto const path = dir.path() / "input.json";
    for (bool project : {true, false}) {
        for (auto depth : {tc::service::max_json_input_depth, std::size_t{100'000}}) {
            auto const prefix = project ? R"({"format":"torrentcontrol-project","version":1,"draft":{},"ignored":)"
                                       : R"({"theme":"dark","ignored":)";
            auto const bytes = prefix + std::string(depth, '[') + "0" + std::string(depth, ']') + "}";
            tc::test::write_bytes(path, bytes);
            require_resource_limit(path, project);
        }
    }
    tc::test::write_bytes(path, R"({"theme":"dark","ignored":[0]})");
    CHECK(tc::service::load_settings(path).theme == "dark");
    tc::test::write_bytes(path, "not JSON");
    CHECK(tc::service::load_settings(path).theme == "system");
}

TEST_CASE("bridge JSON depth limit preserves maximum-depth tagged dictionaries", "[bridge][security]")
{
    using tc::core::bencode::Value;
    auto value = Value::integer(0);
    for (int i = 0; i < 128; ++i) value = Value::dictionary({{"x", std::move(value)}});
    auto const tagged = tc::bridge::bencode_to_json(value);
    auto const request = json{{"protocolVersion", 1}, {"requestId", "tagged"}, {"operation", "echo"},
        {"payload", {{"value", tagged}}}}.dump();
    tc::bridge::Dispatcher dispatcher;
    dispatcher.register_operation("echo", [](json const& payload) { return payload; });
    auto const response = json::parse(dispatcher.handle(request, page));
    REQUIRE(response["ok"] == true);
    CHECK(tc::core::bencode::encode(tc::bridge::bencode_from_json(response["result"]["value"]))
        == tc::core::bencode::encode(value));
}
