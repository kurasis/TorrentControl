#include "tc/bridge/protocol.hpp"
#include "tc/core/error.hpp"
#include "tc/service/json_input.hpp"
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace {
using nlohmann::json;
void require(bool condition) { if (!condition) std::abort(); }
bool valid_id(json const& value)
{
    if (!value.is_string()) return false;
    auto const& id = value.get_ref<std::string const&>();
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
json response(std::string const& bytes)
{
    require(bytes.size() <= tc::bridge::max_message_bytes);
    auto value = json::parse(bytes, nullptr, false);
    require(value.is_object() && value.contains("ok") && value["ok"].is_boolean());
    require(value["protocolVersion"] == tc::bridge::protocol_version);
    require(value.contains("requestId"));
    return value;
}
}

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    if (size > 65536) return 0;
    std::string const bytes(reinterpret_cast<char const*>(data), size);
    std::optional<json> parsed;
    try { parsed = tc::service::parse_json_input(bytes); }
    catch (tc::core::CoreError const& error) {
        require(error.code() == tc::core::ErrorCode::ResourceLimit);
    }
    if (parsed && !parsed->is_discarded()) {
        require(tc::service::parse_json_input(parsed->dump()) == *parsed);
    }
    json expected_id = nullptr;
    bool expected_dispatch = false;
    std::optional<std::uint64_t> expected_revision;
    if (parsed && parsed->is_object() && parsed->contains("requestId") && valid_id((*parsed)["requestId"])) {
        auto const& request = *parsed;
        expected_id = request["requestId"];
        expected_dispatch = request.contains("protocolVersion") && request["protocolVersion"].is_number_integer()
            && request["protocolVersion"] == tc::bridge::protocol_version
            && request.contains("operation") && request["operation"] == "probe"
            && (!request.contains("payload") || request["payload"].is_object());
        if (request.contains("draftRevision")) {
            auto const& revision = request["draftRevision"];
            if (!revision.is_string()) expected_dispatch = false;
            else {
                auto const& text = revision.get_ref<std::string const&>();
                std::uint64_t number = 0;
                // Independent oracle for the decimal uint64 contract (the
                // dispatcher uses a digit check followed by std::stoull).
                auto const converted = std::from_chars(text.data(), text.data() + text.size(), number);
                if (text.empty() || text.size() > 20 || converted.ec != std::errc()
                    || converted.ptr != text.data() + text.size()) expected_dispatch = false;
                else expected_revision = number;
            }
        }
    }
    tc::bridge::Dispatcher dispatcher;
    int calls = 0;
    dispatcher.register_request_operation("probe", [&](tc::bridge::Request const& request) {
        ++calls;
        require(request.payload.is_object());
        require(request.draft_revision == expected_revision);
        return json{{"hasRevision", request.draft_revision.has_value()}};
    });
    auto accepted = response(dispatcher.handle(bytes, "https://torrentcontrol.example/index.html"));
    require(accepted["ok"] == expected_dispatch && calls == (expected_dispatch ? 1 : 0));
    require(accepted["requestId"] == expected_id);
    auto rejected = response(dispatcher.handle(bytes, "https://torrentcontrol.example.evil.invalid/"));
    require(rejected["ok"] == false && rejected["error"]["code"] == "ORIGIN_REJECTED");
    require(calls == (accepted["ok"] == true ? 1 : 0));
    auto refused = response(tc::bridge::reject_request(bytes, "BUSY", "Queue full", true));
    require(refused["ok"] == false && refused["error"]["code"] == "BUSY");
    require(refused["error"]["retryable"] == true);
    require(refused["requestId"] == accepted["requestId"]);
    // A rejected input must not prevent the next valid request from dispatching.
    expected_revision.reset();
    auto healthy = response(dispatcher.handle(
        R"({"protocolVersion":1,"requestId":"healthy","operation":"probe","payload":{}})",
        "https://torrentcontrol.example/index.html"));
    require(healthy["ok"] == true && healthy["requestId"] == "healthy");
    require(calls == (accepted["ok"] == true ? 2 : 1));
    return 0;
}
