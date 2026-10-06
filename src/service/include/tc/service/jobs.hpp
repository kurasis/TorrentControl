#pragma once

// Job queue and state machine (specification sections 9.3 and 14.2).
//
// Every job owns a snapshot of its settings, taken when it is queued, so a
// later draft or profile edit never changes queued work. The scheduler runs
// at most `max_concurrent` jobs, each on its own thread; the native job state
// is authoritative and survives a renderer reload.

#include "tc/core/manifest.hpp"
#include "tc/core/pause.hpp"
#include "tc/core/payload_source.hpp"
#include "tc/core/torrent_engine.hpp"
#include "tc/core/verify.hpp"
#include "tc/service/draft.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tc::service {

// Errors raised by the service itself (not by the core), with a stable code.
class ServiceError : public std::runtime_error {
public:
    ServiceError(std::string code, std::string message, bool retryable = false)
        : std::runtime_error(std::move(message)), code_(std::move(code)), retryable_(retryable)
    {
    }
    std::string const& code() const noexcept { return code_; }
    bool retryable() const noexcept { return retryable_; }

private:
    std::string code_;
    bool retryable_;
};

enum class JobState {
    Draft,
    Scanning,
    Ready,
    Queued,
    Hashing,
    Pausing,
    Paused,
    Cancelling,
    Validating,
    Committing,
    Succeeded,
    SucceededWithWarnings,
    Failed,
    Cancelled,
};

std::string_view to_string(JobState s) noexcept;
bool is_terminal(JobState s) noexcept;
// The transitions the native service accepts; anything else is a bug.
bool is_valid_transition(JobState from, JobState to) noexcept;

enum class JobKind { Create, Verify };

// Sources to scan when the job starts (batch items). The scan result is the
// job's frozen manifest.
struct ScanRequest {
    std::vector<SourceSpec> sources;
    std::string name;
};

struct CreateJobSpec {
    std::string name;
    // Exactly one of `manifest` and `scan` is set.
    std::optional<core::Manifest> manifest;
    std::optional<ScanRequest> scan;
    core::CreateOptions options;
    // info.source, applied as an info edit after hashing (the payload hashes
    // are reused; only the identifiers change).
    std::string source_tag;
    std::filesystem::path output;
    bool replace_existing = false;
    std::string batch_id;
};

struct VerifyJobSpec {
    std::string name;
    std::string torrent_bytes;
    core::PayloadMapping mapping;
};

struct JobError {
    std::string code;
    std::string message;
    std::string phase;
    std::string source_id;
    bool retryable = false;
    std::optional<int> os_error;
};

struct JobResult {
    std::filesystem::path output;
    std::string torrent_name;
    std::string format;
    std::string infohash_v1;
    std::string infohash_v2;
    std::string magnet;
    int piece_length = 0;
    int num_pieces = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t padding_bytes = 0;
    std::size_t real_files = 0;
    std::size_t padding_files = 0;
    std::uint64_t metainfo_bytes = 0;
    bool replaced_existing = false;
    std::string guarantee_note;
    // Torrent paths and the local layout a client needs (virtual collections).
    std::vector<std::pair<std::string, std::string>> layout;
    std::vector<std::string> warnings;
};

struct JobSnapshot {
    std::string id;
    JobKind kind = JobKind::Create;
    std::string name;
    std::string batch_id;
    JobState state = JobState::Queued;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    double bytes_per_second = 0;
    std::optional<double> eta_seconds;
    std::string current_file;
    std::size_t files_done = 0;
    std::size_t files_total = 0;
    std::optional<JobError> error;
    std::optional<JobResult> result;
    // Verification jobs: the per-file report.
    nlohmann::json verify;
    // Redacted, human-readable log lines.
    std::vector<std::string> log;
    // Increments on every change of this job.
    std::uint64_t version = 0;
};

nlohmann::json to_json(JobSnapshot const& s, bool bridge_preview = false);

class JobScheduler {
public:
    using Listener = std::function<void(JobSnapshot const&)>;

    struct Options {
        int max_concurrent = 1;
        // Minimum interval between progress notifications of one job.
        std::chrono::milliseconds progress_interval{250};
        std::function<std::unique_ptr<core::PayloadSource>()> payload_factory;
        // Unix seconds, for log lines.
        std::function<std::int64_t()> clock;
    };

    JobScheduler(Options options, Listener listener);
    ~JobScheduler(); // cancels everything and joins

    JobScheduler(JobScheduler const&) = delete;
    JobScheduler& operator=(JobScheduler const&) = delete;

    std::string enqueue_create(CreateJobSpec spec);
    std::string enqueue_verify(VerifyJobSpec spec);

    // Throw ServiceError: JOB_NOT_FOUND, INVALID_STATE, COMMIT_IN_PROGRESS.
    void pause(std::string const& id);
    void resume(std::string const& id);
    void cancel(std::string const& id);
    // Removes finished jobs from the list.
    void clear_finished();

    void set_max_concurrent(int n);

    std::vector<JobSnapshot> snapshot() const;
    std::optional<JobSnapshot> find(std::string const& id) const;
    nlohmann::json bridge_page(std::size_t offset, std::size_t limit) const;
    nlohmann::json batch_status(std::string const& id) const;
    nlohmann::json verification_page(std::string const& id, std::size_t offset, std::size_t limit, bool errors_only) const;
    nlohmann::json verification_file(std::string const& id, std::size_t index) const;
    nlohmann::json layout_page(std::string const& id, std::size_t offset, std::size_t limit) const;
    nlohmann::json layout_row(std::string const& id, std::size_t index) const;
    nlohmann::json text_page(std::string const& id, std::string const& kind, std::size_t offset, std::size_t limit) const;
    nlohmann::json text_detail(std::string const& id, std::string const& kind, std::size_t index, std::string const& version) const;
    bool has_active() const;
    void cancel_all();
    // Blocks until no job is queued or running (tests and shutdown).
    void wait_idle();

private:
    struct Job;

    void schedule_locked();
    void run(Job& job);
    void run_create(Job& job);
    void run_verify(Job& job);
    void set_state(Job& job, JobState to, std::unique_lock<std::mutex>& lock);
    void notify(Job& job, std::unique_lock<std::mutex>& lock);
    void log(Job& job, std::string line);
    Job& get(std::string const& id);

    Options options_;
    Listener listener_;
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::map<std::string, std::unique_ptr<Job>> jobs_;
    std::vector<std::string> order_;
    struct BatchCounts { std::size_t total = 0, done = 0, failed = 0, cancelled = 0; };
    std::map<std::string, BatchCounts> batch_counts_;
    std::uint64_t next_id_ = 1;
    std::uint64_t collection_revision_ = 0;
    int running_ = 0;
    bool shutting_down_ = false;
};

} // namespace tc::service
