#include "tc/service/diagnostics.hpp"
#include "tc/service/profiles.hpp"
#include "tc/service/storage.hpp"
#include <algorithm>
#include <set>

namespace tc::service {
using nlohmann::json;
namespace {
std::string cache_key(ProbeTarget const& t, NetworkPolicy const& p)
{
    // Cache keys stay native; credentials never appear in results or logs.
    return t.kind + '\n' + t.url + '\n' + std::to_string(t.expected_length) + '\n' + p.http_proxy + '\n' + std::to_string(p.timeout.count()) + (p.udp_retry ? "retry" : "quick");
}
}
DiagnosticsService::DiagnosticsService(Sink sink) : sink_(std::move(sink))
{
    auto const& builtin = builtin_tracker_catalog();
    catalog_ = {{"urls", builtin.urls}, {"source", builtin.source_url}, {"sourceDate", builtin.source_date},
        {"fetchedAt", nullptr}, {"checksum", nullptr}, {"locallyCheckedAt", nullptr}};
    for (int i = 0; i < 4; ++i) workers_.emplace_back([this](std::stop_token stop) { worker(stop); });
}
DiagnosticsService::~DiagnosticsService()
{
    { std::lock_guard lock(mutex_); for (auto& [id, run] : runs_) { (void)id; run.stop.request_stop(); } }
    for (auto& worker_thread : workers_) worker_thread.request_stop();
    cv_.notify_all();
    workers_.clear();
}
std::string DiagnosticsService::start(std::vector<ProbeTarget> targets, NetworkPolicy policy)
{
    if (targets.empty() || targets.size() > 256) throw ServiceError("INVALID_ARGUMENT", "Select 1 to 256 diagnostic targets");
    if (policy.timeout < std::chrono::milliseconds(50) || policy.timeout > std::chrono::seconds(30))
        throw ServiceError("INVALID_ARGUMENT", "Diagnostic timeout must be between 50ms and 30s");
    if (!policy.http_proxy.empty()) {
        auto proxy = parse_probe_url(policy.http_proxy);
        if (proxy.scheme != "http" || !proxy.query.empty() || proxy.path != "/")
            throw ServiceError("INVALID_PROXY", "Use an explicit HTTP proxy origin");
    }
    std::set<std::string> unique;
    std::vector<ProbeTarget> deduplicated;
    for (auto& target : targets) if (unique.insert(cache_key(target, policy)).second) deduplicated.push_back(std::move(target));
    std::lock_guard lock(mutex_);
    // Only one run at a time: duplicate clicks cannot double endpoint traffic.
    for (auto const& [id, run] : runs_) {
        (void)id;
        if (run.next < run.targets.size() || run.running) throw ServiceError("DIAGNOSTICS_BUSY", "Cancel or finish the current diagnostic run first");
    }
    while (runs_.size() >= 8) runs_.erase(std::min_element(runs_.begin(), runs_.end(), [](auto const& a, auto const& b) { return a.second.sequence < b.second.sequence; }));
    auto sequence = next_;
    auto id = "diagnostic-" + std::to_string(next_++);
    Run run;
    run.id = id;
    run.sequence = sequence;
    run.targets = std::move(deduplicated);
    run.policy = std::move(policy);
    runs_.emplace(id, std::move(run));
    cv_.notify_all();
    return id;
}
void DiagnosticsService::cancel(std::string const& id)
{
    std::lock_guard lock(mutex_);
    auto it = runs_.find(id);
    if (it == runs_.end()) throw ServiceError("NOT_FOUND", "No such diagnostic run");
    it->second.stop.request_stop();
    cv_.notify_all();
}
json DiagnosticsService::summary_locked(Run const& run) const
{
    auto state = run.results.size() == run.targets.size() && run.running == 0 ? (run.stop.stop_requested() ? "cancelled" : "completed") : "running";
    return {{"id", run.id}, {"sequence",std::to_string(run.sequence)}, {"state", state}, {"total",run.targets.size()}, {"completed",run.results.size()},
        {"network",run.policy.http_proxy.empty() ? "direct" : "http-proxy"}};
}
json DiagnosticsService::snapshot() const
{
    std::lock_guard lock(mutex_);
    json runs = json::array();
    std::vector<Run const*> ordered;
    for (auto const& [id, run] : runs_) { (void)id; ordered.push_back(&run); }
    std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->sequence < b->sequence; });
    for (auto* run : ordered) runs.push_back(summary_locked(*run));
    return runs;
}
json DiagnosticsService::page(std::string const& id, std::size_t offset, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    auto it = runs_.find(id);
    if (it == runs_.end()) throw ServiceError("NOT_FOUND", "No such diagnostic run");
    json rows = json::array();
    for (std::size_t i = offset; i < it->second.results.size() && rows.size() < std::min(limit, std::size_t(50)); ++i) rows.push_back(it->second.results[i]);
    return {{"run",summary_locked(it->second)},{"rows",std::move(rows)},{"total",it->second.results.size()}};
}
json DiagnosticsService::catalog() const { std::lock_guard lock(mutex_); return catalog_; }
void DiagnosticsService::worker(std::stop_token shutdown)
{
    while (!shutdown.stop_requested()) {
        ProbeTarget target;
        NetworkPolicy policy;
        std::string id;
        std::stop_token stop;
        json result = nullptr;
        { std::unique_lock lock(mutex_);
          cv_.wait(lock, shutdown, [&] {
              return std::any_of(runs_.begin(), runs_.end(), [](auto const& pair) { return pair.second.next < pair.second.targets.size(); });
          });
          if (shutdown.stop_requested()) break;
          auto it = std::find_if(runs_.begin(), runs_.end(), [](auto const& pair) { return pair.second.next < pair.second.targets.size(); });
          if (it == runs_.end()) continue;
          auto& run = it->second;
          id = run.id; target = run.targets[run.next++]; policy = run.policy; stop = run.stop.get_token(); ++run.running;
          auto key = cache_key(target, policy);
          if (!policy.refresh && target.kind != "catalog" && !stop.stop_requested()) {
              auto cached = cache_.find(key);
              if (cached != cache_.end() && cached->second.expires > std::chrono::steady_clock::now()) {
                  result = cached->second.result;
                  result["id"] = target.id; result["file"] = target.torrent_path; result["cached"] = true;
                  result["totalFiles"] = target.total_files;
              }
          }
        }
        if (result.is_null()) result = probe_endpoint(target, policy, stop);
        json event;
        { std::lock_guard lock(mutex_);
          auto& run = runs_.at(id);
          --run.running;
          if (result.contains("catalog") && result["state"] == "transport-responding") catalog_ = result["catalog"];
          result.erase("catalog"); // raw catalog data is not a diagnostics export
          if (result["state"] != "cancelled" && target.kind != "catalog") {
              if (cache_.size() >= 1024) cache_.clear();
              cache_[cache_key(target, policy)] = {result, std::chrono::steady_clock::now() + std::chrono::minutes(10)};
          }
          run.results.push_back(result);
          event = {{"type","diagnostics"},{"run",summary_locked(run)}};
        }
        if (sink_) { try { sink_(event); } catch (...) {} }
        cv_.notify_all();
    }
}
} // namespace tc::service
