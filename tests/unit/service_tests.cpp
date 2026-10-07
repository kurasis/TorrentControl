// Application service: jobs, pause/cancel, profiles, batch, projects and
// settings (specification sections 4, 9.3, 10.4, 14; fixtures U03, U05, U07).

#include "tc/core/error.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/service/app_service.hpp"
#include "tc/service/batch.hpp"
#include "tc/service/jobs.hpp"
#include "tc/service/profiles.hpp"
#include "tc/service/storage.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <new>
#include <thread>

using namespace tc;
using namespace tc::service;
using nlohmann::json;
namespace fs = std::filesystem;

TEST_CASE("settings reject out-of-range integers before narrowing", "[service][settings][audit]")
{
    AppSettings settings;
    auto const before = to_json(settings);
    for (auto text : {"0", "9", "-1", "4294967297", "4294967304", "-4294967295",
             "18446744073709551615", "1.5", "\"2\"", "true", "null"}) {
        INFO("maxConcurrentJobs " << text);
        try {
            apply_settings_patch(settings, {{"language", "ru"}, {"maxConcurrentJobs", json::parse(text)}});
            FAIL_CHECK("Invalid concurrency was accepted");
        } catch (core::CoreError const& e) {
            CHECK(e.code() == core::ErrorCode::InvalidArgument);
        }
        CHECK(to_json(settings) == before);
        settings = AppSettings{};
    }
    for (int jobs : {1, 8}) {
        apply_settings_patch(settings, {{"maxConcurrentJobs", jobs}});
        CHECK(settings.max_concurrent_jobs == jobs);
    }
}

TEST_CASE("small-file reads report IO errors and preserve empty-file and size limits", "[service][storage][audit]")
{
    test::TempDir dir;
    auto const empty = dir.path() / "empty.json";
    test::write_bytes(empty, "");
    CHECK(read_small_file(empty, 0).empty());
    auto const file = dir.path() / "settings.json";
    test::write_bytes(file, "{}");
    CHECK(read_small_file(file, 2) == "{}");
    CHECK_THROWS_AS(read_small_file(file, 1), core::CoreError);
    CHECK_THROWS_AS(read_small_file(dir.path() / "missing.json"), core::CoreError);
    // Some platforms open a directory successfully but fail on the first read.
    CHECK_THROWS_AS(read_small_file(dir.path()), core::CoreError);
}

namespace {

constexpr int kib = 1024;

// Reads block until the gate opens, so a test can catch a job mid-hash.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = true;
    std::atomic<int> waiting{0};

    void close()
    {
        std::lock_guard l(m);
        open = false;
    }
    void release()
    {
        {
            std::lock_guard l(m);
            open = true;
        }
        cv.notify_all();
    }
    void pass()
    {
        std::unique_lock l(m);
        ++waiting;
        cv.wait(l, [&] { return open; });
        --waiting;
    }
};

class GatedSource final : public core::PayloadSource {
public:
    explicit GatedSource(Gate& gate) : gate_(gate), inner_(core::make_file_payload_source()) {}
    std::unique_ptr<core::PayloadReader> open(core::ManifestEntry const& entry) override
    {
        struct Reader final : core::PayloadReader {
            Gate& gate;
            std::unique_ptr<core::PayloadReader> inner;
            Reader(Gate& g, std::unique_ptr<core::PayloadReader> r) : gate(g), inner(std::move(r)) {}
            std::size_t read(std::span<std::byte> b) override
            {
                gate.pass();
                return inner->read(b.first(std::min<std::size_t>(b.size(), 16 * kib)));
            }
            std::optional<core::native::FileObservation> observe() override { return inner->observe(); }
        };
        return std::make_unique<Reader>(gate_, inner_->open(entry));
    }

private:
    Gate& gate_;
    std::unique_ptr<core::PayloadSource> inner_;
};

// Collects events and lets a test wait for a job state.
struct Events {
    std::mutex m;
    std::condition_variable cv;
    std::vector<json> all;

    void push(json const& e)
    {
        {
            std::lock_guard l(m);
            all.push_back(e);
        }
        cv.notify_all();
    }
    bool wait_state(std::string const& job, std::string const& state)
    {
        std::unique_lock l(m);
        return cv.wait_for(l, std::chrono::seconds(20), [&] {
            return std::any_of(all.begin(), all.end(), [&](json const& e) {
                return e["type"] == "job" && e["job"]["id"] == job && e["job"]["state"] == state;
            });
        });
    }
    std::vector<std::string> states(std::string const& job)
    {
        std::lock_guard l(m);
        std::vector<std::string> out;
        for (auto const& e : all)
            if (e["type"] == "job" && e["job"]["id"] == job) {
                std::string const s = e["job"]["state"];
                if (out.empty() || out.back() != s) out.push_back(s);
            }
        return out;
    }
};

struct Harness {
    tc::test::TempDir dir;
    Gate gate;
    Events events;
    std::unique_ptr<AppService> app;

    explicit Harness(int max_concurrent = 1)
    {
        AppService::Options o;
        o.settings_path = dir.path() / "settings" / "settings.json";
        o.jobs.max_concurrent = max_concurrent;
        o.jobs.progress_interval = std::chrono::milliseconds(0);
        o.jobs.payload_factory = [this] { return std::make_unique<GatedSource>(gate); };
        o.now = [] { return std::int64_t{1700000000}; };
        app = std::make_unique<AppService>(std::move(o), [this](json const& e) { events.push(e); });
        if (max_concurrent > 1) app->update_settings({{"maxConcurrentJobs", max_concurrent}});
    }

    fs::path dataset(std::string const& name, int files = 3, std::uint64_t size = 100 * kib)
    {
        fs::path const root = dir.path() / name;
        for (int i = 0; i < files; ++i)
            tc::test::write_file(root / ("f" + std::to_string(i) + ".bin"), size, static_cast<std::uint32_t>(i + 1));
        return root;
    }

    std::string state(std::string const& id) { return std::string(to_string(app->jobs().find(id)->state)); }
};

std::uint64_t rev(json const& draft)
{
    return std::stoull(draft["revision"].get<std::string>());
}

bool has_issue(json const& v, std::string const& code)
{
    return std::any_of(v["issues"].begin(), v["issues"].end(), [&](json const& i) { return i["code"] == code; });
}

} // namespace

