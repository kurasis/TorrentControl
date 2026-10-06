#include "tc/service/jobs.hpp"

#include "tc/core/error.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/output.hpp"
#include "tc/service/sources.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <stop_token>

namespace tc::service {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::string format_bytes(std::uint64_t n)
{
    char const* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

JobError error_from(core::CoreError const& e)
{
    return JobError{std::string(core::to_string(e.code())), e.what(), std::string(core::to_string(e.phase())),
        e.source_id(), e.retryable(), e.os_error()};
}

} // namespace

std::string_view to_string(JobState s) noexcept
{
    switch (s) {
    case JobState::Draft: return "Draft";
    case JobState::Scanning: return "Scanning";
    case JobState::Ready: return "Ready";
    case JobState::Queued: return "Queued";
    case JobState::Hashing: return "Hashing";
    case JobState::Pausing: return "Pausing";
    case JobState::Paused: return "Paused";
    case JobState::Cancelling: return "Cancelling";
    case JobState::Validating: return "Validating";
    case JobState::Committing: return "Committing";
    case JobState::Succeeded: return "Succeeded";
    case JobState::SucceededWithWarnings: return "SucceededWithWarnings";
    case JobState::Failed: return "Failed";
    case JobState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

bool is_terminal(JobState s) noexcept
{
    return s == JobState::Succeeded || s == JobState::SucceededWithWarnings || s == JobState::Failed
        || s == JobState::Cancelled;
}

bool is_valid_transition(JobState from, JobState to) noexcept
{
    using S = JobState;
    switch (from) {
    case S::Draft: return to == S::Scanning;
    case S::Scanning: return to == S::Ready || to == S::Hashing || to == S::Cancelling || to == S::Failed || to == S::Cancelled;
    case S::Ready: return to == S::Queued || to == S::Scanning;
    case S::Queued: return to == S::Scanning || to == S::Hashing || to == S::Cancelled || to == S::Failed;
    case S::Hashing:
        return to == S::Pausing || to == S::Cancelling || to == S::Validating || to == S::Failed || to == S::Succeeded;
    case S::Pausing:
        return to == S::Paused || to == S::Hashing || to == S::Cancelling || to == S::Validating || to == S::Failed;
    case S::Paused: return to == S::Hashing || to == S::Cancelling || to == S::Failed;
    case S::Cancelling: return to == S::Cancelled || to == S::Failed;
    case S::Validating:
        return to == S::Committing || to == S::Cancelling || to == S::Failed || to == S::Succeeded
            || to == S::SucceededWithWarnings;
    case S::Committing: return to == S::Succeeded || to == S::SucceededWithWarnings || to == S::Failed;
    case S::Succeeded:
    case S::SucceededWithWarnings:
    case S::Failed:
    case S::Cancelled: return false;
    }
    return false;
}

namespace {
std::string preview_text(std::string const& text, std::size_t limit = 512)
{
    if (text.size() <= limit) return text;
    while (limit && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80) --limit;
    return text.substr(0, limit) + "…";
}

json preview_row(json const& row, std::size_t index)
{
    json result = row;
    result["index"] = index;
    for (char const* key : {"path", "message"}) {
        auto const text = row.value(key, std::string{});
        result[key] = preview_text(text, 256);
        if (text.size() > 256) result["displayTruncated"] = true;
    }
    return result;
}
}

json to_json(JobSnapshot const& s, bool bridge_preview)
{
    json j{{"id", s.id}, {"kind", s.kind == JobKind::Create ? "create" : "verify"}, {"name", s.name},
        {"batchId", s.batch_id}, {"state", std::string(to_string(s.state))},
        {"bytesDone", std::to_string(s.bytes_done)}, {"bytesTotal", std::to_string(s.bytes_total)},
        {"bytesPerSecond", s.bytes_per_second}, {"currentFile", s.current_file}, {"filesDone", s.files_done},
        {"filesTotal", s.files_total}, {"version", std::to_string(s.version)}};
    j["log"] = json::array();
    auto const log_start = bridge_preview && s.log.size() > 5 ? s.log.size() - 5 : 0;
    for (std::size_t i = log_start; i < s.log.size(); ++i) j["log"].push_back(bridge_preview ? preview_text(s.log[i]) : s.log[i]);
    if (bridge_preview) {
        j["logTotal"] = s.log.size(); j["logTruncated"] = log_start != 0;
        j["name"] = preview_text(s.name); j["currentFile"] = preview_text(s.current_file);
    }
    j["etaSeconds"] = s.eta_seconds ? json(*s.eta_seconds) : json(nullptr);
    if (s.error) {
        json e{{"code", s.error->code}, {"message", s.error->message}, {"phase", s.error->phase},
            {"sourceId", s.error->source_id}, {"retryable", s.error->retryable}};
        if (s.error->os_error) e["osError"] = *s.error->os_error;
        if (bridge_preview) e["message"] = preview_text(s.error->message);
        j["error"] = std::move(e);
    }
    if (s.result) {
        auto const& r = *s.result;
        json layout = json::array();
        for (std::size_t i = 0; i < r.layout.size() && (!bridge_preview || i < 5); ++i) {
            auto const& [torrent_path, local] = r.layout[i];
            layout.push_back({{"torrentPath", bridge_preview ? preview_text(torrent_path) : torrent_path},
                {"local", bridge_preview ? preview_text(local) : local}});
        }
        j["result"] = {{"output", core::to_utf8(r.output)}, {"name", r.torrent_name}, {"format", r.format},
            {"infohashV1", r.infohash_v1}, {"infohashV2", r.infohash_v2}, {"magnet", r.magnet},
            {"pieceLength", r.piece_length}, {"numPieces", r.num_pieces}, {"payloadBytes", std::to_string(r.payload_bytes)},
            {"paddingBytes", std::to_string(r.padding_bytes)}, {"realFiles", r.real_files},
            {"paddingFiles", r.padding_files}, {"metainfoBytes", std::to_string(r.metainfo_bytes)},
            {"replacedExisting", r.replaced_existing}, {"guaranteeNote", r.guarantee_note}, {"layout", std::move(layout)},
            {"warnings", bridge_preview ? json::array() : json(r.warnings)}};
        if (bridge_preview) {
            auto& result = j["result"];
            result["output"] = preview_text(core::to_utf8(r.output));
            result["name"] = preview_text(r.torrent_name); result["magnet"] = preview_text(r.magnet);
            result["guaranteeNote"] = preview_text(r.guarantee_note);
            result["layoutTotal"] = r.layout.size(); result["layoutTruncated"] = r.layout.size() > 5 || r.real_files > r.layout.size();
            result["warnings"] = json::array();
            for (std::size_t i = 0; i < r.warnings.size() && i < 5; ++i) result["warnings"].push_back(preview_text(r.warnings[i]));
            result["warningsTotal"] = r.warnings.size();
            result["warningsTruncated"] = r.warnings.size() > 5
                || std::any_of(r.warnings.begin(), r.warnings.end(), [](auto const& text) { return text.size() > 512; });
        }
    }
    if (!s.verify.is_null()) {
        if (!bridge_preview) j["verify"] = s.verify;
        else {
            json report = json::object();
            for (auto it = s.verify.begin(); it != s.verify.end(); ++it) {
                if (it.key() == "files") continue;
                if (it.key() == "metainfoProblems") {
                    report[it.key()] = json::array();
                    for (std::size_t i = 0; i < it->size() && i < 5; ++i) report[it.key()].push_back(preview_text(it->at(i).get<std::string>()));
                } else report[it.key()] = it.value();
            }
            auto const& files = s.verify.at("files");
            report["files"] = json::array();
            for (std::size_t i = 0; i < files.size() && i < 2; ++i) report["files"].push_back(preview_row(files[i], i));
            report["filesTotal"] = files.size(); report["filesTruncated"] = files.size() > 2;
            j["verify"] = std::move(report);
        }
    }
    return j;
}

struct JobScheduler::Job {
    JobSnapshot snap;
    std::optional<CreateJobSpec> create;
    std::optional<VerifyJobSpec> verify;
    std::vector<std::size_t> verification_errors;
    std::stop_source stop;
    core::PauseControl pause;
    std::jthread thread;
    bool started = false;
    Clock::time_point last_notify{};
    Clock::time_point active_since{};
    std::chrono::duration<double> active_before{0};
};

JobScheduler::JobScheduler(Options options, Listener listener) : options_(std::move(options)), listener_(std::move(listener))
{
    if (!options_.payload_factory) options_.payload_factory = [] { return core::make_file_payload_source(); };
    if (!options_.clock) options_.clock = [] {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    };
    options_.max_concurrent = std::max(1, options_.max_concurrent);
}

JobScheduler::~JobScheduler()
{
    {
        std::lock_guard lock(mutex_);
        shutting_down_ = true;
        for (auto& [id, job] : jobs_) job->stop.request_stop();
    }
    for (auto& [id, job] : jobs_)
        if (job->thread.joinable()) job->thread.join();
}

JobScheduler::Job& JobScheduler::get(std::string const& id)
{
    auto it = jobs_.find(id);
    if (it == jobs_.end()) throw ServiceError("JOB_NOT_FOUND", "No such job");
    return *it->second;
}

void JobScheduler::log(Job& job, std::string line)
{
    std::int64_t const t = options_.clock();
    char stamp[16];
    std::snprintf(stamp, sizeof stamp, "%02d:%02d:%02d ", static_cast<int>((t / 3600) % 24), static_cast<int>((t / 60) % 60),
        static_cast<int>(t % 60));
    job.snap.log.push_back(stamp + std::move(line));
    if (job.snap.log.size() > 200) job.snap.log.erase(job.snap.log.begin());
}

void JobScheduler::notify(Job& job, std::unique_lock<std::mutex>& lock)
{
    ++job.snap.version;
    job.last_notify = Clock::now();
    JobSnapshot copy = job.snap;
    lock.unlock();
    if (listener_) listener_(copy);
    lock.lock();
}

void JobScheduler::set_state(Job& job, JobState to, std::unique_lock<std::mutex>& lock)
{
    if (!is_valid_transition(job.snap.state, to))
        throw std::logic_error("invalid job transition " + std::string(to_string(job.snap.state)) + " -> "
            + std::string(to_string(to)));
    bool const was_active = job.snap.state == JobState::Hashing;
    bool const now_active = to == JobState::Hashing;
    if (was_active && !now_active) job.active_before += Clock::now() - job.active_since;
    if (!was_active && now_active) job.active_since = Clock::now();
    job.snap.state = to;
    if (!job.snap.batch_id.empty() && is_terminal(to)) {
        auto& counts = batch_counts_[job.snap.batch_id];
        if (to == JobState::Succeeded || to == JobState::SucceededWithWarnings) ++counts.done;
        else if (to == JobState::Failed) ++counts.failed;
        else if (to == JobState::Cancelled) ++counts.cancelled;
    }
    if (to != JobState::Hashing) job.snap.eta_seconds.reset();
    log(job, std::string(to_string(to)));
    notify(job, lock);
    if (is_terminal(to)) idle_.notify_all();
}

std::string JobScheduler::enqueue_create(CreateJobSpec spec)
{
    if (spec.manifest.has_value() == spec.scan.has_value())
        throw std::invalid_argument("a create job needs either a manifest or a scan request");
    std::unique_lock lock(mutex_);
    auto job = std::make_unique<Job>();
    job->snap.id = "job-" + std::to_string(next_id_++);
    job->snap.kind = JobKind::Create;
    job->snap.name = spec.name;
    job->snap.batch_id = spec.batch_id;
    job->snap.state = JobState::Queued;
    job->create = std::move(spec);
    Job& ref = *job;
    std::string const id = ref.snap.id;
    jobs_.emplace(id, std::move(job));
    order_.push_back(id);
    if (!ref.snap.batch_id.empty()) ++batch_counts_[ref.snap.batch_id].total;
    ++collection_revision_;
    log(ref, "Queued");
    notify(ref, lock);
    schedule_locked();
    return id;
}

std::string JobScheduler::enqueue_verify(VerifyJobSpec spec)
{
    std::unique_lock lock(mutex_);
    auto job = std::make_unique<Job>();
    job->snap.id = "job-" + std::to_string(next_id_++);
    job->snap.kind = JobKind::Verify;
    job->snap.name = spec.name;
    job->snap.state = JobState::Queued;
    job->verify = std::move(spec);
    Job& ref = *job;
    std::string const id = ref.snap.id;
    jobs_.emplace(id, std::move(job));
    order_.push_back(id);
    ++collection_revision_;
    log(ref, "Queued for verification");
    notify(ref, lock);
    schedule_locked();
    return id;
}

void JobScheduler::schedule_locked()
{
    if (shutting_down_) return;
    for (auto const& id : order_) {
        if (running_ >= options_.max_concurrent) return;
        Job& job = *jobs_.at(id);
        if (job.started || job.snap.state != JobState::Queued) continue;
        job.started = true;
        ++running_;
        job.thread = std::jthread([this, &job] {
            run(job);
            std::lock_guard lock(mutex_);
            --running_;
            schedule_locked();
            idle_.notify_all();
        });
    }
}

void JobScheduler::run(Job& job)
{
    if (job.create) run_create(job);
    else run_verify(job);
}

void JobScheduler::run_create(Job& job)
{
    CreateJobSpec& spec = *job.create;
    std::stop_token const stop = job.stop.get_token();
    std::unique_lock lock(mutex_);

    auto finish_error = [&](JobError err) {
        bool const cancelled = err.code == "CANCELLED" || job.snap.state == JobState::Cancelling;
        if (cancelled) {
            if (job.snap.state != JobState::Cancelling && is_valid_transition(job.snap.state, JobState::Cancelling))
                set_state(job, JobState::Cancelling, lock);
            set_state(job, JobState::Cancelled, lock);
            return;
        }
        log(job, "Error " + err.code + ": " + err.message);
        job.snap.error = std::move(err);
        set_state(job, JobState::Failed, lock);
    };

    try {
        if (spec.scan) {
            set_state(job, JobState::Scanning, lock);
            ScanRequest const request = *spec.scan;
            lock.unlock();
            std::vector<std::filesystem::path> excluded;
            std::error_code ec;
            if (!std::filesystem::exists(spec.output, ec)) excluded.push_back(spec.output);
            core::Manifest m = build_manifest(request.sources, request.name, excluded, stop);
            lock.lock();
            spec.manifest = std::move(m);
        }
        core::Manifest const& manifest = *spec.manifest;

        lock.unlock();
        core::require_valid_manifest(manifest);
        core::check_output_target(spec.output, manifest);
        lock.lock();
        if (stop.stop_requested()) throw core::CoreError(core::ErrorCode::Cancelled, "Cancelled");

        job.snap.bytes_total = manifest.total_length();
        job.snap.files_total = manifest.entries.size();
        log(job, "Hashing " + std::to_string(manifest.entries.size()) + " files, " + format_bytes(job.snap.bytes_total));
        set_state(job, JobState::Hashing, lock);

        job.pause.set_listener([this, &job](bool parked) {
            std::unique_lock l(mutex_);
            if (parked && job.snap.state == JobState::Pausing) set_state(job, JobState::Paused, l);
        });

        core::CreateOptions options = spec.options;
        options.pause = &job.pause;
        auto payload = options_.payload_factory();
        lock.unlock();
        core::CreateResult r = core::create_torrent(manifest, options, *payload, stop, [&](core::CreateProgress const& p) {
            std::unique_lock l(mutex_);
            job.snap.bytes_done = p.payload_bytes_read;
            job.snap.files_done = p.files_completed;
            job.snap.current_file = p.current_file;
            auto const active = job.active_before
                + (job.snap.state == JobState::Hashing ? Clock::now() - job.active_since : Clock::duration::zero());
            double const secs = std::chrono::duration<double>(active).count();
            if (secs > 0.2) {
                job.snap.bytes_per_second = static_cast<double>(p.payload_bytes_read) / secs;
                if (job.snap.bytes_per_second > 0)
                    job.snap.eta_seconds = static_cast<double>(p.payload_bytes_total - p.payload_bytes_read) / job.snap.bytes_per_second;
            }
            bool const done = p.payload_bytes_read == p.payload_bytes_total;
            if (done || Clock::now() - job.last_notify >= options_.progress_interval) notify(job, l);
        });
        lock.lock();
        job.pause.set_listener({});
        job.pause.resume();

        if (stop.stop_requested()) throw core::CoreError(core::ErrorCode::Cancelled, "Cancelled");
        set_state(job, JobState::Validating, lock);
        lock.unlock();
        std::string bytes = std::move(r.torrent_bytes);
        if (!spec.source_tag.empty()) {
            core::InfoEdit edit;
            edit["source"] = core::bencode::Value::string(spec.source_tag);
            bytes = core::apply_info_edit(core::Metainfo::parse(bytes), edit).bytes;
        }
        core::Metainfo const meta = core::Metainfo::parse(bytes);
        if (auto problems = core::validate_metainfo(meta); !problems.empty())
            throw core::CoreError(core::ErrorCode::InvalidMetainfo, "Generated metainfo is invalid: " + problems.front());
        lock.lock();

        // The commit step is not interruptible: decide under the lock.
        if (stop.stop_requested()) throw core::CoreError(core::ErrorCode::Cancelled, "Cancelled");
        set_state(job, JobState::Committing, lock);
        lock.unlock();
        core::CommitOptions commit;
        commit.replace_existing = spec.replace_existing;
        commit.manifest = &manifest;
        core::CommitResult const committed = core::commit_output(spec.output, bytes, commit);
        lock.lock();

        JobResult res;
        res.output = committed.path;
        res.torrent_name = meta.name();
        res.format = std::string(core::to_string(meta.format()));
        if (meta.info_hashes().v1) res.infohash_v1 = core::to_hex(*meta.info_hashes().v1);
        if (meta.info_hashes().v2) res.infohash_v2 = core::to_hex(*meta.info_hashes().v2);
        res.magnet = core::make_magnet(meta);
        res.piece_length = r.piece_length;
        res.num_pieces = r.num_pieces;
        res.payload_bytes = r.payload_bytes;
        res.padding_bytes = r.padding_bytes;
        for (auto const& f : core::metainfo_files(meta)) (f.pad ? res.padding_files : res.real_files)++;
        res.metainfo_bytes = bytes.size();
        res.replaced_existing = committed.replaced_existing;
        res.guarantee_note = committed.guarantee_note;
        for (auto const& e : manifest.entries)
            if (res.layout.size() < 1000) res.layout.emplace_back(manifest.torrent_path_string(e), core::to_utf8(e.source_path));
        for (auto const& w : r.preflight.warnings) res.warnings.push_back(w.message);
        if (!committed.guarantee_note.empty()) res.warnings.push_back(committed.guarantee_note);
        bool const warnings = !res.warnings.empty();
        job.snap.result = std::move(res);
        log(job, "Saved " + core::to_utf8(committed.path));
        set_state(job, warnings ? JobState::SucceededWithWarnings : JobState::Succeeded, lock);
    } catch (core::CoreError const& e) {
        if (!lock.owns_lock()) lock.lock();
        job.pause.set_listener({});
        finish_error(error_from(e));
    } catch (std::exception const& e) {
        if (!lock.owns_lock()) lock.lock();
        job.pause.set_listener({});
        finish_error(JobError{"INTERNAL", e.what(), "", "", false, std::nullopt});
    }
}

void JobScheduler::run_verify(Job& job)
{
    VerifyJobSpec const& spec = *job.verify;
    std::stop_token const stop = job.stop.get_token();
    std::unique_lock lock(mutex_);
    auto finish_error = [&](JobError error) {
        job.pause.set_listener({});
        job.pause.resume();
        if (error.code == "CANCELLED" || job.snap.state == JobState::Cancelling) {
            if (job.snap.state != JobState::Cancelling && is_valid_transition(job.snap.state, JobState::Cancelling))
                set_state(job, JobState::Cancelling, lock);
            set_state(job, JobState::Cancelled, lock);
        } else {
            log(job, "Error " + error.code + ": " + error.message);
            job.snap.error = std::move(error);
            set_state(job, JobState::Failed, lock);
        }
    };
    try {
        core::Metainfo const meta = core::Metainfo::parse(spec.torrent_bytes);
        set_state(job, JobState::Hashing, lock);
        job.pause.set_listener([this, &job](bool parked) {
            std::unique_lock l(mutex_);
            if (parked && job.snap.state == JobState::Pausing) set_state(job, JobState::Paused, l);
        });
        core::VerifyOptions vo;
        vo.pause = &job.pause;
        auto payload = options_.payload_factory();
        lock.unlock();
        core::VerifyResult const r = core::verify_payload(meta, spec.mapping, *payload, stop,
            [&](core::CreateProgress const& p) {
                std::unique_lock l(mutex_);
                job.snap.bytes_done = p.payload_bytes_read;
                job.snap.bytes_total = p.payload_bytes_total;
                job.snap.files_done = p.files_completed;
                job.snap.files_total = p.files_total;
                job.snap.current_file = p.current_file;
                if (Clock::now() - job.last_notify >= options_.progress_interval) notify(job, l);
            },
            vo);
        lock.lock();
        job.pause.set_listener({});
        job.pause.resume();
        if (stop.stop_requested()) throw core::CoreError(core::ErrorCode::Cancelled, "Cancelled");

        json files = json::array();
        for (auto const& f : r.files)
            files.push_back({{"path", f.torrent_path}, {"status", std::string(core::to_string(f.status))},
                {"message", f.message}, {"badV1Pieces", std::to_string(f.bad_v1_pieces)},
                {"badV2Pieces", std::to_string(f.bad_v2_pieces)}});
        job.verification_errors.clear();
        for (std::size_t i = 0; i < files.size(); ++i)
            if (files[i]["status"] != "ok") job.verification_errors.push_back(i);
        job.snap.verify = {{"ok", r.ok}, {"metainfoProblems", r.metainfo_problems},
            {"v1PiecesTotal", std::to_string(r.v1_pieces_total)}, {"v1PiecesBad", std::to_string(r.v1_pieces_bad)},
            {"v2FilesChecked", std::to_string(r.v2_files_checked)}, {"v2FilesBad", std::to_string(r.v2_files_bad)},
            {"payloadBytesRead", std::to_string(r.payload_bytes_read)}, {"files", std::move(files)}};
        set_state(job, JobState::Validating, lock);
        if (r.ok) {
            set_state(job, JobState::Succeeded, lock);
        } else {
            job.snap.error = JobError{"PAYLOAD_MISMATCH",
                r.metainfo_problems.empty() ? "The payload does not match the torrent" : "The torrent itself is invalid",
                "Verifying", "", false, std::nullopt};
            set_state(job, JobState::Failed, lock);
        }
    } catch (core::CoreError const& e) {
        if (!lock.owns_lock()) lock.lock();
        finish_error(error_from(e));
    } catch (std::exception const& e) {
        if (!lock.owns_lock()) lock.lock();
        finish_error(JobError{"INTERNAL", e.what(), "Verifying", "", false, std::nullopt});
    } catch (...) {
        if (!lock.owns_lock()) lock.lock();
        finish_error(JobError{"INTERNAL", "Unknown verification error", "Verifying", "", false, std::nullopt});
    }
}

void JobScheduler::pause(std::string const& id)
{
    std::unique_lock lock(mutex_);
    Job& job = get(id);
    if (job.snap.state != JobState::Hashing)
        throw ServiceError("INVALID_STATE", "Only a job that is hashing can be paused");
    job.pause.request_pause();
    set_state(job, JobState::Pausing, lock);
}

void JobScheduler::resume(std::string const& id)
{
    std::unique_lock lock(mutex_);
    Job& job = get(id);
    if (job.snap.state != JobState::Paused && job.snap.state != JobState::Pausing)
        throw ServiceError("INVALID_STATE", "Only a paused job can be resumed");
    set_state(job, JobState::Hashing, lock);
    job.pause.resume();
}

void JobScheduler::cancel(std::string const& id)
{
    std::unique_lock lock(mutex_);
    Job& job = get(id);
    switch (job.snap.state) {
    case JobState::Queued:
        if (!job.started) {
            set_state(job, JobState::Cancelled, lock);
            return;
        }
        break;
    case JobState::Committing:
        throw ServiceError("COMMIT_IN_PROGRESS",
            "The torrent file is being moved into place; this short step cannot be interrupted", true);
    case JobState::Cancelling: return;
    default:
        if (is_terminal(job.snap.state)) throw ServiceError("INVALID_STATE", "The job has already finished");
        break;
    }
    job.stop.request_stop();
    if (job.snap.state != JobState::Queued) set_state(job, JobState::Cancelling, lock);
}

void JobScheduler::clear_finished()
{
    std::unique_lock lock(mutex_);
    std::vector<std::string> keep;
    for (auto const& id : order_) {
        auto it = jobs_.find(id);
        if (is_terminal(it->second->snap.state)) {
            if (it->second->thread.joinable()) {
                // The thread is finishing its bookkeeping; join outside the lock.
                std::jthread t = std::move(it->second->thread);
                lock.unlock();
                t.join();
                lock.lock();
            }
            jobs_.erase(it);
        } else {
            keep.push_back(id);
        }
    }
    order_ = std::move(keep);
    ++collection_revision_;
}

void JobScheduler::set_max_concurrent(int n)
{
    std::lock_guard lock(mutex_);
    options_.max_concurrent = std::clamp(n, 1, 8);
    schedule_locked();
}

std::vector<JobSnapshot> JobScheduler::snapshot() const
{
    std::lock_guard lock(mutex_);
    std::vector<JobSnapshot> out;
    for (auto const& id : order_) out.push_back(jobs_.at(id)->snap);
    return out;
}

std::optional<JobSnapshot> JobScheduler::find(std::string const& id) const
{
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return std::nullopt;
    return it->second->snap;
}

json JobScheduler::batch_status(std::string const& id) const
{
    std::lock_guard lock(mutex_);
    auto found = batch_counts_.find(id);
    BatchCounts const counts = found == batch_counts_.end() ? BatchCounts{} : found->second;
    auto const [total, done, failed, cancelled] = counts;
    return {{"batchId", id}, {"total", total}, {"done", done}, {"failed", failed}, {"cancelled", cancelled},
        {"finished", total > 0 && done + failed + cancelled == total}};
}

json JobScheduler::bridge_page(std::size_t offset, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    json rows = json::array();
    std::size_t bytes = 0;
    limit = std::min(limit, std::size_t(50));
    for (std::size_t i = offset; i < order_.size() && rows.size() < limit; ++i) {
        auto row = to_json(jobs_.at(order_[i])->snap, true);
        auto const size = row.dump(-1, ' ', false, json::error_handler_t::replace).size();
        if (size > 256 * 1024) throw ServiceError("RESULT_TOO_LARGE", "The job summary exceeds its display budget");
        if (!rows.empty() && size > 256 * 1024 - bytes) break;
        bytes += size;
        rows.push_back(std::move(row));
    }
    auto const next = offset + rows.size();
    return {{"jobs", std::move(rows)}, {"total", order_.size()},
        {"nextOffset", next < order_.size() ? json(next) : json(nullptr)},
        {"collectionRevision", std::to_string(collection_revision_)}};
}

json JobScheduler::verification_page(std::string const& id, std::size_t offset, std::size_t limit, bool errors_only) const
{
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) throw ServiceError("JOB_NOT_FOUND", "No such verification job");
    auto const& job = *it->second;
    if (job.snap.kind != JobKind::Verify) throw ServiceError("INVALID_STATE", "This job has no verification report");
    json rows = json::array();
    auto const& report = job.snap.verify;
    auto const files = report.is_null() ? 0 : report.at("files").size();
    auto const total = errors_only ? job.verification_errors.size() : files;
    limit = std::min(limit, std::size_t(250));
    for (std::size_t i = offset; i < total && rows.size() < limit; ++i) {
        auto index = errors_only ? job.verification_errors[i] : i;
        rows.push_back(preview_row(report.at("files")[index], index));
    }
    return {{"rows", std::move(rows)}, {"total", total}, {"filesTotal", files}, {"version", std::to_string(job.snap.version)}};
}

json JobScheduler::verification_file(std::string const& id, std::size_t index) const
{
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) throw ServiceError("JOB_NOT_FOUND", "No such verification job");
    auto const& report = it->second->snap.verify;
    if (report.is_null() || index >= report.at("files").size()) throw ServiceError("NOT_FOUND", "No such verified file");
    return {{"file", report.at("files")[index]}, {"version", std::to_string(it->second->snap.version)}};
}

json JobScheduler::layout_page(std::string const& id, std::size_t offset, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) throw ServiceError("JOB_NOT_FOUND", "No such job");
    if (!it->second->snap.result) throw ServiceError("INVALID_STATE", "This job has no creation result");
    auto const& result = *it->second->snap.result;
    json rows = json::array();
    limit = std::min(limit, std::size_t(50));
    for (std::size_t i = offset; i < result.layout.size() && rows.size() < limit; ++i)
        rows.push_back({{"torrentPath", preview_text(result.layout[i].first)}, {"local", preview_text(result.layout[i].second)},
            {"index", i}, {"displayTruncated", result.layout[i].first.size() > 512 || result.layout[i].second.size() > 512}});
    return {{"rows", std::move(rows)}, {"total", result.layout.size()}, {"realFiles", result.real_files},
        {"complete", result.layout.size() == result.real_files}};
}

