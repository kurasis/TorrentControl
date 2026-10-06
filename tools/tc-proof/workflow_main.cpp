// Interactive, developer-only AppService driver. One JSON request/response per
// line; the job runs on the production scheduler while control remains usable.
#include "workflow_fixture.hpp"
#include "tc/core/error.hpp"

#include "workflow_probe.hpp"
#include "session_fixture.hpp"
#include <iostream>

using nlohmann::json;

int main()
{
    try {
        std::string line;
        if (!std::getline(std::cin, line)) return 1;
        json const input = json::parse(line);
        tc::proof::WorkflowProbe probe;
        tc::service::AppService::Options options;
        // The wrapper is enabled only for SMB fault coordination. With no
        // probe the memory workflow uses the ordinary service options.
        if (input.value("probe", false)) options.jobs.payload_factory = [&] { return std::make_unique<tc::proof::ProbeSource>(probe); };
        tc::service::AppService app(options, {});
        tc::proof::SessionFixture session(input);
        std::string id;
        std::size_t job_offset = 0;
        std::cout << json{{"ok", true}, {"ready", true}}.dump() << std::endl;
        while (std::getline(std::cin, line)) {
            try {
                json const command = json::parse(line);
                std::string const op = command.at("operation");
                json result;
                if (op == "sessionPrepare") result = session.prepare(app, command.at("format"));
                else if (op == "sessionEnqueue") result = session.enqueue(app, command.at("kind"), command.at("count"));
                else if (op == "sessionStatus") result = app.retention_summary();
                else if (op == "sessionFinish") result = session.finish_cycle(app);
                else if (op == "sessionPage") result = app.jobs().bridge_page(command.at("offset"), 50);
                else if (op == "sessionDetails") {
                    auto const job = app.jobs().bridge_job(command.at("jobId"));
                    result = app.jobs().text_page(command.at("jobId"), "log", 0, 50);
                    result["details"] = job.at("kind") == "verify"
                        ? app.jobs().verification_page(command.at("jobId"), 0, 50, false)
                        : app.jobs().layout_page(command.at("jobId"), 0, 50);
                }
                else if (op == "scan") result = tc::proof::prepare_fixture(app, input);
                else if (op == "review") result = app.validate_draft();
                else if (op == "create") {
                    job_offset = app.jobs().bridge_page(0, 0).at("total");
                    id = app.start_create(); result["jobId"] = id;
                }
                else if (op == "snapshot") {
                    auto const page = app.jobs().bridge_page(job_offset, 1);
                    result = {{"job", !id.empty() && !page.at("jobs").empty() ? page.at("jobs").at(0) : json(nullptr)}, {"probe", probe.snapshot()}};
                } else if (op == "cancel") app.jobs().cancel(id);
                else if (op == "pause") app.jobs().pause(id);
                else if (op == "resume") app.jobs().resume(id);
                else if (op == "join") { app.jobs().wait_idle(); result = {{"joined", true}, {"probe", probe.snapshot()}}; }
                else if (op == "clear") { app.clear_finished_jobs(); result["remaining"] = app.jobs().bridge_page(0, 50).at("total"); }
                else if (op == "arm") {
                    probe.arm(command.at("site"));
                } else if (op == "release") {
                    probe.release();
                } else if (op == "exit") {
                    app.jobs().cancel_all(); app.jobs().wait_idle();
                    std::cout << json{{"ok", true}, {"result", {{"joined", true}}}}.dump() << std::endl;
                    break;
                } else throw std::runtime_error("Unknown workflow operation");
                std::cout << json{{"ok", true}, {"result", result}}.dump() << std::endl;
            } catch (std::exception const& error) {
                std::cout << json{{"ok", false}, {"error", error.what()}}.dump() << std::endl;
            }
        }
        return 0;
    } catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
