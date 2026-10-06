#include "tc/core/error.hpp"
#include "tc/core/torrent_engine.hpp"
#include "tc/service/jobs.hpp"
#include "test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include "windows_payload_reader.hpp"
#include <windows.h>
#endif

using namespace tc::core;
using namespace tc::service;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
struct Block {
    std::mutex mutex;
    std::condition_variable_any cv;
    bool released = false;
    bool entered = false;
    bool cooperative = true;
    bool probe = false;
    std::atomic<int> live_readers{0};
    std::atomic<bool> got_stop{false};
    bool wait_entered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return entered; });
    }
    void release() {
        { std::lock_guard lock(mutex); released = true; }
        cv.notify_all();
    }
    void wait(std::stop_token stop) {
        std::unique_lock lock(mutex);
        entered = true;
        got_stop = stop.stop_possible();
        cv.notify_all();
        if (!cv.wait(lock, stop, [&] { return released; }))
            throw CoreError(ErrorCode::Cancelled, "Controlled pending read cancelled");
    }
};
class BlockingSource final : public PayloadSource {
public:
    explicit BlockingSource(Block& block) : block_(block), inner_(make_file_payload_source()) {}
    std::unique_ptr<PayloadReader> open(ManifestEntry const& entry) override {
        struct Reader final : PayloadReader {
            Block& block;
            std::unique_ptr<PayloadReader> inner;
            Reader(Block& b, std::unique_ptr<PayloadReader> r) : block(b), inner(std::move(r)) { ++block.live_readers; }
            ~Reader() override { --block.live_readers; }
            std::size_t read(std::span<std::byte> buffer) override {
                if (!block.probe || buffer.size() == 1) block.wait({});
                return inner->read(buffer);
            }
            std::size_t read(std::span<std::byte> buffer, std::stop_token stop) override {
                if (!block.cooperative) return PayloadReader::read(buffer, stop);
                if (!block.probe || buffer.size() == 1) block.wait(stop);
                return inner->read(buffer, stop);
            }
            std::optional<native::FileObservation> observe() override { return inner->observe(); }
        };
        return std::make_unique<Reader>(block_, inner_->open(entry));
    }
private:
    Block& block_;
    std::unique_ptr<PayloadSource> inner_;
};
bool terminal_within(JobScheduler& jobs, std::string const& id) {
    auto const deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (is_terminal(jobs.find(id)->state)) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
CreateJobSpec spec_for(fs::path const& source, fs::path const& output) {
    CreateJobSpec spec;
    spec.name = "Controlled read";
    spec.manifest = scan_source(source);
    spec.options.piece_length = 16384;
    spec.options.buffer_budget = 32768;
    spec.output = output;
    return spec;
}
}

TEST_CASE("stop-aware compatibility adapters reject cancellation before opening or reading", "[engine][W08]")
{
    struct Reader final : PayloadReader {
        int calls = 0;
        std::size_t read(std::span<std::byte>) override { ++calls; return 0; }
    } reader;
    struct Source final : PayloadSource {
        int calls = 0;
        std::unique_ptr<PayloadReader> open(ManifestEntry const&) override { ++calls; return {}; }
    } source;
    std::stop_source stop;
    stop.request_stop();
    std::array<std::byte, 1> bytes{};
    CHECK_THROWS_AS(static_cast<PayloadReader&>(reader).read(bytes, stop.get_token()), CoreError);
    CHECK_THROWS_AS(static_cast<PayloadSource&>(source).open({}, stop.get_token()), CoreError);
    CHECK(reader.calls == 0);
    CHECK(source.calls == 0);
}

TEST_CASE("pending payload and EOF reads cancel creation and verification without reports or output", "[service][W08]")
{
    bool const verify = GENERATE(false, true);
    bool const probe = GENERATE(false, true);
    tc::test::TempDir dir;
    auto const file = dir.path() / "source.bin";
    auto const output = dir.path() / "cancelled.torrent";
    tc::test::write_file(file, 65536);
    Block block;
    block.probe = probe;
    JobScheduler::Options options;
    options.payload_factory = [&] { return std::make_unique<BlockingSource>(block); };
    JobScheduler jobs(options, {});
    auto spec = spec_for(file, output);
    std::string id;
    if (verify) {
        auto native = make_file_payload_source();
        auto torrent = create_torrent(*spec.manifest, spec.options, *native);
        auto parsed = Metainfo::parse(torrent.torrent_bytes);
        id = jobs.enqueue_verify({"Verify pending read", torrent.torrent_bytes, map_to_root(parsed, file)});
    } else id = jobs.enqueue_create(spec);
    bool const entered = block.wait_entered();
    if (!is_terminal(jobs.find(id)->state)) jobs.cancel(id);
    bool const finished = terminal_within(jobs, id);
    // Release even on failure, so an adapter regression fails rather than hangs.
    block.release();
    jobs.wait_idle();
    REQUIRE(entered);
    CHECK(block.got_stop);
    CHECK(finished);
    auto result = jobs.find(id);
    CHECK(result->state == JobState::Cancelled);
    CHECK(result->verify.is_null());
    CHECK_FALSE(result->result);
    CHECK_FALSE(fs::exists(output));
    CHECK(block.live_readers == 0);
    auto next = jobs.enqueue_create(std::move(spec));
    jobs.wait_idle();
    CHECK(jobs.find(next)->state == JobState::Succeeded);
    CHECK(fs::exists(output));
}

