#pragma once

// The native application model behind the bridge (specification sections 4,
// 9.3, 13 and 14). Platform independent: the Windows host supplies dialogs and
// shell actions, and the bridge maps operations onto these methods.
//
// Threading: the Windows command worker calls public methods; scanning and
// jobs have their own workers. Events are delivered through the sink from any
// thread, and the host marshals them to the UI thread.

#include "tc/core/metainfo.hpp"
#include "tc/core/field_registry.hpp"
#include "tc/service/batch.hpp"
#include "tc/service/draft.hpp"
#include "tc/service/diagnostics.hpp"
#include "tc/service/jobs.hpp"
#include "tc/service/profiles.hpp"
#include "tc/service/storage.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace tc::service {

class AppService {
public:
    // Events: {"type":"job","job":{...}}, {"type":"scan",...}, {"type":"draft",...}.
    using EventSink = std::function<void(nlohmann::json const& event)>;

    struct Options {
        // Empty: settings are kept in memory only.
        std::filesystem::path settings_path;
        JobScheduler::Options jobs;
        // Unix seconds for the "now" creation-date policy.
        std::function<std::int64_t()> now;
    };

    AppService(Options options, EventSink sink);
    ~AppService();

    AppService(AppService const&) = delete;
    AppService& operator=(AppService const&) = delete;

    // ---- Draft ----------------------------------------------------------
    // Mutations take the revision the page last saw; a mismatch throws
    // ServiceError(STALE_REVISION) so an edit is never applied to a draft the
    // user has not seen. std::nullopt skips the check (native callers).
    nlohmann::json draft_json() const;
    // Bounded collections stay native-owned. Revision is mandatory for mutable views.
    nlohmann::json model_page(std::string const& model, std::string const& key, std::string const& owner,
        std::size_t offset, std::size_t limit, std::string const& revision) const;
    nlohmann::json edit_draft_row(std::string const& key, std::string const& owner, std::size_t index,
        std::string const& action, nlohmann::json const& value, std::optional<std::uint64_t> revision);
    nlohmann::json model_text(std::string const& model, std::string const& key, std::string const& owner,
        std::size_t offset, std::string const& revision) const;
    nlohmann::json add_sources(std::vector<std::filesystem::path> const& paths);
    nlohmann::json remove_source(std::string const& id, std::optional<std::uint64_t> revision);
    nlohmann::json set_source_options(std::string const& id, nlohmann::json const& options, std::optional<std::uint64_t> revision);
    nlohmann::json update_draft(nlohmann::json const& patch, std::optional<std::uint64_t> revision);
    nlohmann::json set_output(std::filesystem::path const& path);
    nlohmann::json new_draft();
    // Profile switch preview, apply (undoable) and undo.
    nlohmann::json plan_profile(std::string const& id) const;
    nlohmann::json apply_profile(std::string const& id, std::optional<std::uint64_t> revision);
    nlohmann::json undo();

    // ---- Scan and review --------------------------------------------------
    nlohmann::json scan_state() const;
    // Blocks until the current scan finishes (tests and the CLI).
    void wait_for_scan();
    nlohmann::json manifest_page(std::size_t offset, std::size_t limit, std::string const& filter) const;
    nlohmann::json skipped_page(std::size_t offset, std::size_t limit) const;
    // Issues (errors block creation), layout summary and the review of active
    // advanced settings.
    nlohmann::json validate_draft() const;
    std::string start_create();

    // ---- Batch --------------------------------------------------------------
    nlohmann::json plan_batch(std::string const& mode, std::string const& policy, std::filesystem::path const& output_dir);
    // `overrides`: {"item-1": {"policy":"replace","included":true}, ...}
    nlohmann::json update_batch(nlohmann::json const& overrides, std::string const& revision = {});
    std::vector<std::string> start_batch(std::string const& revision = {});
    nlohmann::json started_batch() const;

    // ---- Jobs -----------------------------------------------------------------
    JobScheduler& jobs() { return *jobs_; }
    void clear_finished_jobs();
    nlohmann::json retention_summary() const;
    bool has_active_jobs() const { return jobs_->has_active(); }

