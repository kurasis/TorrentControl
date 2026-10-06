#pragma once

#include "workflow_fixture.hpp"

namespace tc::proof {
class SessionFixture {
public:
    explicit SessionFixture(nlohmann::json input) : input_(std::move(input)) {}
    nlohmann::json prepare(service::AppService& app, std::string const& format) {
        input_["format"] = format;
        input_["replace"] = true;
        auto prepared = prepare_fixture(app, input_);
        app.start_create();
        app.jobs().wait_idle();
        auto const page = app.jobs().bridge_page(0, 1);
        if (page.at("jobs").size() != 1 || !page.at("jobs").at(0).at("state").get<std::string>().starts_with("Succeeded"))
            throw std::runtime_error("Session baseline creation failed");
        torrent_id_ = app.open_torrent(core::path_from_utf8(input_.at("output").get<std::string>())).at("id");
        app.clear_finished_jobs();
        return prepared;
    }
    nlohmann::json enqueue(service::AppService& app, std::string const& kind, std::size_t count) {
        if (count == 0 || count > 100) throw std::runtime_error("Session waves require 1..100 jobs");
        auto const root = core::path_from_utf8(input_.at("root").get<std::string>());
        auto const folder = core::path_from_utf8(input_.at("batchOutput").get<std::string>());
        for (std::size_t i = 0; i < count; ++i) {
            if (kind == "create") app.start_create();
            else if (kind == "verify") app.verify_torrent(torrent_id_, root);
            else if (kind == "batch") {
                app.plan_batch("single", "replace", folder);
                auto ids = app.start_batch();
                if (ids.size() != 1) throw std::runtime_error("A session batch must contain exactly one real job");
            } else throw std::runtime_error("Unknown session job kind");
        }
        return {{"enqueued", count}, {"kind", kind}};
    }
    nlohmann::json finish_cycle(service::AppService& app) {
        app.jobs().wait_idle();
        auto result = app.retention_summary();
        if (result.at("jobs") != 1000 || result.at("succeeded") != 1000 || result.at("created") != 700
            || result.at("verified") != 300 || result.at("failed") != 0 || result.at("cancelled") != 0)
            throw std::runtime_error("The 1000-job session did not complete the expected real work");
        for (auto const* key : {"createSpecs", "verifySpecs", "inputManifestEntries", "verifyInputBytes", "jobThreads", "running"})
            if (result.at(key) != 0) throw std::runtime_error(std::string("Completed session retained ") + key);
        if (result.at("workers").get<std::size_t>() > 8) throw std::runtime_error("Session worker pool exceeded its bound");
        // Results must remain readable after the frozen inputs have been released.
        for (std::size_t offset : {0u, 450u, 950u}) {
            auto const page = app.jobs().bridge_page(offset, 50);
            if (page.at("jobs").size() != 50 || page.at("total") != 1000)
                throw std::runtime_error("Session history page was lost");
            auto const& job = page.at("jobs").at(0);
            auto const id = job.at("id").get<std::string>();
            if (app.jobs().text_page(id, "log", 0, 50).at("rows").empty())
                throw std::runtime_error("Session log was lost");
            auto const details = job.at("kind") == "verify" ? app.jobs().verification_page(id, 0, 50, false)
                : app.jobs().layout_page(id, 0, 50);
            if (details.at("total").get<std::size_t>() < 32) throw std::runtime_error("Session file report was lost");
        }
        auto const artifacts = core::path_from_utf8(input_.at("artifactRoot").get<std::string>());
        auto const format = input_.at("format").get<std::string>();
        std::filesystem::copy_file(core::path_from_utf8(input_.at("output").get<std::string>()), artifacts / (format + ".torrent"));
        std::filesystem::copy_file(core::path_from_utf8(input_.at("batchOutput").get<std::string>()) / "Collection.torrent",
            artifacts / (format + "-batch.torrent"));
        result["format"] = format;
        result["historyPagesChecked"] = {0, 450, 950};
        return result;
    }
private:
    nlohmann::json input_;
    std::string torrent_id_;
};
}
