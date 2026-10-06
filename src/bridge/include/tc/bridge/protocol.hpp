#pragma once

// CommandBridge message contract (specification section 13.1).
//
// Request:  {"protocolVersion":1,"requestId":"req-1","operation":"getEngineInfo",
//            "draftRevision":"7","payload":{}}
// Response: {"protocolVersion":1,"requestId":"req-1","ok":true,"result":{...}}
//       or  {"protocolVersion":1,"requestId":"req-1","ok":false,
//            "error":{"code":"...","message":"...","retryable":false}}
//
// Every message is parsed with a JSON parser and validated before dispatch.
// Nothing received from the page is ever interpolated into script.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tc::bridge {

inline constexpr int protocol_version = 1;
inline constexpr std::size_t max_message_bytes = 1024 * 1024;

// Origin the bundled frontend is served from (virtual host mapping).
inline constexpr std::string_view app_origin = "https://torrentcontrol.example";

// True when `source_uri` is a document served from `origin` (exact scheme and
// host, followed by a path). Used to reject messages from any other frame.
bool is_allowed_source(std::string_view source_uri, std::string_view origin = app_origin);

// Correlated refusal without running a handler (for a saturated host queue).
std::string reject_request(std::string_view message, std::string code, std::string reason, bool retryable = false);

class BridgeError : public std::runtime_error {
public:
    BridgeError(std::string code, std::string message, bool retryable = false)
        : std::runtime_error(std::move(message)), code_(std::move(code)), retryable_(retryable)
    {
    }
    std::string const& code() const noexcept { return code_; }
    bool retryable() const noexcept { return retryable_; }

private:
    std::string code_;
    bool retryable_;
};

// A validated request as seen by a handler.
struct Request {
    nlohmann::json const& payload;
    // The draft revision the page last saw, when the message carried one.
    std::optional<std::uint64_t> draft_revision;
    // Files the browser attached natively to the message (drag and drop).
    // They come from the WebView2 runtime, not from page-supplied strings.
    std::vector<std::filesystem::path> const& attached_paths;
};

class Dispatcher {
public:
    // Handlers return the result and may throw BridgeError,
    // tc::core::CoreError or tc::service::ServiceError.
    using Handler = std::function<nlohmann::json(nlohmann::json const& payload)>;
    using RequestHandler = std::function<nlohmann::json(Request const& request)>;

    void register_operation(std::string name, Handler handler);
    void register_request_operation(std::string name, RequestHandler handler);

    // Validates and dispatches one message. Always returns a serialized
    // response and never throws. Messages from a disallowed source are
    // answered with an ORIGIN_REJECTED error and not dispatched.
    std::string handle(std::string_view message, std::string_view source_uri,
        std::vector<std::filesystem::path> const& attached_paths = {}) const;

private:
    std::map<std::string, RequestHandler, std::less<>> handlers_;
};

// Native-to-page events: {"protocolVersion":1,"event":"job","sequence":"7",
// "payload":{...}}. Sequence numbers are monotonic across all events so the
// page can drop anything older than a snapshot it already applied.
class EventChannel {
public:
    // Serializes `event` ({"type": ..., ...}); safe to call from any thread.
    std::string wrap(nlohmann::json const& event);
    std::uint64_t last_sequence() const;

private:
    mutable std::mutex mutex_;
    std::uint64_t sequence_ = 0;
};

// Registers operations backed by the core that need no host services.
void register_core_operations(Dispatcher& d, std::string app_version);

} // namespace tc::bridge