TEST_CASE("job transitions follow the specified state machine", "[service][jobs]")
{
    CHECK(is_valid_transition(JobState::Queued, JobState::Hashing));
    CHECK(is_valid_transition(JobState::Hashing, JobState::Pausing));
    CHECK(is_valid_transition(JobState::Pausing, JobState::Paused));
    CHECK(is_valid_transition(JobState::Paused, JobState::Hashing));
    CHECK(is_valid_transition(JobState::Validating, JobState::Committing));
    CHECK(is_valid_transition(JobState::Committing, JobState::SucceededWithWarnings));
    CHECK_FALSE(is_valid_transition(JobState::Committing, JobState::Cancelling)); // non-interruptible
    CHECK_FALSE(is_valid_transition(JobState::Paused, JobState::Validating));
    CHECK_FALSE(is_valid_transition(JobState::Succeeded, JobState::Hashing));
    CHECK_FALSE(is_valid_transition(JobState::Queued, JobState::Committing));
    for (auto s : {JobState::Succeeded, JobState::SucceededWithWarnings, JobState::Failed, JobState::Cancelled})
        CHECK(is_terminal(s));
}

TEST_CASE("a draft goes from sources to a committed torrent", "[service]")
{
    Harness h;
    fs::path const root = h.dataset("Album");
    json draft = h.app->add_sources({root});
    CHECK(draft["sources"].size() == 1);
    CHECK(draft["effectiveName"] == "Album");
    CHECK(draft["output"].get<std::string>().ends_with("Album.torrent"));
    h.app->wait_for_scan();

    json const v = h.app->validate_draft();
    INFO(v.dump(2));
    REQUIRE(v["canCreate"] == true);
    CHECK(v["summary"]["realFiles"] == 3);
    CHECK(v["summary"]["payloadBytes"] == std::to_string(300 * kib));
    CHECK(v["summary"].contains("estimatedMetainfoBytes"));

    std::string const id = h.app->start_create();
    h.app->jobs().wait_idle();
    auto const job = h.app->jobs().find(id);
    REQUIRE(job);
    INFO(to_json(*job).dump(2));
    // Three 100 KiB files in 256 KiB pieces: hybrid padding is reported.
    REQUIRE(job->state == JobState::SucceededWithWarnings);
    REQUIRE(job->result->warnings.size() == 1);
    CHECK(job->result->warnings[0].find("padding") != std::string::npos);
    CHECK(job->result->real_files == 3);
    CHECK(job->result->format == "hybrid");
    core::Metainfo const m = core::Metainfo::parse(tc::test::read_all(job->result->output));
    CHECK(core::to_hex(*m.info_hashes().v2) == job->result->infohash_v2);
    CHECK(core::validate_metainfo(m).empty());

    std::vector<std::string> const expected{"Queued", "Hashing", "Validating", "Committing", "SucceededWithWarnings"};
    CHECK(h.events.states(id) == expected);
}

TEST_CASE("source tag is applied inside info and keeps payload hashes", "[service]")
{
    Harness h;
    h.app->add_sources({h.dataset("Tagged")});
    h.app->wait_for_scan();
    h.app->update_draft({{"source", "TRACKER-X"}, {"format", "v1"}}, std::nullopt);
    std::string const id = h.app->start_create();
    h.app->jobs().wait_idle();
    auto const job = h.app->jobs().find(id);
    REQUIRE(job->state == JobState::Succeeded);
    core::Metainfo const m = core::Metainfo::parse(tc::test::read_all(job->result->output));
    CHECK(m.info().find("source")->text() == "TRACKER-X");
}

