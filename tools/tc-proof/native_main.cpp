// Internal benchmark of the actual native service and serialized dispatcher.
#include "tc/bridge/app_operations.hpp"
#include "tc/bridge/protocol.hpp"
#include "tc/core/manifest.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

using nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct Host final : tc::bridge::HostServices {
    fs::path root;
    std::vector<fs::path> pick_open(OpenKind) override { return {root}; }
    std::optional<fs::path> pick_save(SaveKind, std::string const&, fs::path const&) override { return {}; }
    void show_in_folder(fs::path const&) override {}
    bool open_with_default_app(fs::path const&) override { return false; }
    bool open_url(std::string const&) override { return false; }
};
json stats(std::vector<double> values)
{
    std::sort(values.begin(),values.end());
    return {{"samples",values.size()},{"p50Ms",values[values.size()/2]},
        {"p95Ms",values[(values.size()-1)*95/100]},{"maxMs",values.back()}};
}
std::uint64_t peak_rss()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),&counters,static_cast<DWORD>(sizeof(counters)))) return 0;
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF,&usage) != 0) return 0;
    return static_cast<std::uint64_t>(usage.ru_maxrss)*1024;
#endif
}
int main()
{
    try {
        json input; std::cin >> input;
        Host host; host.root=tc::core::path_from_utf8(input.at("root").get<std::string>());
        tc::service::AppService app({},{});
        tc::bridge::Dispatcher dispatcher;
        tc::bridge::register_app_operations(dispatcher,app,host);
        std::size_t max_response=0;
        double elapsed=0;
        int next=0;
        auto call=[&](std::string const& operation,json payload=json::object()) {
            auto message=json{{"protocolVersion",1},{"requestId",std::to_string(++next)},
                {"operation",operation},{"payload",std::move(payload)}}.dump();
            auto start=Clock::now();
            auto response=dispatcher.handle(message,"https://torrentcontrol.example/index.html");
            elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            max_response=std::max(max_response,response.size());
            auto value=json::parse(response);
            if (!value.at("ok").get<bool>()) throw std::runtime_error("Native benchmark bridge request failed: "+operation);
            return value.at("result");
        };
        auto scan_start=Clock::now();
        call("selectSources",{{"kind","folder"}});
        app.wait_for_scan();
        double scan_ms=std::chrono::duration<double,std::milli>(Clock::now()-scan_start).count();
        auto initial_snapshot = call("getSnapshot");
        double first_snapshot_ms = elapsed;
        if (initial_snapshot["scan"].at("state") != "ready") throw std::runtime_error("Native scan failed");
        auto page=call("getManifestPage",{{"offset",0},{"limit",200},{"filter",""}});
        auto total=page.at("total").get<std::size_t>();
        if (!total) throw std::runtime_error("Native benchmark requires real files");
        std::vector<double> unfiltered,filtered,validation,snapshots;
        for (std::size_t i=0; i<100; ++i) {
            auto offset=(i%2==0 ? i*200 : total-std::min(total,std::size_t(200)))%total;
            page=call("getManifestPage",{{"offset",offset},{"limit",200},{"filter",""}});
            if (page["entries"].size()>200 || page["total"] != total) throw std::runtime_error("Invalid native page");
            unfiltered.push_back(elapsed);
        }
        auto filter=input.value("filter",std::string("-099"));
        page=call("getManifestPage",{{"offset",0},{"limit",200},{"filter",filter}});
        double first_filter=elapsed;
        auto filtered_total=page.at("total").get<std::size_t>();
        for (int i=0;i<50;++i) {
            page=call("getManifestPage",{{"offset",0},{"limit",200},{"filter",filter}});
            if (page["total"] != filtered_total) throw std::runtime_error("Unstable filtered page");
            filtered.push_back(elapsed);
        }
        bool can_create=false;
        for (int i=0;i<3;++i) { auto value=call("validateDraft"); validation.push_back(elapsed); can_create=value.at("canCreate"); }
        for (int i=0;i<5;++i) { call("getSnapshot"); snapshots.push_back(elapsed); }
        std::cout<<json{{"files",total},{"scanMs",scan_ms},{"unfilteredPages",stats(unfiltered)},
            {"firstSnapshotMs",first_snapshot_ms},
            {"firstFilterMs",first_filter},{"filteredFiles",filtered_total},{"filteredPages",stats(filtered)},
            {"validation",stats(validation)},{"snapshot",stats(snapshots)},{"canCreate",can_create},
            {"maxResponseBytes",max_response},{"peakRssBytes",peak_rss()},{"networkRequests",0}}.dump()<<'\n';
        return 0;
    } catch (std::exception const& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