json JobScheduler::layout_row(std::string const& id, std::size_t index) const
{
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) throw ServiceError("JOB_NOT_FOUND", "No such job");
    if (!it->second->snap.result || index >= it->second->snap.result->layout.size())
        throw ServiceError("NOT_FOUND", "No such retained mapping row");
    auto const& row = it->second->snap.result->layout[index];
    return {{"torrentPath", row.first}, {"local", row.second}};
}

json JobScheduler::text_page(std::string const& id, std::string const& kind, std::size_t offset, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) throw ServiceError("JOB_NOT_FOUND", "No such job");
    auto const& job = it->second->snap;
    if (kind != "log" && kind != "warnings") throw ServiceError("INVALID_PAYLOAD", "Unknown job text collection");
    if (kind == "warnings" && !job.result) throw ServiceError("INVALID_STATE", "This job has no creation warnings");
    auto const& lines = kind == "log" ? job.log : job.result->warnings;
    json rows = json::array();
    limit = std::min(limit, std::size_t(50));
    for (std::size_t i = offset; i < lines.size() && rows.size() < limit; ++i)
        rows.push_back({{"index", i}, {"text", preview_text(lines[i])}, {"displayTruncated", lines[i].size() > 512}});
    return {{"rows", std::move(rows)}, {"total", lines.size()}, {"version", std::to_string(job.version)}};
}

