// Internal deterministic network proof runner, not shipped.
#include "tc/service/diagnostics.hpp"
#include "tc/service/storage.hpp"
#include "tc/service/app_service.hpp"
#include <iostream>
using nlohmann::json;
int main()
{
    try {
        json input; std::cin >> input;
        tc::service::NetworkPolicy policy;
        policy.timeout = std::chrono::milliseconds(input.value("timeoutMs", 500));
        policy.http_proxy = input.value("httpProxy", "");
        policy.udp_retry = input.value("udpRetry", false);
        policy.refresh = input.value("refresh", false);
        std::vector<tc::service::ProbeTarget> targets;
        for (auto const& t : input.at("targets")) targets.push_back({t.value("id","test"),t.at("url"),t.value("kind","tracker"),t.value("file",""),t.value("expectedLength",std::uint64_t(0))});
        tc::service::AppService app({}, {});
        auto& service = app.diagnostics();
        json output = json::array();
        for (int repeat = 0; repeat < input.value("repeat",1); ++repeat) {
            auto id = service.start(targets, policy);
            auto started = std::chrono::steady_clock::now();
            bool cancelled = false;
            for (;;) {
                if (input.contains("cancelAfterMs") && !cancelled && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(input.at("cancelAfterMs").get<int>())) {
                    service.cancel(id); cancelled = true;
                }
                auto page = service.page(id, 0, 50);
                if (page["run"]["state"] != "running") { output.push_back(page); break; }
                if (std::chrono::steady_clock::now() - started > std::chrono::seconds(100)) throw std::runtime_error("network proof deadline");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        json result{{"runs",output},{"catalog",service.catalog()}};
        if (input.value("catalogApply",false)) {
            auto trackers = app.draft_json()["trackers"];
            trackers[0]["enabled"] = false;
            trackers.push_back({{"url","https://custom.example/announce?passkey=keep"},{"tier",77},{"enabled",true}});
            trackers.push_back({{"url","udp://127.0.0.1:1337/announce"},{"tier",88},{"enabled",false}});
            app.update_draft({{"trackers",trackers}}, std::nullopt);
            auto plan = app.plan_catalog_apply();
            auto checksum = plan.at("checksum").get<std::string>();
            auto refused = [&](auto&& operation) {
                try { operation(); return std::string("unexpected-success"); }
                catch (tc::service::ServiceError const& error) { return error.code(); }
            };
            result["staleCatalog"] = refused([&] { app.apply_catalog(std::string(64,'0'),std::nullopt); });
            result["staleRevision"] = refused([&] { app.apply_catalog(checksum,0); });
            result["catalogPlan"] = plan;
            result["catalogApplied"] = app.apply_catalog(checksum,std::stoull(plan["draftRevision"].get<std::string>()));
            app.update_draft({{"private",true}},std::nullopt);
            result["privateCatalog"] = refused([&] { app.apply_catalog(checksum,std::nullopt); });
            result["privatePlan"] = app.plan_catalog_apply();
        }
        std::cout << result.dump() << '\n';
        return 0;
    } catch (std::exception const&) { std::cerr << "Network proof failed\n"; return 1; }
}