    // ---- Existing torrents -----------------------------------------------------
    nlohmann::json open_torrent(std::filesystem::path const& path);
    nlohmann::json torrent_files_page(std::string const& id, std::size_t offset, std::size_t limit) const;
    std::string verify_torrent(std::string const& id, std::filesystem::path const& payload_root);
    std::string magnet_for(std::string const& torrent_or_job_id) const;
    std::filesystem::path torrent_path(std::string const& id) const;
    std::shared_ptr<core::Metainfo const> torrent_metainfo(std::string const& id) const;
    nlohmann::json preview_torrent_edit(std::string const& id, core::OuterEdit const& outer,
        core::InfoEdit const& info, bool remove_signatures, nlohmann::json const& display_changes = nlohmann::json::array());
    nlohmann::json editor_preview(std::string const& token) const;
    nlohmann::json choose_editor_output(std::string const& token, std::filesystem::path const& path);
    nlohmann::json save_torrent_edit(std::string const& token, bool replace_existing);
    nlohmann::json diagnostic_targets(std::string const& kind, std::string const& torrent_id = {}) const;
    std::string start_diagnostics(std::string const& kind, std::string const& torrent_id, NetworkPolicy policy);
    DiagnosticsService& diagnostics() { return *diagnostics_; }
    std::string update_tracker_catalog(NetworkPolicy policy);
    nlohmann::json plan_catalog_apply() const;
    nlohmann::json apply_catalog(std::string const& checksum, std::optional<std::uint64_t> revision);

    // ---- Projects and settings -------------------------------------------------
    void save_project(std::filesystem::path const& path);
    nlohmann::json load_project(std::filesystem::path const& path);
    nlohmann::json settings_json() const;
    nlohmann::json update_settings(nlohmann::json const& patch);
    nlohmann::json profiles_json() const;
    nlohmann::json profiles_state() const;
    nlohmann::json save_custom_profile(std::string const& name);
    nlohmann::json delete_custom_profile(std::string const& id);
    nlohmann::json export_profile(std::string const& id, bool include_secrets) const;

    // Everything a freshly loaded page needs (renderer recovery, U03).
    nlohmann::json snapshot() const;

    // Suggested names for native dialogs.
    std::string suggested_name() const;
    std::filesystem::path suggested_folder() const;

private:
    struct ScanResult;
    struct OpenedTorrent {
        std::filesystem::path path;
        std::shared_ptr<core::Metainfo const> meta;
        nlohmann::json overview;
    };
    struct PendingEdit {
        std::string torrent_id;
        std::shared_ptr<core::MetadataPreview const> candidate;
        nlohmann::json summary;
        std::filesystem::path output;
    };

    void check_revision(std::optional<std::uint64_t> revision) const;
    std::vector<ProbeTarget> collect_diagnostic_targets_locked(std::string const& kind, std::string const& torrent_id) const;
    void bump_locked(bool sources_changed);
    void start_scan_locked();
    void scan_worker(std::stop_token shutdown);
    void refresh_output_locked();
    std::optional<Profile> find_profile_locked(std::string const& id) const;
    nlohmann::json draft_json_locked() const;
    nlohmann::json model_page_locked(std::string const& model, std::string const& key, std::string const& owner,
        std::size_t offset, std::size_t limit, std::string const& revision) const;
    nlohmann::json batch_json_locked() const;
    nlohmann::json profiles_json_locked() const;
    nlohmann::json summary_locked() const;
    nlohmann::json creation_settings_issues_locked() const;
    nlohmann::json validate_draft_locked() const;
    void emit(nlohmann::json event);
    // Persist a candidate before committing it to the live application model.
    void save_settings_locked(AppSettings const& candidate) const;

    Options options_;
    EventSink sink_;
    mutable std::mutex mutex_;
    Draft draft_;
    bool output_auto_ = true;
    std::vector<Draft> undo_;
    std::uint64_t next_source_ = 1;
    std::uint64_t sources_revision_ = 0;
    std::shared_ptr<ScanResult> scan_;
    // One scan worker; a newer request stops the running scan.
    bool scan_pending_ = false;
    std::stop_source scan_stop_;
    std::condition_variable_any scan_cv_;
    std::jthread scan_thread_;
    AppSettings settings_;
    std::optional<BatchPlan> batch_;
    std::uint64_t batch_revision_ = 0;
    nlohmann::json batch_overrides_ = nlohmann::json::object();
    std::map<std::string, std::vector<std::string>> batch_job_ids_;
    std::string last_started_batch_;
    std::uint64_t profiles_revision_ = 0;
    std::uint64_t next_batch_ = 1;
    std::map<std::string, OpenedTorrent> torrents_;
    std::uint64_t next_torrent_ = 1;
    nlohmann::json selected_torrent_ = nullptr;
    // One immutable candidate per opened torrent. A newer preview invalidates
    // its previous token, including any output selected for that token.
    std::map<std::string, PendingEdit> edits_;
    std::uint64_t next_edit_ = 1;
    std::vector<std::string> catalog_managed_urls_;
    std::unique_ptr<JobScheduler> jobs_;
    std::unique_ptr<DiagnosticsService> diagnostics_;
};

} // namespace tc::service
