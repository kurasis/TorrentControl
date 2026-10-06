// Interactive, developer-only AppService driver. One JSON request/response per
// line; the job runs on the production scheduler while control remains usable.
#include "workflow_fixture.hpp"
#include "tc/core/error.hpp"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>

using nlohmann::json;
namespace {
struct Probe {
    std::mutex mutex;
    std::condition_variable wake;
    std::string gate;
    bool held = false;
    bool release = false;
    std::atomic<bool> native_open{false}, native_read{false};
    std::atomic<std::size_t> readers{0}, reads{0};
    std::atomic<std::uint64_t> bytes{0};
    void checkpoint(std::string const& site) {
        std::unique_lock lock(mutex);
        if (gate != site) return;
        gate.clear(); held = true; release = false;
        wake.wait(lock, [&] { return release; });
        held = false;
    }
    json snapshot() {
        std::lock_guard lock(mutex);
        return {{"held", held}, {"nativeOpen", native_open.load()}, {"nativeRead", native_read.load()},
            {"readers", readers.load()}, {"readCalls", reads.load()}, {"bytesRead", bytes.load()}};
    }
};
struct Active {
    std::atomic<bool>& flag;
    explicit Active(std::atomic<bool>& f) : flag(f) { flag = true; }
    ~Active() { flag = false; }
};
class Reader final : public tc::core::PayloadReader {
public:
    Reader(std::unique_ptr<tc::core::PayloadReader> inner, Probe& p) : inner_(std::move(inner)), p_(p) { ++p_.readers; }
    ~Reader() override { inner_.reset(); --p_.readers; }
    std::size_t read(std::span<std::byte> buffer) override { return read(buffer, {}); }
    std::size_t read(std::span<std::byte> buffer, std::stop_token stop) override {
        p_.checkpoint("read");
        Active active(p_.native_read); ++p_.reads;
        auto const n = inner_->read(buffer, stop); p_.bytes += n; return n;
    }
    std::optional<tc::core::native::FileObservation> observe() override { return inner_->observe(); }
private:
    std::unique_ptr<tc::core::PayloadReader> inner_;
    Probe& p_;
};
class Source final : public tc::core::PayloadSource {
public:
    explicit Source(Probe& p) : p_(p), inner_(tc::core::make_file_payload_source()) {}
    std::unique_ptr<tc::core::PayloadReader> open(tc::core::ManifestEntry const& e) override { return open(e, {}); }
    std::unique_ptr<tc::core::PayloadReader> open(tc::core::ManifestEntry const& e, std::stop_token stop) override {
        p_.checkpoint("open"); Active active(p_.native_open);
        return std::make_unique<Reader>(inner_->open(e, stop), p_);
    }
private:
    Probe& p_;
    std::unique_ptr<tc::core::PayloadSource> inner_;
};
}

int main()
{
    try {
        std::string line;
        if (!std::getline(std::cin, line)) return 1;
        json const input = json::parse(line);
        Probe probe;
        tc::service::AppService::Options options;
        // The wrapper is enabled only for SMB fault coordination. With no
        // probe the memory workflow uses the ordinary service options.
        if (input.value("probe", false)) options.jobs.payload_factory = [&] { return std::make_unique<Source>(probe); };
        tc::service::AppService app(options, {});
        std::string id;
        std::size_t job_offset = 0;
        std::cout << json{{"ok", true}, {"ready", true}}.dump() << std::endl;
        while (std::getline(std::cin, line)) {
            try {
                json const command = json::parse(line);
                std::string const op = command.at("operation");
                json result;
                if (op == "scan") result = tc::proof::prepare_fixture(app, input);
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
                else if (op == "clear") { app.jobs().clear_finished(); result["remaining"] = app.jobs().bridge_page(0, 50).at("total"); }
                else if (op == "arm") {
                    std::lock_guard lock(probe.mutex);
                    probe.gate = command.at("site");
                    if (probe.gate != "open" && probe.gate != "read") throw std::runtime_error("Invalid probe site");
                } else if (op == "release") {
                    { std::lock_guard lock(probe.mutex); probe.release = true; }
                    probe.wake.notify_all();
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
