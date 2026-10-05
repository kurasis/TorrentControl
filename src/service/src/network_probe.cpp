#include "tc/service/diagnostics.hpp"
#include "tc/service/profiles.hpp"
#include "tc/service/storage.hpp"
#include "tc/core/error.hpp"
#include <boost/asio.hpp>
#include <ares.h>
#include <curl/curl.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cctype>
#include <memory>
#include <random>
#include <set>

namespace tc::service {
using nlohmann::json;
namespace asio = boost::asio;
using Clock = std::chrono::steady_clock;
namespace {
struct CurlUrlDelete { void operator()(CURLU* p) const { curl_url_cleanup(p); } };
struct CurlDelete { void operator()(CURL* p) const { curl_easy_cleanup(p); } };
struct MultiDelete { void operator()(CURLM* p) const { curl_multi_cleanup(p); } };
struct HeadersDelete { void operator()(curl_slist* p) const { curl_slist_free_all(p); } };
using UrlHandle = std::unique_ptr<CURLU, CurlUrlDelete>;
void initialize()
{
    static bool const ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK
        && ares_library_init(ARES_LIB_INIT_ALL) == ARES_SUCCESS;
    if (!ready) throw ServiceError("NETWORK_INIT", "Network library initialization failed");
}
[[noreturn]] void invalid_url() { throw ServiceError("INVALID_URL", "Invalid endpoint URL (scheme, host, port or fragment)"); }
std::string part(CURLU* u, CURLUPart p)
{
    char* value = nullptr;
    if (curl_url_get(u, p, &value, 0) != CURLUE_OK) return {};
    std::string result(value);
    curl_free(value);
    return result;
}
std::string label(std::string const& url)
{
    try { return parse_probe_url(url).origin(); }
    catch (...) { return "Invalid endpoint"; }
}
std::string percent(std::string_view value)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += digits[c >> 4]; out += digits[c & 15]; }
    }
    return out;
}
std::string random_bytes(std::size_t size)
{
    std::random_device rd;
    std::string out(size, '\0');
    for (char& c : out) c = static_cast<char>(rd() & 255);
    return out;
}
std::string safe_query(std::string const& query)
{
    static std::set<std::string> const swarm{"info_hash","peer_id","uploaded","downloaded","left","event","port","compact","numwant","key","ip","ipv6","ipv4"};
    std::string result;
    for (std::size_t i = 0; i < query.size();) {
        auto end = query.find('&', i);
        if (end == std::string::npos) end = query.size();
        auto item = query.substr(i, end - i);
        auto eq = item.find('=');
        auto key = item.substr(0, eq);
        int length = 0;
        char* decoded = curl_easy_unescape(nullptr, key.c_str(), static_cast<int>(key.size()), &length);
        std::string decoded_key = decoded ? std::string(decoded, static_cast<std::size_t>(length)) : key;
        curl_free(decoded);
        if (!swarm.contains(decoded_key)) { if (!result.empty()) result += '&'; result += item; }
        i = end + 1;
    }
    return result;
}
struct HttpResult {
    CURLcode code = CURLE_OK;
    long status = 0;
    std::string body, location, content_range, encoding, ip;
    bool capped = false, cancelled = false, header_capped = false;
    std::size_t cap = 65536, headers = 0;
    curl_off_t dns_us = 0, connect_us = 0, tls_us = 0, total_us = 0;
};
size_t body_callback(char* data, size_t size, size_t count, void* context)
{
    auto& r = *static_cast<HttpResult*>(context);
    auto n = size * count;
    if (n > r.cap - r.body.size()) { r.capped = true; return 0; }
    r.body.append(data, n);
    return n;
}
size_t header_callback(char* data, size_t size, size_t count, void* context)
{
    auto& r = *static_cast<HttpResult*>(context);
    auto n = size * count;
    r.headers += n;
    if (r.headers > 32768) { r.header_capped = true; return 0; }
    std::string line(data, n);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        auto name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto first = line.find_first_not_of(" \t", colon + 1);
        auto end = line.find_last_not_of(" \t\r\n");
        auto value = first == std::string::npos || end < first ? "" : line.substr(first, end - first + 1);
        if (name == "location") r.location = value;
        if (name == "content-range") r.content_range = value;
        if (name == "content-encoding") r.encoding = value;
    }
    return n;
}
HttpResult http_request(std::string const& url, NetworkPolicy const& policy, int family,
    bool range, std::size_t cap, std::stop_token stop, Clock::time_point deadline, CURL* easy)
{
    HttpResult r;
    r.cap = cap;
    std::unique_ptr<CURLM, MultiDelete> multi(curl_multi_init());
    if (!easy || !multi) throw ServiceError("NETWORK_INIT", "Cannot allocate network request");
    curl_easy_reset(easy);
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) { r.code = CURLE_OPERATION_TIMEDOUT; return r; }
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_PROXY, policy.http_proxy.c_str());
    curl_easy_setopt(easy, CURLOPT_NOPROXY, ""); // respect explicit proxy even for LAN
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(remaining));
    curl_easy_setopt(easy, CURLOPT_IPRESOLVE, family == 4 ? CURL_IPRESOLVE_V4 : CURL_IPRESOLVE_V6);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(easy, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "TorrentControl/0.1 diagnostics");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, body_callback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &r);
    std::unique_ptr<curl_slist, HeadersDelete> headers(curl_slist_append(nullptr, "Accept-Encoding: identity"));
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.get());
    if (range) curl_easy_setopt(easy, CURLOPT_RANGE, "0-0");
    if (curl_multi_add_handle(multi.get(), easy) != CURLM_OK) throw ServiceError("NETWORK_INIT", "Cannot schedule network request");
    int running = 0;
    do {
        if (stop.stop_requested()) { r.cancelled = true; r.code = CURLE_ABORTED_BY_CALLBACK; break; }
        if (Clock::now() >= deadline) { r.code = CURLE_OPERATION_TIMEDOUT; break; }
        auto code = curl_multi_perform(multi.get(), &running);
        if (code != CURLM_OK) { r.code = CURLE_RECV_ERROR; break; }
        if (running) { int events = 0; (void)curl_multi_poll(multi.get(), nullptr, 0, 20, &events); }
    } while (running);
    if (!running && !r.cancelled && r.code == CURLE_OK) {
        int left = 0;
        while (auto* message = curl_multi_info_read(multi.get(), &left))
            if (message->msg == CURLMSG_DONE) r.code = message->data.result;
    }
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &r.status);
    char* ip = nullptr;
    curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &ip);
    if (ip) r.ip = ip;
    curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME_T, &r.dns_us);
    curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME_T, &r.connect_us);
    curl_easy_getinfo(easy, CURLINFO_APPCONNECT_TIME_T, &r.tls_us);
    curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME_T, &r.total_us);
    curl_multi_remove_handle(multi.get(), easy);
    return r;
}
std::string network_state(HttpResult const& r)
{
    if (r.cancelled) return "cancelled";
    if (r.code == CURLE_OPERATION_TIMEDOUT) return "no-response";
    if (r.code == CURLE_COULDNT_RESOLVE_HOST || r.code == CURLE_COULDNT_RESOLVE_PROXY) return "dns-error";
    if (r.code == CURLE_PEER_FAILED_VERIFICATION || r.code == CURLE_SSL_CONNECT_ERROR || r.code == CURLE_SSL_CACERT_BADFILE) return "tls-error";
    if (r.header_capped) return "invalid-response";
    if (r.code != CURLE_OK && !r.capped) return "transport-error";
    if (!r.encoding.empty() && r.encoding != "identity") return "invalid-response";
    if (r.status == 401 || r.status == 403 || r.status == 407) return "responding-restricted";
    return {};
}
std::string tracker_state(HttpResult const& r, bool scrape)
{
    auto state = network_state(r);
    if (!state.empty()) return state;
    if (scrape && (r.status == 404 || r.status == 405 || r.status == 501)) return "scrape-unsupported";
    if (r.capped || r.status != 200) return "invalid-response";
    try {
        auto v = core::bencode::parse(r.body, {32, 10000});
        if (!v.is_dictionary()) return "invalid-response";
        if (auto* failure = v.find("failure reason"); failure && failure->is_string()) return "responding-restricted";
        if (scrape) {
            auto const* files = v.find("files");
            if (!files || !files->is_dictionary()) return "invalid-response";
            for (auto const& entry : files->entries()) {
                if (entry.key.size() != 20 || !entry.value.is_dictionary()) return "invalid-response";
                for (auto const* key : {"complete", "incomplete", "downloaded"}) {
                    auto* count = entry.value.find(key);
                    if (!count || !count->as_int64() || *count->as_int64() < 0) return "invalid-response";
                }
            }
            return "protocol-responding";
        }
        auto* interval = v.find("interval");
        auto* peers = v.find("peers");
        if (interval && interval->as_int64() && *interval->as_int64() >= 0 && peers && peers->is_string() && peers->text().size() % 6 == 0)
            return "protocol-responding";
    } catch (core::CoreError const&) {}
    return "invalid-response";
}
std::string seed_state(HttpResult const& r, std::uint64_t expected)
{
    auto state = network_state(r);
    if (!state.empty()) return state;
    if (r.status == 416 && expected == 0 && r.content_range == "bytes */0") return "empty-file";
    if (r.status == 200) return expected == 0 && r.body.empty() && !r.capped ? "empty-file" : "range-ignored";
    if (r.status != 206 || r.capped) return r.status == 416 ? "range-unsatisfied" : "invalid-response";
    if (r.content_range != "bytes 0-0/" + std::to_string(expected) || expected == 0 || r.body.size() != 1) return "invalid-response";
    return "range-supported";
}
struct DnsState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int status = ARES_SUCCESS;
    std::vector<asio::ip::address> addresses;
};
void dns_callback(void* context, int status, int, ares_addrinfo* result)
{
    auto& state = *static_cast<DnsState*>(context);
    { std::lock_guard lock(state.mutex);
      state.status = status;
      if (result) for (auto* node = result->nodes; node; node = node->ai_next) {
          if (node->ai_family == AF_INET) {
              asio::ip::address_v4::bytes_type bytes{};
              std::memcpy(bytes.data(), &reinterpret_cast<sockaddr_in*>(node->ai_addr)->sin_addr, bytes.size());
              state.addresses.emplace_back(asio::ip::address_v4(bytes));
          } else if (node->ai_family == AF_INET6) {
              asio::ip::address_v6::bytes_type bytes{};
              auto* addr = reinterpret_cast<sockaddr_in6*>(node->ai_addr);
              std::memcpy(bytes.data(), &addr->sin6_addr, bytes.size());
              state.addresses.emplace_back(asio::ip::address_v6(bytes, addr->sin6_scope_id));
          }
      }
      state.done = true;
    }
    if (result) ares_freeaddrinfo(result);
    state.cv.notify_all();
}
std::vector<asio::ip::address> resolve_udp(std::string const& host, NetworkPolicy const& policy, std::stop_token stop)
{
    DnsState state;
    ares_channel_t* channel = nullptr;
    ares_options options{};
    options.evsys = ARES_EVSYS_DEFAULT;
    options.timeout = static_cast<int>(policy.timeout.count());
    options.tries = 1;
    if (ares_init_options(&channel, &options, ARES_OPT_EVENT_THREAD | ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES) != ARES_SUCCESS)
        throw ServiceError("DNS_ERROR", "Cannot initialize DNS resolver");
    ares_addrinfo_hints hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    ares_getaddrinfo(channel, host.c_str(), nullptr, &hints, dns_callback, &state);
    auto deadline = Clock::now() + policy.timeout;
    { std::unique_lock lock(state.mutex);
      while (!state.done && !stop.stop_requested() && Clock::now() < deadline) state.cv.wait_for(lock, std::chrono::milliseconds(20));
    }
    ares_cancel(channel);
    ares_destroy(channel);
    if (stop.stop_requested()) throw ServiceError("CANCELLED", "Diagnostic cancelled");
    if (!state.done || state.status != ARES_SUCCESS || state.addresses.empty()) throw ServiceError("DNS_ERROR", "Endpoint DNS resolution failed");
    return state.addresses;
}
void put32(std::string& bytes, std::uint32_t value)
{
    for (int shift : {24,16,8,0}) bytes += static_cast<char>((value >> shift) & 255);
}
std::uint32_t get32(unsigned char const* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
json udp_probe(ParsedUrl const& url, NetworkPolicy const& policy, std::stop_token stop)
{
    json attempts = json::array();
    if (!policy.http_proxy.empty()) return {{"state","probe-unsupported"},{"operation","udp-connect"},{"attempts", attempts}};
    auto dns_started = Clock::now();
    auto addresses = resolve_udp(url.host, policy, stop);
    auto dns_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - dns_started).count();
    std::set<int> tested;
    auto port = static_cast<unsigned short>(std::stoul(url.port));
    for (auto const& address : addresses) {
        if (stop.stop_requested()) break;
        int family = address.is_v4() ? 4 : 6;
        if (!tested.insert(family).second) continue;
        auto started = Clock::now();
        asio::io_context io;
        asio::ip::udp::socket socket(io);
        asio::ip::udp::endpoint endpoint(address, port);
        boost::system::error_code ec;
        socket.open(endpoint.protocol(), ec);
        std::string state = ec ? "family-unavailable" : "no-response";
        if (!ec) socket.non_blocking(true, ec);
        bool mismatch = false;
        int sent = 0;
        for (int round = 0; !ec && round < (policy.udp_retry ? 2 : 1); ++round) {
            if (stop.stop_requested()) break;
            std::string bytes("\x00\x00\x04\x17\x27\x10\x19\x80", 8);
            std::random_device rd;
            auto transaction = static_cast<std::uint32_t>(rd());
            put32(bytes, 0); put32(bytes, transaction);
            socket.send_to(asio::buffer(bytes), endpoint, 0, ec);
            ++sent;
            auto deadline = Clock::now() + policy.timeout * (round + 1);
            while (!ec && Clock::now() < deadline && !stop.stop_requested()) {
                unsigned char reply[4096]{};
                asio::ip::udp::endpoint sender;
                auto size = socket.receive_from(asio::buffer(reply), sender, 0, ec);
                if (ec == asio::error::would_block || ec == asio::error::try_again) {
                    ec.clear(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue;
                }
                if (ec) break;
                if (sender != endpoint || size < 8 || get32(reply + 4) != transaction) { mismatch = true; continue; }
                auto action = get32(reply);
                if (action == 0 && size >= 16) { state = "protocol-responding"; break; }
                if (action == 3) { state = "responding-restricted"; break; }
                mismatch = true;
            }
            if (state != "no-response" || stop.stop_requested()) break;
        }
        if (stop.stop_requested()) state = "cancelled";
        else if (ec) state = "transport-error";
        else if (state == "no-response" && mismatch) state = "invalid-response";
        attempts.push_back({{"family", family}, {"state", state}, {"operation","udp-connect"},
            {"dnsMs", dns_ms},
            {"latencyMs", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()}, {"requests", sent}});
    }
    for (int family : {4,6}) if (!tested.contains(family) && !stop.stop_requested())
        attempts.push_back({{"family",family},{"state","family-unavailable"},{"operation","udp-connect"},{"dnsMs",dns_ms},{"requests",0}});
    std::string state = attempts.empty() ? "family-unavailable" : attempts.front().at("state").get<std::string>();
    for (auto const& a : attempts) if (a["state"] == "protocol-responding" || a["state"] == "responding-restricted") state = a["state"];
    if (stop.stop_requested()) state = "cancelled";
    return {{"state", state}, {"operation","udp-connect"}, {"authorization","unknown"}, {"acceptsTorrent","unknown"}, {"attempts", attempts}};
}
} // namespace

