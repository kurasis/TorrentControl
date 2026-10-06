#include "tc/bridge/protocol.hpp"

#include "tc/core/error.hpp"
#include "tc/core/torrent_engine.hpp"
#include "tc/service/jobs.hpp"

#include <algorithm>

namespace tc::bridge {

namespace {

using nlohmann::json;

bool valid_request_id(std::string const& id)
{
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool valid_revision(std::string const& rev)
{
    return !rev.empty() && rev.size() <= 20 && std::all_of(rev.begin(), rev.end(), [](char c) { return c >= '0' && c <= '9'; });
}

json error_response(json request_id, std::string code, std::string message, bool retryable = false)
{
    return json{{"protocolVersion", protocol_version}, {"requestId", std::move(request_id)}, {"ok", false},
        {"error", {{"code", std::move(code)}, {"message", std::move(message)}, {"retryable", retryable}}}};
}

std::string serialize(json const& j)
{
    // Replace invalid UTF-8 instead of throwing; responses must always serialize.
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace

std::string reject_request(std::string_view message, std::string code, std::string reason, bool retryable)
{
    json id = nullptr;
    if (message.size() <= max_message_bytes) {
        auto request = json::parse(message, nullptr, false);
        if (request.is_object()) {
            auto it = request.find("requestId");
            if (it != request.end() && it->is_string() && valid_request_id(it->get<std::string>())) id = *it;
        }
    }
    return serialize(error_response(std::move(id), std::move(code), std::move(reason), retryable));
}

bool is_allowed_source(std::string_view source_uri, std::string_view origin)
{
    if (source_uri.size() <= origin.size()) return false;
    if (source_uri.substr(0, origin.size()) != origin) return false;
    return source_uri[origin.size()] == '/';
}

void Dispatcher::register_operation(std::string name, Handler handler)
{
    handlers_.insert_or_assign(std::move(name), [h = std::move(handler)](Request const& r) { return h(r.payload); });
}

void Dispatcher::register_request_operation(std::string name, RequestHandler handler)
{
    handlers_.insert_or_assign(std::move(name), std::move(handler));
}

std::string EventChannel::wrap(json const& event)
{
    std::uint64_t seq = 0;
    {
        std::lock_guard lock(mutex_);
        seq = ++sequence_;
    }
    json payload = event;
    std::string type = payload.value("type", "event");
    payload.erase("type");
    return serialize(json{{"protocolVersion", protocol_version}, {"event", std::move(type)},
        {"sequence", std::to_string(seq)}, {"payload", std::move(payload)}});
}

std::uint64_t EventChannel::last_sequence() const
{
    std::lock_guard lock(mutex_);
    return sequence_;
}

std::string Dispatcher::handle(std::string_view message, std::string_view source_uri,
    std::vector<std::filesystem::path> const& attached_paths) const
{
    if (!is_allowed_source(source_uri))
        return serialize(error_response(nullptr, "ORIGIN_REJECTED", "Message from an unexpected origin"));
    if (message.size() > max_message_bytes)
        return serialize(error_response(nullptr, "MESSAGE_TOO_LARGE", "Message exceeds the 1 MiB limit"));

    json const request = json::parse(message, nullptr, /*allow_exceptions=*/false);
    if (request.is_discarded() || !request.is_object())
        return serialize(error_response(nullptr, "INVALID_MESSAGE", "Message is not a JSON object"));

    auto const id_it = request.find("requestId");
    if (id_it == request.end() || !id_it->is_string() || !valid_request_id(id_it->get<std::string>()))
        return serialize(error_response(nullptr, "INVALID_MESSAGE", "Missing or invalid requestId"));
    json const request_id = *id_it;

    auto const version_it = request.find("protocolVersion");
    if (version_it == request.end() || !version_it->is_number_integer() || version_it->get<std::int64_t>() != protocol_version)
        return serialize(error_response(request_id, "UNSUPPORTED_PROTOCOL", "Unsupported protocol version"));

    auto const op_it = request.find("operation");
    if (op_it == request.end() || !op_it->is_string())
        return serialize(error_response(request_id, "INVALID_MESSAGE", "Missing operation"));
    auto const handler = handlers_.find(op_it->get<std::string>());
    if (handler == handlers_.end())
        return serialize(error_response(request_id, "UNKNOWN_OPERATION", "Unknown operation"));

    std::optional<std::uint64_t> revision;
    if (auto const rev = request.find("draftRevision"); rev != request.end()) {
        if (!rev->is_string() || !valid_revision(rev->get<std::string>()))
            return serialize(error_response(request_id, "INVALID_MESSAGE", "draftRevision must be a decimal string"));
        try {
            revision = std::stoull(rev->get<std::string>());
        } catch (std::out_of_range const&) {
            return serialize(error_response(request_id, "INVALID_MESSAGE", "draftRevision is out of range"));
        }
    }

    json payload = json::object();
    if (auto const p = request.find("payload"); p != request.end()) {
        if (!p->is_object()) return serialize(error_response(request_id, "INVALID_MESSAGE", "payload must be an object"));
        payload = *p;
    }

    try {
        json result = handler->second(Request{payload, revision, attached_paths});
        return serialize(json{{"protocolVersion", protocol_version}, {"requestId", request_id}, {"ok", true},
            {"result", std::move(result)}});
    } catch (BridgeError const& e) {
        return serialize(error_response(request_id, e.code(), e.what(), e.retryable()));
    } catch (service::ServiceError const& e) {
        return serialize(error_response(request_id, e.code(), e.what(), e.retryable()));
    } catch (core::CoreError const& e) {
        return serialize(error_response(request_id, std::string(core::to_string(e.code())), e.what(), e.retryable()));
    } catch (std::exception const&) {
        return serialize(error_response(request_id, "INTERNAL", "Internal error"));
    }
}

void register_core_operations(Dispatcher& d, std::string app_version)
{
    d.register_operation("getEngineInfo", [app_version = std::move(app_version)](json const&) {
        return json{{"appVersion", app_version}, {"engineVersion", core::engine_version()},
            {"protocolVersion", protocol_version}};
    });
}

} // namespace tc::bridge
