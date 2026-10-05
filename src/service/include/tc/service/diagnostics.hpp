#pragma once

#include "tc/core/metainfo.hpp"
#include "tc/service/jobs.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace tc::service {

struct NetworkPolicy {
    // Empty means direct. No implicit environment proxy or direct fallback.
    std::string http_proxy;
    std::chrono::milliseconds timeout{15000};
    bool udp_retry = false; // quick connect only; optional second 30s window
    bool refresh = false;
};

struct ProbeTarget {
    std::string id;
    std::string url;
    std::string kind; // tracker, bep19, bep17-transport, catalog
    std::string torrent_path;
    std::uint64_t expected_length = 0;
    std::size_t total_files = 0;
};

struct ParsedUrl {
    std::string scheme, host, port, path, query, userinfo;
    std::string origin() const;
    std::string str() const;
};
ParsedUrl parse_probe_url(std::string const& url);
std::string resolve_web_seed(std::string const& base, core::Metainfo const& meta, core::MetainfoFile const& file);
std::string resolve_web_seed(std::string const& base, std::string const& name, core::MetainfoFile const& file);
std::vector<ProbeTarget> torrent_probe_targets(core::Metainfo const& meta, std::string const& kind);
nlohmann::json probe_endpoint(ProbeTarget const& target, NetworkPolicy const& policy, std::stop_token stop);

// Last valid catalog survives failed fetch/parse; applying to a draft is a
// separate reviewed operation. No periodic updater is implicitly enabled.
class DiagnosticsService {
public:
    using Sink = std::function<void(nlohmann::json const&)>;
    explicit DiagnosticsService(Sink sink = {});
    ~DiagnosticsService();
    std::string start(std::vector<ProbeTarget> targets, NetworkPolicy policy);
    void cancel(std::string const& id);
    nlohmann::json snapshot() const;
    nlohmann::json page(std::string const& id, std::size_t offset, std::size_t limit) const;
    nlohmann::json catalog() const;
private:
    struct Run {
        std::string id;
        std::uint64_t sequence = 0;
        NetworkPolicy policy;
        std::vector<ProbeTarget> targets;
        nlohmann::json results = nlohmann::json::array();
        std::size_t next = 0, running = 0;
        std::stop_source stop;
    };
    struct Cached { nlohmann::json result; std::chrono::steady_clock::time_point expires; };
    void worker(std::stop_token shutdown);
    nlohmann::json summary_locked(Run const& run) const;
    Sink sink_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::map<std::string, Run> runs_;
    std::map<std::string, Cached> cache_;
    nlohmann::json catalog_;
    std::uint64_t next_ = 1;
    std::vector<std::jthread> workers_;
};

} // namespace tc::service