TEST_CASE("uncancellable adapters retain their reader and Cancelling state until completion", "[service][W08]")
{
    tc::test::TempDir dir;
    auto const file = dir.path() / "source.bin";
    auto const output = dir.path() / "cancelled.torrent";
    tc::test::write_file(file, 65536);
    Block block;
    block.cooperative = false;
    JobScheduler::Options options;
    options.payload_factory = [&] { return std::make_unique<BlockingSource>(block); };
    JobScheduler jobs(options, {});
    auto id = jobs.enqueue_create(spec_for(file, output));
    bool const entered = block.wait_entered();
    if (!is_terminal(jobs.find(id)->state)) jobs.cancel(id);
    auto const snapshot = jobs.bridge_page(0, 50); // stays available during a stalled read
    auto const before = jobs.find(id)->state;
    int const readers = block.live_readers;
    bool const no_output = !fs::exists(output);
    block.release();
    jobs.wait_idle();
    REQUIRE(entered);
    CHECK(before == JobState::Cancelling);
    CHECK(snapshot["jobs"][0]["state"] == "Cancelling");
    CHECK(readers == 1);
    CHECK(no_output);
    CHECK(jobs.find(id)->state == JobState::Cancelled);
    CHECK(block.live_readers == 0);
    CHECK_FALSE(fs::exists(output));
}

TEST_CASE("scheduler shutdown cancels a pending stop-aware reader before joining", "[service][W08]")
{
    tc::test::TempDir dir;
    auto const file = dir.path() / "source.bin";
    tc::test::write_file(file, 65536);
    Block block;
    JobScheduler::Options options;
    options.payload_factory = [&] { return std::make_unique<BlockingSource>(block); };
    auto jobs = std::make_unique<JobScheduler>(options, JobScheduler::Listener{});
    jobs->enqueue_create(spec_for(file, dir.path() / "cancelled.torrent"));
    bool const entered = block.wait_entered();
    auto destroyed = std::async(std::launch::async, [&] { jobs.reset(); });
    bool const finished = destroyed.wait_for(2s) == std::future_status::ready;
    block.release();
    destroyed.get();
    REQUIRE(entered);
    CHECK(finished);
    CHECK(block.live_readers == 0);
    CHECK_FALSE(fs::exists(dir.path() / "cancelled.torrent"));
}

#ifdef _WIN32
TEST_CASE("Windows cancels a real kernel-pending overlapped read and reuses the handle", "[engine][windows][W08]")
{
    struct Handle {
        HANDLE value;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    static std::atomic<unsigned> next{0};
    auto name = L"\\\\.\\pipe\\tc-read-cancel-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++next);
    Handle server{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr)};
    REQUIRE(server.value != INVALID_HANDLE_VALUE);
    HANDLE const client = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    REQUIRE(client != INVALID_HANDLE_VALUE);
    auto reader = tc::core::detail::take_windows_payload_reader(client, "controlled pending pipe");
    BOOL const connected = ConnectNamedPipe(server.value, nullptr);
    REQUIRE((connected != FALSE || GetLastError() == ERROR_PIPE_CONNECTED));
    std::stop_source stop;
    std::array<std::byte, 8> bytes{};
    std::promise<ErrorCode> result;
    auto future = result.get_future();
    std::jthread worker([&] {
        try { reader->read(bytes, stop.get_token()); result.set_value(ErrorCode::EngineError); }
        catch (CoreError const& e) { result.set_value(e.code()); }
        catch (...) { result.set_exception(std::current_exception()); }
    });
    BOOL pending = FALSE;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!pending && std::chrono::steady_clock::now() < deadline) {
        GetThreadIOPendingFlag(worker.native_handle(), &pending);
        std::this_thread::sleep_for(1ms);
    }
    stop.request_stop();
    bool const settled = future.wait_for(2s) == std::future_status::ready;
    DWORD written = 0;
    if (!settled) WriteFile(server.value, "x", 1, &written, nullptr); // bounded failure cleanup
    auto const error = future.get();
    worker.join();
    CHECK(pending != FALSE); // proves actual kernel-pending I/O, not a sleeping fake
    CHECK(settled);
    CHECK(error == ErrorCode::Cancelled);
    // The previous stop callback and OVERLAPPED are gone; a fresh read works.
    REQUIRE(WriteFile(server.value, "y", 1, &written, nullptr) != FALSE);
    CHECK(reader->read(bytes) == 1);
    CHECK(bytes[0] == std::byte{'y'});
}
#endif
