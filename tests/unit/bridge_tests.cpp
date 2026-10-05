#include "tc/bridge/protocol.hpp"
#include "tc/core/error.hpp"

#include <catch2/catch_test_macros.hpp>

using tc::bridge::Dispatcher;
using nlohmann::json;

namespace {

constexpr char const* page = "https://torrentcontrol.example/index.html";

json call(Dispatcher const& d, std::string const& message, std::string const& source = page)
{
    return json::parse(d.handle(message, source));
}

Dispatcher make()
{
    Dispatcher d;
    tc::bridge::register_core_operations(d, "0.1.0");
    d.register_operation("fail", [](json const&) -> json {
        throw tc::core::CoreError(tc::core::ErrorCode::SourceMissing, "gone");
    });
    d.register_operation("echo", [](json const& p) { return p; });
    return d;
}

} // namespace

TEST_CASE("origin checks accept only the bundled application origin", "[bridge][U01]")
{
    using tc::bridge::is_allowed_source;
    CHECK(is_allowed_source("https://torrentcontrol.example/"));
    CHECK(is_allowed_source("https://torrentcontrol.example/index.html"));
    CHECK_FALSE(is_allowed_source("https://torrentcontrol.example"));
    CHECK_FALSE(is_allowed_source("https://torrentcontrol.example.evil.com/"));
    CHECK_FALSE(is_allowed_source("https://torrentcontrol.example:8443/"));
    CHECK_FALSE(is_allowed_source("http://torrentcontrol.example/"));
    CHECK_FALSE(is_allowed_source("file:///C:/index.html"));
}

TEST_CASE("getEngineInfo reports the pinned engine", "[bridge]")
{
    json const r = call(make(), R"({"protocolVersion":1,"requestId":"req-1","operation":"getEngineInfo","payload":{}})");
    CHECK(r["ok"] == true);
    CHECK(r["requestId"] == "req-1");
    CHECK(r["result"]["engineVersion"].get<std::string>().starts_with("2.1.2"));
    CHECK(r["result"]["appVersion"] == "0.1.0");
}

TEST_CASE("invalid messages are rejected with stable codes", "[bridge][U01]")
{
    Dispatcher const d = make();
    CHECK(call(d, R"({"protocolVersion":1,"requestId":"r","operation":"echo"})", "https://evil.example/")["error"]["code"] == "ORIGIN_REJECTED");
    CHECK(call(d, "not json")["error"]["code"] == "INVALID_MESSAGE");
    CHECK(call(d, "[1,2]")["error"]["code"] == "INVALID_MESSAGE");
    CHECK(call(d, R"({"protocolVersion":1,"requestId":"bad id!","operation":"echo"})")["error"]["code"] == "INVALID_MESSAGE");
    CHECK(call(d, R"({"protocolVersion":2,"requestId":"r","operation":"echo"})")["error"]["code"] == "UNSUPPORTED_PROTOCOL");
    CHECK(call(d, R"({"protocolVersion":1,"requestId":"r","operation":"execShell"})")["error"]["code"] == "UNKNOWN_OPERATION");
    CHECK(call(d, R"({"protocolVersion":1,"requestId":"r","operation":"echo","payload":"x"})")["error"]["code"] == "INVALID_MESSAGE");
    CHECK(call(d, R"({"protocolVersion":1,"requestId":"r","operation":"echo","draftRevision":7})")["error"]["code"] == "INVALID_MESSAGE");
    std::string const huge = R"({"protocolVersion":1,"requestId":"r","operation":"echo","payload":{"x":")" + std::string(1024 * 1024, 'a') + "\"}}";
    CHECK(call(d, huge)["error"]["code"] == "MESSAGE_TOO_LARGE");
}

TEST_CASE("handler errors map to stable error codes", "[bridge]")
{
    json const r = call(make(), R"({"protocolVersion":1,"requestId":"r9","operation":"fail"})");
    CHECK(r["ok"] == false);
    CHECK(r["requestId"] == "r9");
    CHECK(r["error"]["code"] == "SOURCE_MISSING");
}

TEST_CASE("payload strings round-trip as data", "[bridge][U01]")
{
    json const r = call(make(), R"({"protocolVersion":1,"requestId":"r","operation":"echo","draftRevision":"7","payload":{"name":"</script><img onerror=alert(1)>"}})");
    CHECK(r["result"]["name"] == "</script><img onerror=alert(1)>");
}