TEST_CASE("pause parks the job and resume finishes it", "[service][jobs]")
{
    Harness h;
    h.app->add_sources({h.dataset("Big", 2, 512 * kib)});
    h.app->wait_for_scan();
    h.gate.close();
    std::string const id = h.app->start_create();
    REQUIRE(h.events.wait_state(id, "Hashing"));
    while (h.gate.waiting == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    h.app->jobs().pause(id);
    CHECK(h.state(id) == "Pausing");
    h.gate.release(); // the in-flight read completes, then the reader parks
    REQUIRE(h.events.wait_state(id, "Paused"));
    auto const paused_at = h.app->jobs().find(id)->bytes_done;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(h.app->jobs().find(id)->bytes_done == paused_at); // no reads while paused
    CHECK_THROWS_AS(h.app->jobs().pause(id), ServiceError);

    h.app->jobs().resume(id);
    h.app->jobs().wait_idle();
    CHECK(h.state(id) == "Succeeded");
    std::vector<std::string> const expected{"Queued", "Hashing", "Pausing", "Paused", "Hashing", "Validating", "Committing", "Succeeded"};
    CHECK(h.events.states(id) == expected);
}

TEST_CASE("cancel while paused leaves no output", "[service][jobs][W08]")
{
    Harness h;
    h.app->add_sources({h.dataset("Cancel", 2, 512 * kib)});
    h.app->wait_for_scan();
    fs::path const out = fs::path(h.app->draft_json()["output"].get<std::string>());
    h.gate.close();
    std::string const id = h.app->start_create();
    REQUIRE(h.events.wait_state(id, "Hashing"));
    h.app->jobs().pause(id);
    h.gate.release();
    REQUIRE(h.events.wait_state(id, "Paused"));

    h.app->jobs().cancel(id);
    h.app->jobs().wait_idle();
    CHECK(h.state(id) == "Cancelled");
    CHECK_FALSE(fs::exists(out));
    CHECK_THROWS_AS(h.app->jobs().cancel(id), ServiceError); // already finished
}

TEST_CASE("queued work keeps its settings snapshot and runs in order", "[service][jobs][U07]")
{
    Harness h;
    h.app->add_sources({h.dataset("First", 1, 300 * kib)});
    h.app->wait_for_scan();
    h.gate.close();
    std::string const first = h.app->start_create();
    REQUIRE(h.events.wait_state(first, "Hashing"));

    // A second draft is queued behind the first, then the draft changes.
    h.app->new_draft();
    h.app->add_sources({h.dataset("Second", 1, 50 * kib)});
    h.app->wait_for_scan();
    h.app->update_draft({{"format", "v2"}}, std::nullopt);
    std::string const second = h.app->start_create();
    CHECK(h.state(second) == "Queued");
    h.app->update_draft({{"format", "v1"}, {"comment", "changed later"}}, std::nullopt);
    h.app->apply_profile("trackerless", std::nullopt);

    std::string const third = h.app->start_create();
    h.app->jobs().cancel(third); // a queued job is cancelled without running
    CHECK(h.state(third) == "Cancelled");

    h.gate.release();
    h.app->jobs().wait_idle();
    auto const s = h.app->jobs().find(second);
    REQUIRE(s->state == JobState::Succeeded);
    CHECK(s->result->format == "v2");
    core::Metainfo const m = core::Metainfo::parse(tc::test::read_all(s->result->output));
    CHECK(m.root().find("comment") == nullptr);
    CHECK(m.root().find("announce") != nullptr); // public trackers from the snapshot
}

TEST_CASE("stale draft revisions are rejected", "[service]")
{
    Harness h;
    json const d = h.app->update_draft({{"comment", "a"}}, std::nullopt);
    CHECK_NOTHROW(h.app->update_draft({{"comment", "b"}}, rev(d)));
    try {
        h.app->update_draft({{"comment", "c"}}, rev(d));
        FAIL("expected STALE_REVISION");
    } catch (ServiceError const& e) {
        CHECK(e.code() == "STALE_REVISION");
    }
    CHECK(h.app->draft_json()["comment"] == "b");
    CHECK_THROWS_AS(h.app->update_draft({{"output", "/etc/passwd"}}, std::nullopt), core::CoreError);
    CHECK_THROWS_AS(h.app->update_draft({{"pieceLength", 1000}}, std::nullopt), core::CoreError);
}

TEST_CASE("reducing worker concurrency drains running jobs before starting queued work", "[service][jobs][retention]")
{
    tc::test::TempDir dir;
    auto const file = dir.path() / "a.bin";
    tc::test::write_file(file, 32768, 1);
    auto const bytes = tc::test::make_torrent(file, core::TorrentFormat::V1, 16384);
    auto const mapping = core::map_to_root(core::Metainfo::parse(bytes), file);
    Gate gate; gate.close();
    JobScheduler::Options options;
    options.max_concurrent = 2;
    options.payload_factory = [&] { return std::make_unique<GatedSource>(gate); };
    std::mutex events_mutex;
    std::vector<std::pair<std::string, JobState>> events;
    JobScheduler jobs(options, [&](JobSnapshot const& job) {
        std::lock_guard lock(events_mutex); events.emplace_back(job.id, job.state);
    });
    auto const first = jobs.enqueue_verify(VerifyJobSpec{"first", bytes, mapping});
    auto const second = jobs.enqueue_verify(VerifyJobSpec{"second", bytes, mapping});
    auto const third = jobs.enqueue_verify(VerifyJobSpec{"third", bytes, mapping});
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (gate.waiting < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    bool const parallel = gate.waiting == 2;
    if (!parallel) gate.release();
    REQUIRE(parallel);
    CHECK(jobs.find(third)->state == JobState::Queued);
    jobs.set_max_concurrent(1); gate.release(); jobs.wait_idle();
    auto position = [&](std::string const& id, JobState state) {
        return std::find(events.begin(), events.end(), std::pair{id, state}) - events.begin();
    };
    CHECK(position(third, JobState::Hashing) > position(first, JobState::Succeeded));
    CHECK(position(third, JobState::Hashing) > position(second, JobState::Succeeded));
    CHECK(jobs.find(third)->state == JobState::Succeeded);
    CHECK(jobs.retention_summary()["workers"] == 2);
    jobs.set_max_concurrent(100); jobs.set_max_concurrent(1);
    CHECK(jobs.retention_summary()["workers"] == 8);
    CHECK(jobs.retention_summary()["verifySpecs"] == 0);
}

TEST_CASE("cleared batch identifiers and queued inputs are bounded while other work runs", "[service][jobs][retention]")
{
    Harness h;
    h.app->add_sources({h.dataset("Collection", 2, 16 * kib)});
    h.app->wait_for_scan();
    h.gate.close();
    auto const active = h.app->start_create();
    bool const hashing = h.events.wait_state(active, "Hashing");
    if (!hashing) h.gate.release();
    REQUIRE(hashing);
    for (int i = 0; i < 130; ++i) {
        h.app->plan_batch("single", "rename", h.dir.path());
        auto const ids = h.app->start_batch();
        REQUIRE(ids.size() == 1);
        h.app->jobs().cancel(ids.front());
    }
    h.app->clear_finished_jobs();
    auto const retained = h.app->retention_summary();
    CHECK(retained["jobs"] == 1);
    CHECK(retained["createSpecs"] == 1); // The running job still owns its frozen input.
    CHECK(retained["batches"] == 0);
    CHECK(retained["appBatches"] == 0);
    CHECK(retained["appBatchJobIds"] == 0);
    CHECK(retained["archivedBatches"] == 64);
    CHECK(h.app->jobs().batch_status("batch-1")["total"] == 0);
    CHECK(h.app->jobs().batch_status("batch-100")["cancelled"] == 1);
    CHECK(h.app->jobs().batch_status("batch-130")["cancelled"] == 1);
    h.gate.release(); h.app->jobs().wait_idle();
    CHECK(h.state(active).starts_with("Succeeded"));
    CHECK(h.app->retention_summary()["createSpecs"] == 0);
}

TEST_CASE("terminal listeners can remove their own history without destroying a running worker", "[service][jobs][retention]")
{
    tc::test::TempDir dir;
    auto const file = dir.path() / "a.bin";
    tc::test::write_file(file, 16384, 1);
    auto const bytes = tc::test::make_torrent(file, core::TorrentFormat::V1, 16384);
    auto const mapping = core::map_to_root(core::Metainfo::parse(bytes), file);
    std::unique_ptr<JobScheduler> jobs;
    std::atomic<int> completed{0};
    jobs = std::make_unique<JobScheduler>(JobScheduler::Options{}, [&](JobSnapshot const& job) {
        if (is_terminal(job.state)) { jobs->clear_finished(); ++completed; }
    });
    jobs->enqueue_verify(VerifyJobSpec{"first", bytes, mapping});
    jobs->enqueue_verify(VerifyJobSpec{"second", bytes, mapping});
    jobs->wait_idle();
    CHECK(completed == 2);
    CHECK(jobs->snapshot().empty());
    CHECK(jobs->retention_summary()["workers"] == 1);
}

TEST_CASE("concurrent history clearing does not invalidate another clear or enqueue", "[service][jobs][retention]")
{
    tc::test::TempDir dir;
    auto const file = dir.path() / "a.bin";
    tc::test::write_file(file, 16384, 1);
    auto const bytes = tc::test::make_torrent(file, core::TorrentFormat::V1, 16384);
    auto const mapping = core::map_to_root(core::Metainfo::parse(bytes), file);
    Gate listener_gate; listener_gate.close();
    std::atomic<int> terminals{0};
    JobScheduler jobs({}, [&](JobSnapshot const& job) {
        if (is_terminal(job.state) && terminals.fetch_add(1) == 0) listener_gate.pass();
    });
    jobs.enqueue_verify(VerifyJobSpec{"first", bytes, mapping});
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!listener_gate.waiting && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    bool const waiting = listener_gate.waiting > 0;
    if (!waiting) listener_gate.release();
    REQUIRE(waiting);
    std::jthread clear([&] { jobs.clear_finished(); });
    while (jobs.bridge_page(0, 0)["total"] != 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    bool const removed = jobs.bridge_page(0, 0)["total"] == 0;
    auto const second = jobs.enqueue_verify(VerifyJobSpec{"second", bytes, mapping});
    jobs.clear_finished();
    listener_gate.release(); clear.join(); jobs.wait_idle();
    CHECK(removed);
    CHECK(jobs.find(second)->state == JobState::Succeeded);
    CHECK(jobs.retention_summary()["verifySpecs"] == 0);
}

TEST_CASE("profile switches show removals, keep private trackers and can be undone", "[service][profiles]")
{
    Harness h;
    std::string const passkey_url = "https://tracker.example/announce/0123456789abcdef0123456789abcdef";
    json d = h.app->draft_json();
    CHECK(d["profile"] == "public");
    CHECK(d["trackers"].size() == 10);
    json trackers = d["trackers"];
    trackers.push_back({{"url", passkey_url}, {"tier", 10}});
    h.app->update_draft({{"trackers", trackers}, {"webSeeds", json::array({"https://cdn.example/files/"})}}, std::nullopt);

    json const plan = h.app->plan_profile("private");
    auto field = [&](std::string const& f) {
        for (auto const& c : plan["changes"])
            if (c["field"] == f) return c;
        return json();
    };
    CHECK(field("private")["after"] == true);
    CHECK(field("trackers")["after"].size() == 1);
    CHECK(field("trackers")["after"][0]["url"] == passkey_url); // never dropped
    CHECK(field("trackers")["removesUserValue"] == false);
    CHECK(field("webSeeds")["removesUserValue"] == true);

    h.app->apply_profile("private", std::nullopt);
    d = h.app->draft_json();
    CHECK(d["private"] == true);
    CHECK(d["format"] == "v1");
    CHECK(d["trackers"].size() == 1);
    CHECK(d["canUndo"] == true);

    json const trackerless = h.app->plan_profile("trackerless");
    bool removes = false;
    for (auto const& c : trackerless["changes"])
        if (c["field"] == "trackers") removes = c["removesUserValue"];
    CHECK(removes); // the passkey URL would be removed: the UI must list it

    h.app->undo();
    d = h.app->draft_json();
    CHECK(d["private"] == false);
    CHECK(d["trackers"].size() == 11);
    CHECK(d["webSeeds"].size() == 1);
}

TEST_CASE("private drafts are validated before creation", "[service][profiles]")
{
    Harness h;
    h.app->add_sources({h.dataset("Private")});
    h.app->wait_for_scan();
    h.app->update_draft({{"private", true}}, std::nullopt);
    json v = h.app->validate_draft();
    CHECK(v["canCreate"] == false);
    CHECK(has_issue(v, "PRIVATE_PUBLIC_TRACKER"));

    h.app->apply_profile("private", std::nullopt);
    v = h.app->validate_draft();
    CHECK(has_issue(v, "PRIVATE_WITHOUT_TRACKER"));
    CHECK_THROWS_AS(h.app->start_create(), ServiceError);

    h.app->update_draft({{"trackers", json::array({{{"url", "https://private.example/announce?passkey=secret"}}})}}, std::nullopt);
    v = h.app->validate_draft();
    INFO(v.dump(2));
    CHECK(v["canCreate"] == true);
}

TEST_CASE("an existing output needs an explicit replace choice", "[service][W07]")
{
    Harness h;
    fs::path const root = h.dataset("Again");
    h.app->add_sources({root});
    h.app->wait_for_scan();
    fs::path const out = fs::path(h.app->draft_json()["output"].get<std::string>());
    tc::test::write_bytes(out, "old");
    CHECK(has_issue(h.app->validate_draft(), "OUTPUT_EXISTS"));
    h.app->update_draft({{"replaceExisting", true}}, std::nullopt);
    CHECK(h.app->validate_draft()["canCreate"] == true);

    // An output inside the selected folder is payload: a conflict, not a silent overwrite.
    h.app->set_output(root / "f1.bin");
    json const v = h.app->validate_draft();
    CHECK(has_issue(v, "OUTPUT_CONFLICT"));
}

TEST_CASE("batch plans per file and per child folder with conflict handling", "[service][batch][U07]")
{
    Harness h;
    fs::path const shows = h.dir.path() / "Shows";
    tc::test::write_file(shows / "A" / "e1.mkv", 40 * kib, 1);
    tc::test::write_file(shows / "B" / "e1.mkv", 40 * kib, 2);
    tc::test::write_file(shows / "C" / "nested" / "x.bin", 40 * kib, 3);
    tc::test::write_file(shows / "loose.txt", 10, 4);
    fs::path const out = h.dir.path() / "out";
    fs::create_directories(out);
    tc::test::write_bytes(out / "A.torrent", "existing");

    h.app->add_sources({shows});
    json plan = h.app->plan_batch("perChildFolder", "rename", out);
    REQUIRE(plan["items"].size() == 3);
    CHECK(plan["items"][0]["name"] == "A");
    CHECK(plan["items"][0]["existsOnDisk"] == true);
    CHECK(plan["items"][0]["output"].get<std::string>().ends_with("A (2).torrent"));
    CHECK(plan["notes"].size() == 1); // loose.txt is not in any per-folder torrent

    plan = h.app->update_batch({{"item-1", {{"policy", "skip"}}}});
    CHECK(plan["items"][0]["included"] == false);

    std::vector<std::string> const ids = h.app->start_batch();
    REQUIRE(ids.size() == 2);
    h.app->jobs().wait_idle();
    for (auto const& id : ids) {
        auto const j = h.app->jobs().find(id);
        INFO(to_json(*j).dump(2));
        CHECK(j->state == JobState::Succeeded);
        CHECK(j->batch_id == "batch-1");
    }
    CHECK(tc::test::read_all(out / "A.torrent") == "existing");
    CHECK(fs::exists(out / "B.torrent"));
    CHECK(fs::exists(out / "C.torrent"));
    // Child-folder recursion follows the folder's own setting.
    auto const c = h.app->jobs().find(ids[1]);
    CHECK(c->result->real_files == 1);
}

TEST_CASE("batch aggregate counts retain success failure and cancellation after clearing", "[service][batch][paging]")
{
    Harness h;
    auto root = h.dataset("Batch status", 3, 32 * kib);
    h.app->add_sources({root});
    h.app->wait_for_scan();
    auto out = h.dir.path() / "out";
    fs::create_directory(out);
    auto plan = h.app->plan_batch("perFile", "rename", out);
    h.gate.close();
    auto ids = h.app->start_batch(plan["revision"]);
    REQUIRE(ids.size() == 3);
    bool const hashing = h.events.wait_state(ids.front(), "Hashing");
    if (!hashing) h.gate.release();
    REQUIRE(hashing);
    h.app->jobs().cancel(ids.back());
    fs::remove(root / "f1.bin");
    h.gate.release();
    h.app->jobs().wait_idle();
    auto report = h.app->jobs().batch_status("batch-1");
    CHECK(report["total"] == 3);
    CHECK(report["done"] == 1);
    CHECK(report["failed"] == 1);
    CHECK(report["cancelled"] == 1);
    CHECK(report["finished"] == true);
    h.app->jobs().clear_finished();
    CHECK(h.app->jobs().batch_status("batch-1") == report);
}

TEST_CASE("one failed batch item does not affect the others", "[service][batch][U07]")
{
    Harness h;
    fs::path const root = h.dir.path() / "Mixed";
    tc::test::write_file(root / "good.bin", 30 * kib, 1);
    tc::test::write_file(root / "empty.bin", 0);
    tc::test::write_file(root / "also good.bin", 20 * kib, 2);
    h.app->add_sources({root});
    json const plan = h.app->plan_batch("perFile", "rename", h.dir.path());
    REQUIRE(plan["items"].size() == 3);
    std::vector<std::string> const ids = h.app->start_batch();
    h.app->jobs().wait_idle();
    std::map<std::string, std::string> states;
    for (auto const& id : ids) {
        auto const j = h.app->jobs().find(id);
        states[j->name] = std::string(to_string(j->state));
        if (j->state == JobState::Failed) CHECK(j->error->code == "EMPTY_PAYLOAD");
    }
    CHECK(states["good.bin"] == "Succeeded");
    CHECK(states["also good.bin"] == "Succeeded");
    CHECK(states["empty.bin"] == "Failed");
}

TEST_CASE("duplicate batch outputs are renamed even under replace", "[service][batch]")
{
    tc::test::TempDir dir;
    tc::test::write_file(dir.path() / "x" / "Same" / "a", 10, 1);
    tc::test::write_file(dir.path() / "y" / "Same" / "b", 10, 2);
    Draft d;
    d.sources = {{.id = "s1", .path = dir.path() / "x", .is_directory = true},
        {.id = "s2", .path = dir.path() / "y", .is_directory = true}};
    BatchPlan p = plan_batch(d, BatchMode::PerChildFolder, dir.path(), ConflictPolicy::Replace);
    REQUIRE(p.items.size() == 2);
    CHECK(p.items[0].duplicate_in_batch);
    CHECK(p.items[0].output.filename() == "Same.torrent");
    CHECK(p.items[1].output.filename() == "Same (2).torrent");
}

TEST_CASE("projects round-trip the draft and are never payload", "[service][projects]")
{
    Harness h;
    fs::path const root = h.dataset("Proj");
    h.app->add_sources({root});
    h.app->update_draft({{"comment", "Привет"}, {"pieceLength", 65536}, {"creationDate", "omit"}}, std::nullopt);
    h.app->wait_for_scan();
    fs::path const project = root / "Proj.tcproject";
    h.app->save_project(project);

    json const saved = json::parse(tc::test::read_all(project));
    CHECK(saved["format"] == "torrentcontrol-project");
    CHECK(saved["version"] == 1);
    CHECK(saved["pieceSizePolicy"]["resolvedPieceLength"] == 65536);

    h.app->new_draft();
    json const d = h.app->load_project(project);
    CHECK(d["comment"] == "Привет");
    CHECK(d["pieceLength"] == 65536);
    CHECK(d["creationDate"] == "omit");
    CHECK(d["sources"][0]["path"] == core::to_utf8(fs::absolute(root).lexically_normal()));
    h.app->wait_for_scan();
    CHECK(h.app->validate_draft()["summary"]["realFiles"] == 3); // the .tcproject is excluded

    json newer = saved;
    newer["version"] = 99;
    tc::test::write_bytes(h.dir.path() / "newer.tcproject", newer.dump());
    CHECK_THROWS_AS(h.app->load_project(h.dir.path() / "newer.tcproject"), core::CoreError);
}

TEST_CASE("settings protect passkeys and exports are redacted", "[service][settings][U05]")
{
    std::string const secret_url = "https://private.example/announce/a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6";
    fs::path settings_file;
    {
        Harness h;
        settings_file = h.dir.path() / "settings" / "settings.json";
        h.app->apply_profile("private", std::nullopt);
        h.app->update_draft({{"trackers", json::array({{{"url", secret_url}}})}}, std::nullopt);
        json const p = h.app->save_custom_profile("My tracker");
        std::string const text = tc::test::read_all(settings_file);
        CHECK(text.find("a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6") == std::string::npos);

        json const listed = h.app->profiles_json();
        CHECK(listed.dump().find("a1b2c3d4e5f6") == std::string::npos);
        json const exported = h.app->export_profile(p["id"], false);
        CHECK(exported.dump().find("a1b2c3d4e5f6") == std::string::npos);
        CHECK(h.app->export_profile(p["id"], true)["trackers"][0]["url"] == secret_url);

        // Reloading the settings restores the protected value.
        AppSettings const loaded = load_settings(settings_file);
        REQUIRE(loaded.custom_profiles.size() == 1);
        CHECK(loaded.custom_profiles[0].trackers[0].url == secret_url);
        CHECK(loaded.last_profile == p["id"].get<std::string>());
    }
}

TEST_CASE("credentials are masked in URLs", "[service][U05]")
{
    CHECK(redact_url("udp://tracker.opentrackr.org:1337/announce") == "udp://tracker.opentrackr.org:1337/announce");
    CHECK(redact_url("https://t.example/announce?passkey=abc&uid=5") == "https://t.example/announce?passkey=***&uid=***");
    CHECK(redact_url("https://t.example/a1b2c3d4e5f6a7b8c9d0/announce") == "https://t.example/***/announce");
    CHECK(redact_url("http://user:pw@t.example:80/x") == "http://***@t.example:80/x");
    CHECK_FALSE(looks_secret("http://tracker.qu.ax:6969/announce"));
    CHECK(looks_secret("https://t.example/announce?pk=1"));
}

TEST_CASE("opened torrents can be verified against a folder", "[service][verify]")
{
    Harness h;
    fs::path const root = h.dataset("Check");
    h.app->add_sources({root});
    h.app->wait_for_scan();
    std::string const id = h.app->start_create();
    h.app->jobs().wait_idle();
    fs::path const torrent = h.app->jobs().find(id)->result->output;

    json const info = h.app->open_torrent(torrent);
    CHECK(info["problems"].empty());
    CHECK(info["realFiles"] == 3);
    CHECK(info["trackers"].size() == 10);
    CHECK(h.app->torrent_files_page(info["id"], 0, 2)["files"].size() == 2);

    std::string const ok = h.app->verify_torrent(info["id"], root);
    h.app->jobs().wait_idle();
    CHECK(h.state(ok) == "Succeeded");

    {
        std::fstream f(root / "f1.bin", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(10);
        f.put('\0');
        f.put('\1');
    }
    std::string const bad = h.app->verify_torrent(info["id"], root);
    h.app->jobs().wait_idle();
    auto const j = h.app->jobs().find(bad);
    REQUIRE(j->state == JobState::Failed);
    CHECK(j->error->code == "PAYLOAD_MISMATCH");
    CHECK_FALSE(j->verify["ok"].get<bool>());
}

TEST_CASE("a snapshot restores the view without replaying work", "[service][U03]")
{
    Harness h;
    h.app->add_sources({h.dataset("Recover", 1, 300 * kib)});
    h.app->wait_for_scan();
    h.gate.close();
    std::string const id = h.app->start_create();
    REQUIRE(h.events.wait_state(id, "Hashing"));

    json const a = h.app->snapshot();
    json const b = h.app->snapshot();
    CHECK(a["jobs"].size() == 1);
    CHECK(b["jobs"].size() == 1);
    CHECK(a["jobs"][0]["id"] == id);
    CHECK(a["draft"]["sources"].size() == 1);
    h.gate.release();
    h.app->jobs().wait_idle();
    CHECK(h.app->jobs().snapshot().size() == 1);
}

TEST_CASE("manifest pages are bounded and filterable", "[service][U02]")
{
    Harness h;
    fs::path const root = h.dir.path() / "Many";
    for (int i = 0; i < 1500; ++i) tc::test::write_file(root / ("file" + std::to_string(i) + ".txt"), 1, 1);
    h.app->add_sources({root});
    h.app->wait_for_scan();
    json const page = h.app->manifest_page(0, 5000, "");
    CHECK(page["total"] == 1500);
    CHECK(page["entries"].size() == 1000); // capped
    json const filtered = h.app->manifest_page(0, 50, "FILE14");
    CHECK(filtered["total"] == 111); // file14, file140-149, file1400-1499
}

TEST_CASE("native manifest caches invalidate edits and retain live output checks", "[service][performance][U02]")
{
    Harness h;
    auto root = h.dir.path() / "Alpha";
    tc::test::write_file(root / "a.bin",100,1);
    tc::test::write_file(root / "b.bin",200,2);
    h.app->add_sources({root});
    h.app->wait_for_scan();
    h.app->update_draft({{"format","v1"},{"pieceLength",16384}},std::nullopt);
    auto first = h.app->snapshot()["scan"]["summary"];
    CHECK(first["paddingBytes"] == "0");
    CHECK(h.app->validate_draft()["canCreate"] == true);
    CHECK(h.app->manifest_page(0,1,"ALPHA")["total"] == 2);
    CHECK(h.app->manifest_page(1,1,"alpha")["entries"].front()["torrentPath"] == "Alpha/b.bin");
    h.app->update_draft({{"name","Beta"},{"format","hybrid"},{"pieceLength",32768}},std::nullopt);
    auto changed = h.app->snapshot()["scan"]["summary"];
    CHECK(changed["name"] == "Beta");
    CHECK(changed["pieceLength"] == 32768);
    CHECK(changed["paddingBytes"] != "0");
    CHECK(h.app->manifest_page(0,50,"Alpha")["total"] == 0);
    CHECK(h.app->manifest_page(0,50,"Beta")["total"] == 2);
    CHECK(h.app->validate_draft()["canCreate"] == true);
    auto output = core::path_from_utf8(h.app->draft_json()["output"].get<std::string>());
    tc::test::write_file(output,3,1); // no draft revision changes
    CHECK(h.app->validate_draft()["canCreate"] == false);
    fs::remove(output);
    CHECK(h.app->validate_draft()["canCreate"] == true);
    h.app->update_draft({{"private",true},{"trackers",json::array()}},std::nullopt);
    CHECK(h.app->validate_draft()["canCreate"] == false);
    h.app->update_draft({{"private",false}},std::nullopt);
    CHECK(h.app->validate_draft()["canCreate"] == true);
    tc::test::write_file(root / "c.bin",300,3);
    auto source = h.app->draft_json()["sources"].front()["id"].get<std::string>();
    h.app->set_source_options(source,{{"recursive",true}},std::nullopt);
    h.app->wait_for_scan();
    CHECK(h.app->manifest_page(0,50,"Beta")["total"] == 3);
}

TEST_CASE("single and every batch mode enforce the same private policy", "[service][batch][profiles][U07]")
{
    auto const* mode = GENERATE("single", "perFile", "perChildFolder");
    INFO(mode);
    Harness h;
    fs::path const root = h.dir.path() / "PrivateBatch";
    tc::test::write_file(root / "child" / "a.bin", 32 * kib, 1);
    tc::test::write_file(root / "loose.bin", 32 * kib, 2);
    fs::path const output = h.dir.path() / "batch";
    fs::create_directory(output);
    h.app->add_sources({root});
    h.app->wait_for_scan();
    h.app->set_output(h.dir.path() / "single.torrent");
    h.app->plan_batch(mode, "rename", output);

    std::string code;
    SECTION("public preset in private torrent")
    {
        h.app->update_draft({{"private", true}}, std::nullopt);
        code = "PRIVATE_PUBLIC_TRACKER";
    }
    SECTION("private without an enabled tracker")
    {
        h.app->apply_profile("private", std::nullopt);
        h.app->update_draft({{"trackers", json::array({{{"url", "https://authorized.example/announce"}, {"enabled", false}}})}}, std::nullopt);
        code = "PRIVATE_WITHOUT_TRACKER";
    }
    SECTION("web seeds forbidden by private profile")
    {
        h.app->apply_profile("private", std::nullopt);
        h.app->update_draft({{"trackers", json::array({{{"url", "https://authorized.example/announce"}}})},
            {"webSeeds", json::array({"https://cdn.example/data/"})}}, std::nullopt);
        code = "PRIVATE_WEB_SEEDS";
    }
    REQUIRE_FALSE(code.empty());
    CHECK(has_issue(h.app->validate_draft(), code));
    CHECK_THROWS_AS(h.app->start_create(), ServiceError);
    CHECK_THROWS_AS(h.app->start_batch(), ServiceError);
    CHECK(h.app->jobs().snapshot().empty());
    CHECK(fs::is_empty(output));
}

TEST_CASE("authorized private batches use their validated settings snapshot", "[service][batch][profiles][U07]")
{
    Harness h;
    h.app->add_sources({h.dataset("Authorized", 2, 16 * kib)});
    h.app->wait_for_scan();
    h.app->apply_profile("private", std::nullopt);
    h.app->update_draft({{"trackers", json::array({{{"url", "https://authorized.example/announce"}}})}}, std::nullopt);
    // An invalid single-torrent destination is unrelated to batch destinations.
    h.app->set_output(h.dir.path() / "absent" / "single.torrent");
    CHECK(has_issue(h.app->validate_draft(), "OUTPUT_FOLDER_MISSING"));
    h.app->plan_batch("perFile", "rename", h.dir.path());
    h.gate.close();
    auto const ids = h.app->start_batch();
    REQUIRE(ids.size() == 2);
    h.app->update_draft({{"private", false}, {"trackers", json::array()}}, std::nullopt);
    h.gate.release();
    h.app->jobs().wait_idle();
    for (auto const& id : ids) {
        auto const j = h.app->jobs().find(id);
        REQUIRE(j->state == JobState::Succeeded);
        auto const meta = core::Metainfo::parse(tc::test::read_all(j->result->output));
        CHECK(meta.info().find("private")->as_int64() == 1);
        CHECK(meta.root().find("announce")->text() == "https://authorized.example/announce");
    }
}

TEST_CASE("automatic project piece decisions survive reopening and resaving", "[service][projects]")
{
    Harness h;
    h.app->add_sources({h.dataset("Auto", 2, 16 * kib)});
    h.app->wait_for_scan();
    h.app->update_draft({{"creationDate", "omit"}}, std::nullopt);
    auto const before = h.app->validate_draft()["summary"]["pieceLength"];
    fs::path const project = h.dir.path() / "auto.tcproject";
    h.app->save_project(project);
    auto saved = json::parse(tc::test::read_all(project));
    CHECK(saved["draft"]["pieceLength"] == 0);
    CHECK(saved["pieceSizePolicy"]["resolvedPieceLength"] == before);
    // A valid saved decision must be used even when today's auto policy differs.
    saved["pieceSizePolicy"]["resolvedPieceLength"] = 65536;
    tc::test::write_bytes(project, saved.dump());
    h.app->new_draft();
    auto const loaded = h.app->load_project(project);
    CHECK(loaded["pieceLength"] == 65536);
    h.app->wait_for_scan();
    CHECK(h.app->validate_draft()["summary"]["pieceLength"] == 65536);
    auto id = h.app->start_create();
    h.app->jobs().wait_idle();
    auto const result = h.app->jobs().find(id);
    REQUIRE(result->result);
    CHECK((result->state == JobState::Succeeded || result->state == JobState::SucceededWithWarnings));
    CHECK(h.app->jobs().find(id)->result->piece_length == 65536);
    std::string const first_bytes = tc::test::read_all(result->result->output);
    h.app->save_project(project);
    CHECK(load_project(project).piece_length == 65536);
    h.app->new_draft();
    h.app->load_project(project);
    h.app->wait_for_scan();
    h.app->update_draft({{"replaceExisting", true}}, std::nullopt);
    auto const repeated = h.app->start_create();
    h.app->jobs().wait_idle();
    auto const second = h.app->jobs().find(repeated);
    REQUIRE(second->result);
    CHECK(tc::test::read_all(second->result->output) == first_bytes);
    // Choosing Auto is the explicit way to discard the frozen decision.
    h.app->update_draft({{"pieceLength", 0}}, std::nullopt);
    CHECK(h.app->validate_draft()["summary"]["pieceLength"] == before);
}

TEST_CASE("project piece policies are validated before restoring", "[service][projects]")
{
    tc::test::TempDir dir;
    auto project = project_json(Draft{}, 65536);
    fs::path const path = dir.path() / "policy.tcproject";
    SECTION("unknown policy version") { project["pieceSizePolicy"]["version"] = 99; }
    SECTION("missing policy version") { project["pieceSizePolicy"].erase("version"); }
    SECTION("invalid policy object") { project["pieceSizePolicy"] = false; }
    SECTION("missing resolved size") { project["pieceSizePolicy"].erase("resolvedPieceLength"); }
    SECTION("negative size") { project["pieceSizePolicy"]["resolvedPieceLength"] = -1; }
    SECTION("oversized value") { project["pieceSizePolicy"]["resolvedPieceLength"] = std::uint64_t(-1); }
    SECTION("non integer") { project["pieceSizePolicy"]["resolvedPieceLength"] = "65536"; }
    SECTION("not a power of two") { project["pieceSizePolicy"]["resolvedPieceLength"] = 65537; }
    SECTION("too small") { project["pieceSizePolicy"]["resolvedPieceLength"] = 8192; }
    SECTION("contradictory manual choice") { project["draft"]["pieceLength"] = 16384; }
    tc::test::write_bytes(path, project.dump());
    CHECK_THROWS_AS(load_project(path), core::CoreError);
}

TEST_CASE("unresolved and legacy projects can still select automatic pieces", "[service][projects]")
{
    tc::test::TempDir dir;
    auto project = project_json(Draft{}, 0);
    SECTION("unresolved decision") {}
    SECTION("legacy without policy metadata") { project.erase("pieceSizePolicy"); }
    fs::path const path = dir.path() / "unresolved.tcproject";
    tc::test::write_bytes(path, project.dump());
    CHECK(load_project(path).piece_length == 0);
}

TEST_CASE("unexpected verification exceptions fail only their job and release the queue", "[service][verify][jobs]")
{
    class BrokenSource final : public core::PayloadSource {
    public:
        std::unique_ptr<core::PayloadReader> open(core::ManifestEntry const&) override
        {
            throw std::runtime_error("payload adapter failed");
        }
    };
    tc::test::TempDir dir;
    fs::path const file = dir.path() / "a.bin";
    tc::test::write_file(file, 16 * kib, 1);
    std::string const bytes = tc::test::make_torrent(file, core::TorrentFormat::V1, 16384);
    auto const mapping = core::map_to_root(core::Metainfo::parse(bytes), file);
    int failure = 0;
    SECTION("factory exception under scheduler lock") { failure = 1; }
    SECTION("adapter exception outside scheduler lock") { failure = 2; }
    SECTION("non standard exception") { failure = 3; }
    SECTION("allocation exception") { failure = 4; }
    std::atomic<int> calls{0};
    JobScheduler::Options options;
    options.max_concurrent = 1;
    options.payload_factory = [&]() -> std::unique_ptr<core::PayloadSource> {
        if (calls.fetch_add(1) == 0) {
            if (failure == 1) throw std::runtime_error("factory failed");
            if (failure == 2) return std::make_unique<BrokenSource>();
            if (failure == 3) throw 42;
            throw std::bad_alloc();
        }
        return core::make_file_payload_source();
    };
    JobScheduler jobs(options, {});
    auto const bad = jobs.enqueue_verify(VerifyJobSpec{"bad", bytes, mapping});
    auto const good = jobs.enqueue_verify(VerifyJobSpec{"good", bytes, mapping});
    jobs.wait_idle();
    auto const failed = jobs.find(bad);
    REQUIRE(failed->state == JobState::Failed);
    REQUIRE(failed->error);
    CHECK(failed->error->code == "INTERNAL");
    CHECK(failed->error->phase == "Verifying");
    CHECK(jobs.find(good)->state == JobState::Succeeded);
    CHECK_FALSE(jobs.has_active());
}

TEST_CASE("failed settings writes leave preferences profiles and draft unchanged", "[service][settings]")
{
    bool const parent_failure = GENERATE(true, false);
    Harness h;
    auto const saved = h.app->save_custom_profile("Existing profile");
    auto const before_settings = h.app->settings_json();
    auto const before_profiles = h.app->profiles_json();
    auto const before_draft = h.app->draft_json();
    auto const folder = h.dir.path() / "settings";
    auto const file = folder / "settings.json";
    auto const backup = h.dir.path() / "settings-backup";
    std::string const original = tc::test::read_all(file);
    if (parent_failure) {
        fs::rename(folder, backup);
        tc::test::write_file(folder, 8);
    } else {
        fs::rename(file, backup);
        fs::create_directory(file);
        tc::test::write_file(file / "keep", 8);
    }
    for (int operation = 0; operation != 4; ++operation) {
        INFO("operation " << operation << ", parent failure " << parent_failure);
        try {
            if (operation == 0) h.app->update_settings({{"theme", "dark"}, {"maxConcurrentJobs", 2}});
            else if (operation == 1) h.app->save_custom_profile("Not saved");
            else if (operation == 2) h.app->delete_custom_profile(saved["id"]);
            else h.app->apply_profile("private", std::nullopt);
            FAIL("save must fail");
        } catch (ServiceError const& error) {
            CHECK(error.code() == "SETTINGS_WRITE_FAILED");
            CHECK(error.retryable());
        }
        CHECK(h.app->settings_json() == before_settings);
        CHECK(h.app->profiles_json() == before_profiles);
        CHECK(h.app->draft_json() == before_draft);
        CHECK(tc::test::read_all(parent_failure ? backup / "settings.json" : backup) == original);
        if (!parent_failure)
            for (auto const& entry : fs::directory_iterator(folder))
                CHECK(entry.path().extension() != ".tmp");
    }
    if (parent_failure) { fs::remove(folder); fs::rename(backup, folder); }
    else { fs::remove_all(file); fs::rename(backup, file); }
    h.app->update_settings({{"theme", "dark"}});
    CHECK(load_settings(file).theme == "dark");
    auto const retry = h.app->save_custom_profile("Retry");
    CHECK(load_settings(file).custom_profiles.size() == 2);
    h.app->delete_custom_profile(retry["id"]);
    CHECK(load_settings(file).custom_profiles.size() == 1);
    h.app->apply_profile("private", std::nullopt);
    CHECK(load_settings(file).last_profile == "private");
}