ParsedUrl parse_probe_url(std::string const& text)
{
    initialize();
    if (text.empty() || text.size() > 8192) invalid_url();
    for (unsigned char c : text) if (c <= 32 || c == 127 || c == '\\') invalid_url();
    UrlHandle u(curl_url());
    if (!u || curl_url_set(u.get(), CURLUPART_URL, text.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK) invalid_url();
    if (!part(u.get(), CURLUPART_FRAGMENT).empty() || text.find('#') != std::string::npos) invalid_url();
    ParsedUrl result;
    result.scheme = part(u.get(), CURLUPART_SCHEME);
    result.host = part(u.get(), CURLUPART_HOST);
    std::transform(result.host.begin(), result.host.end(), result.host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (result.host.starts_with('[')) result.host = result.host.substr(1, result.host.size() - 2);
    result.port = part(u.get(), CURLUPART_PORT);
    if (result.port.empty()) result.port = result.scheme == "https" ? "443" : result.scheme == "http" ? "80" : "";
    unsigned port = 0;
    auto converted = std::from_chars(result.port.data(), result.port.data() + result.port.size(), port);
    if (result.host.empty()) invalid_url();
    if (!(result.port.empty() && result.scheme != "http" && result.scheme != "https" && result.scheme != "udp")
        && (converted.ec != std::errc() || converted.ptr != result.port.data() + result.port.size() || port == 0 || port > 65535)) invalid_url();
    result.path = part(u.get(), CURLUPART_PATH);
    result.query = part(u.get(), CURLUPART_QUERY);
    result.userinfo = part(u.get(), CURLUPART_USER);
    auto password = part(u.get(), CURLUPART_PASSWORD);
    if (!password.empty()) result.userinfo += ':' + password;
    return result;
}
std::string ParsedUrl::origin() const { return scheme + "://" + (host.find(':') != std::string::npos ? '[' + host + ']' : host) + (port.empty() ? "" : ':' + port); }
std::string ParsedUrl::str() const
{
    auto hostpart = host.find(':') != std::string::npos ? '[' + host + ']' : host;
    return scheme + "://" + (userinfo.empty() ? "" : userinfo + '@') + hostpart + (port.empty() ? "" : ':' + port) + path + (query.empty() ? "" : '?' + query);
}
std::string resolve_web_seed(std::string const& base, core::Metainfo const& meta, core::MetainfoFile const& file)
{
    return resolve_web_seed(base, meta.name(), file);
}
std::string resolve_web_seed(std::string const& base, std::string const& name, core::MetainfoFile const& file)
{
    auto url = parse_probe_url(base);
    bool single = file.path.empty();
    if (single && !url.path.ends_with('/')) return url.str();
    if (!single && !url.path.ends_with('/')) throw ServiceError("INVALID_URL", "A multifile web seed requires a directory URL ending in slash");
    // The engine's effective v2 layout already decides whether name is a root
    // or the single file. Never prepend name twice or request virtual pads.
    url.path += percent(name);
    for (auto const& component : file.path) url.path += '/' + percent(component);
    return url.str();
}
std::vector<ProbeTarget> torrent_probe_targets(core::Metainfo const& meta, std::string const& kind)
{
    std::vector<ProbeTarget> targets;
    std::set<std::string> seen;
    auto add = [&](std::string const& url, std::string type, core::MetainfoFile const* file = nullptr) {
        if (targets.size() >= 256) throw ServiceError("RESOURCE_LIMIT", "At most 256 diagnostic targets per run");
        auto identity = type + ':' + url;
        if (seen.insert(identity).second) targets.push_back({"endpoint-" + std::to_string(targets.size() + 1), url, std::move(type), file ? file->torrent_path : "", file ? file->length : 0});
    };
    if (kind == "trackers") {
        auto const* tiers = meta.root().find("announce-list");
        if (tiers && tiers->is_list()) for (auto const& tier : tiers->items()) {
            if (tier.is_list()) for (auto const& u : tier.items()) if (u.is_string()) add(u.text(), "tracker");
        }
        if (auto const* u = meta.root().find("announce"); u && u->is_string()) add(u->text(), "tracker");
    } else if (kind == "web-seeds") {
        auto files = core::metainfo_files(meta);
        std::vector<core::MetainfoFile const*> real;
        for (auto const& file : files) if (!file.pad) real.push_back(&file);
        // First, a nested file when present, and last; at most three per base.
        std::vector<core::MetainfoFile const*> samples;
        if (!real.empty()) samples.push_back(real.front());
        auto nested = std::find_if(real.begin(), real.end(), [](auto* file) { return file->path.size() > 1; });
        if (nested != real.end() && std::find(samples.begin(), samples.end(), *nested) == samples.end()) samples.push_back(*nested);
        if (!real.empty() && std::find(samples.begin(), samples.end(), real.back()) == samples.end()) samples.push_back(real.back());
        auto seed = [&](core::bencode::Value const& value) {
            if (!value.is_string()) return;
            for (auto* file : samples) {
                try { add(resolve_web_seed(value.text(), meta, *file), "bep19", file); }
                catch (ServiceError const&) { add(value.text(), "bep19-invalid-base", file); }
                if (!targets.empty()) targets.back().total_files = real.size();
            }
        };
        if (auto* urls = meta.root().find("url-list")) {
            if (urls->is_string()) seed(*urls);
            else if (urls->is_list()) for (auto const& url : urls->items()) seed(url);
        }
        if (auto* seeds = meta.root().find("httpseeds"); seeds && seeds->is_list())
            for (auto const& url : seeds->items()) if (url.is_string()) add(url.text(), meta.format() == core::MetainfoFormat::V2 ? "bep17-unsupported-v2" : "bep17-transport");
    } else throw ServiceError("INVALID_ARGUMENT", "Diagnostic kind must be trackers or web-seeds");
    return targets;
}
json probe_endpoint(ProbeTarget const& target, NetworkPolicy const& policy, std::stop_token stop)
{
    auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    json result{{"id", target.id}, {"endpoint", label(target.url)}, {"kind", target.kind}, {"file", target.torrent_path},
        {"totalFiles", target.total_files},
        {"checkedAt", std::to_string(now)}, {"network", policy.http_proxy.empty() ? "direct" : "http-proxy"},
        {"proxy", policy.http_proxy.empty() ? "" : label(policy.http_proxy)}, {"cached", false}, {"integrity","not-verified"}, {"acceptsTorrent","unknown"}};
    try {
        initialize();
        if (stop.stop_requested()) { result["state"] = "cancelled"; return result; }
        auto url = parse_probe_url(target.url);
        if (target.kind == "bep19-invalid-base" || target.kind == "bep17-unsupported-v2") { result["state"] = "probe-unsupported"; return result; }
        if (url.scheme == "udp" && target.kind == "tracker") { result.update(udp_probe(url, policy, stop)); return result; }
        if (url.scheme != "http" && url.scheme != "https") { result["state"] = "probe-unsupported"; return result; }
        bool scrape = false;
        if (target.kind == "tracker") {
            url.query = safe_query(url.query);
            auto slash = url.path.find_last_of('/');
            if (slash != std::string::npos && url.path.substr(slash + 1).starts_with("announce")) {
                url.path.replace(slash + 1, 8, "scrape");
                scrape = true;
                if (!url.query.empty()) url.query += '&';
                url.query += "info_hash=" + percent(random_bytes(20));
            }
        }
        result["operation"] = target.kind == "tracker" ? (scrape ? "http-scrape" : "http-endpoint") : target.kind == "bep19" ? "range-0-0" : "transport-only";
        json attempts = json::array();
        for (int family : {4,6}) {
            if (stop.stop_requested()) break;
            std::unique_ptr<CURL, CurlDelete> easy(curl_easy_init());
            auto deadline = Clock::now() + policy.timeout / 2;
            auto current = url.str();
            HttpResult response;
            bool blocked = false;
            for (int redirects = 0; redirects <= 3; ++redirects) {
                response = http_request(current, policy, family, target.kind == "bep19", target.kind == "catalog" ? 1024 * 1024 : target.kind == "bep19" ? 4096 : 65536, stop, deadline, easy.get());
                if (response.status < 300 || response.status >= 400 || response.location.empty() || response.code != CURLE_OK) break;
                UrlHandle redirect(curl_url());
                if (curl_url_set(redirect.get(), CURLUPART_URL, current.c_str(), 0) != CURLUE_OK
                    || curl_url_set(redirect.get(), CURLUPART_URL, response.location.c_str(), 0) != CURLUE_OK) { blocked = true; break; }
                auto next = parse_probe_url(part(redirect.get(), CURLUPART_URL));
                // Conservative redirect policy: same origin only. No credential
                // forwarding, public-to-LAN redirect, scheme downgrade or proxy bypass.
                if (next.origin() != url.origin() || next.userinfo != url.userinfo || redirects == 3) { blocked = true; break; }
                current = next.str();
            }
            std::string state;
            if (blocked) state = "redirect-blocked";
            else if (target.kind == "tracker") state = tracker_state(response, scrape);
            else if (target.kind == "bep19") state = seed_state(response, target.expected_length);
            else {
                state = network_state(response);
                if (state.empty()) state = !response.capped && response.status >= 200 && response.status < 300 ? "transport-responding" : "invalid-response";
            }
            json observation{{"family", family}, {"state", state}, {"httpStatus", response.status},
                {"dnsMs", response.dns_us / 1000}, {"connectMs", response.connect_us / 1000},
                {"tlsMs", response.tls_us / 1000}, {"latencyMs", response.total_us / 1000},
                {"bytesKept", response.body.size()}, {"bodyCapped", response.capped}};
            attempts.push_back(observation);
            if (target.kind == "catalog" && state == "transport-responding") {
                std::vector<std::string> urls;
                std::set<std::string> unique;
                std::size_t start = 0;
                while (start < response.body.size()) {
                    auto end = response.body.find('\n', start); if (end == std::string::npos) end = response.body.size();
                    auto line = response.body.substr(start, end - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) {
                        auto entry = parse_probe_url(line);
                        if ((entry.scheme != "http" && entry.scheme != "https" && entry.scheme != "udp") || !entry.userinfo.empty() || !entry.query.empty() || redact_url(line) != line) throw ServiceError("INVALID_CATALOG", "Catalog includes an unsupported or authenticated endpoint");
                        if (unique.insert(line).second) urls.push_back(line);
                        if (urls.size() > 512 || end - start > 8192) throw ServiceError("INVALID_CATALOG", "Catalog exceeds line or entry limit");
                    }
                    start = end + 1;
                }
                if (urls.empty()) throw ServiceError("INVALID_CATALOG", "Catalog contains no valid endpoints");
                result["catalog"] = {{"urls",urls},{"source",target.url},{"fetchedAt",std::to_string(now)},
                    {"checksum",core::to_hex(core::sha256(response.body))},{"sourceDate",nullptr}};
                break;
            }
        }
        std::string state = stop.stop_requested() ? "cancelled" : attempts.empty() ? "no-response" : attempts.front()["state"].get<std::string>();
        for (auto const& a : attempts) {
            auto candidate = a["state"].get<std::string>();
            if (candidate == "protocol-responding" || candidate == "responding-restricted" || candidate == "range-supported" || candidate == "empty-file" || candidate == "transport-responding") { state = candidate; break; }
        }
        if (stop.stop_requested()) state = "cancelled";
        result["state"] = state;
        result["attempts"] = std::move(attempts);
        if (target.kind == "bep19") result["expectedLength"] = std::to_string(target.expected_length);
    } catch (ServiceError const& e) {
        result["state"] = e.code() == "CANCELLED" ? "cancelled" : e.code() == "DNS_ERROR" ? "dns-error" : "invalid-response";
        result["errorCode"] = e.code();
    } catch (...) { result["state"] = "transport-error"; }
    return result;
}
} // namespace tc::service