json JobScheduler::text_detail(std::string const& id, std::string const& kind, std::size_t index, std::string const& version) const
{
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) throw ServiceError("JOB_NOT_FOUND", "No such job");
    auto const& job = it->second->snap;
    if (!version.empty() && version != std::to_string(job.version))
        throw ServiceError("STALE_JOB", "The job changed; refresh its text page", true);
    if (kind != "log" && kind != "warnings") throw ServiceError("INVALID_PAYLOAD", "Unknown job text collection");
    if (kind == "warnings" && !job.result) throw ServiceError("INVALID_STATE", "This job has no creation warnings");
    auto const& lines = kind == "log" ? job.log : job.result->warnings;
    if (index >= lines.size()) throw ServiceError("NOT_FOUND", "No such job text row");
    return {{"text", lines[index]}, {"version", std::to_string(job.version)}};
}

bool JobScheduler::has_active() const
{
    std::lock_guard lock(mutex_);
    return std::any_of(jobs_.begin(), jobs_.end(), [](auto const& kv) { return !is_terminal(kv.second->snap.state); });
}

void JobScheduler::cancel_all()
{
    std::vector<std::string> ids;
    {
        std::lock_guard lock(mutex_);
        for (auto const& id : order_)
            if (!is_terminal(jobs_.at(id)->snap.state)) ids.push_back(id);
    }
    for (auto const& id : ids) {
        try {
            cancel(id);
        } catch (ServiceError const&) {
            // Committing jobs finish on their own.
        }
    }
}

void JobScheduler::wait_idle()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] {
        return running_ == 0
            && std::all_of(jobs_.begin(), jobs_.end(), [](auto const& kv) { return is_terminal(kv.second->snap.state); });
    });
}

} // namespace tc::service
