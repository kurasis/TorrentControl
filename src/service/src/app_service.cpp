#include "tc/service/app_service.hpp"

#include "tc/core/error.hpp"
#include "tc/core/output.hpp"
#include "tc/core/verify.hpp"
#include "tc/service/sources.hpp"
#include "view_paging.hpp"

#include <algorithm>
#include <chrono>
#include <set>

namespace tc::service {

namespace fs = std::filesystem;
using nlohmann::json;
using core::CoreError;
using core::ErrorCode;

struct AppService::ScanResult {
    std::uint64_t sources_revision = 0;
    // scanning | ready | failed | empty
    std::string state = "empty";
    std::optional<core::Manifest> manifest;
    std::optional<JobError> error;
    // Cache data derived from the frozen manifest/draft, never filesystem checks.
    json summary_cache = nullptr;
    json engine_issues_cache = nullptr;
    std::optional<std::vector<core::ManifestIssue>> manifest_issues_cache;
    std::string filter_cache_key;
    std::vector<std::size_t> filter_cache_indices;
};

namespace {

constexpr std::size_t max_page = 1000;
constexpr std::size_t max_undo = 50;

json issue(std::string code, std::string severity, std::string message, std::string source_id = {})
{
    return json{{"code", std::move(code)}, {"severity", std::move(severity)}, {"message", std::move(message)},
        {"sourceId", std::move(source_id)}};
}

std::int64_t system_now()
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

bool supported_tracker_scheme(std::string_view url)
{
    return url.starts_with("udp://") || url.starts_with("http://") || url.starts_with("https://");
}

json entry_json(core::Manifest const& m, core::ManifestEntry const& e)
{
    return json{{"sourceId", e.source_id}, {"torrentPath", m.torrent_path_string(e)}, {"source", core::to_utf8(e.source_path)},
        {"length", std::to_string(e.length)}, {"reason", e.inclusion_reason}, {"hidden", e.flags.hidden},
        {"system", e.flags.system}, {"cloud", e.flags.cloud_placeholder}, {"requiresHydration", e.flags.requires_hydration}};
}

} // namespace

AppService::AppService(Options options, EventSink sink) : options_(std::move(options)), sink_(std::move(sink))
{
    catalog_managed_urls_ = builtin_tracker_catalog().urls;
    if (!options_.now) options_.now = system_now;
    if (!options_.settings_path.empty()) settings_ = load_settings(options_.settings_path);
    options_.jobs.max_concurrent = settings_.max_concurrent_jobs;
    jobs_ = std::make_unique<JobScheduler>(options_.jobs, [this](JobSnapshot const& s) {
        emit(json{{"type", "job"}, {"job", to_json(s, true)}});
    });
    diagnostics_ = std::make_unique<DiagnosticsService>([this](json const& event) { emit(event); });
    scan_ = std::make_shared<ScanResult>();

    // A new draft starts from the last used profile.
    if (auto p = find_profile_locked(settings_.last_profile)) draft_ = service::plan_profile(draft_, *p).result;
    else draft_ = service::plan_profile(draft_, builtin_profiles().front()).result;
    draft_.revision = 1;

    scan_thread_ = std::jthread([this](std::stop_token st) { scan_worker(st); });
}

AppService::~AppService()
{
    {
        std::lock_guard lock(mutex_);
        scan_stop_.request_stop();
    }
    scan_thread_.request_stop();
    scan_cv_.notify_all();
    scan_thread_ = {};
    jobs_.reset(); // cancels and joins jobs before the sink goes away
    diagnostics_.reset();
}

void AppService::emit(json event)
{
    if (sink_) sink_(event);
}

void AppService::check_revision(std::optional<std::uint64_t> revision) const
{
    if (revision && *revision != draft_.revision)
        throw ServiceError("STALE_REVISION", "The draft changed since this view was loaded; it has been refreshed", true);
}

void AppService::bump_locked(bool sources_changed)
{
    ++draft_.revision;
    scan_->summary_cache = nullptr;
    scan_->engine_issues_cache = nullptr;
    scan_->manifest_issues_cache.reset();
    scan_->filter_cache_key.clear();
    scan_->filter_cache_indices.clear();
    if (sources_changed) {
        ++sources_revision_;
        start_scan_locked();
    }
    refresh_output_locked();
}

void AppService::refresh_output_locked()
{
    if (!output_auto_) return;
    std::string const name = effective_name(draft_).empty() ? "Collection" : effective_name(draft_);
    draft_.output = draft_.sources.empty() ? fs::path{} : default_output(draft_.sources, name);
}

void AppService::start_scan_locked()
{
    scan_stop_.request_stop();
    auto next = std::make_shared<ScanResult>();
    next->sources_revision = sources_revision_;
    next->state = draft_.sources.empty() ? "empty" : "scanning";
    scan_ = next;
    scan_pending_ = !draft_.sources.empty();
    scan_cv_.notify_all();
}

void AppService::scan_worker(std::stop_token shutdown)
{
    std::unique_lock lock(mutex_);
    while (!shutdown.stop_requested()) {
        scan_cv_.wait(lock, shutdown, [&] { return scan_pending_; });
        if (shutdown.stop_requested()) return;
        scan_pending_ = false;
        std::vector<SourceSpec> const sources = draft_.sources;
        std::string const name = draft_.sources.size() > 1 ? (draft_.name.empty() ? "Collection" : draft_.name) : draft_.name;
        std::uint64_t const revision = sources_revision_;
        scan_stop_ = std::stop_source{};
        std::stop_token const token = scan_stop_.get_token();
        lock.unlock();
        emit(json{{"type", "scan"}, {"state", "scanning"}, {"sourcesRevision", std::to_string(revision)}});

        auto result = std::make_shared<ScanResult>();
        result->sources_revision = revision;
        try {
            result->manifest = build_manifest(sources, name, {}, token);
            result->state = "ready";
        } catch (CoreError const& e) {
            result->state = e.code() == ErrorCode::Cancelled ? "scanning" : "failed";
            result->error = JobError{std::string(core::to_string(e.code())), e.what(),
                std::string(core::to_string(e.phase())), e.source_id(), e.retryable(), e.os_error()};
        } catch (std::exception const& e) {
            result->state = "failed";
            result->error = JobError{"INTERNAL", e.what(), "Scanning", "", false, std::nullopt};
        }

        lock.lock();
        if (revision != sources_revision_ || token.stop_requested()) continue; // superseded
        scan_ = result;
        json event{{"type", "scan"}, {"state", result->state}, {"sourcesRevision", std::to_string(revision)},
            {"draftRevision", std::to_string(draft_.revision)}};
        if (result->error) event["error"] = {{"code", result->error->code}, {"message", result->error->message}};
        if (result->manifest) event["summary"] = summary_locked();
        lock.unlock();
        emit(event);
        lock.lock();
    }
}

void AppService::wait_for_scan()
{
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (scan_->state != "scanning" && !scan_pending_) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---- Draft -------------------------------------------------------------------

json AppService::draft_json_locked() const
{
    json j = to_json(draft_, false);
    j["profileMeta"] = nullptr;
    for (auto const& profile : builtin_profiles())
        if (profile.id == draft_.profile_id) j["profileMeta"] = view::profile(profile);
    for (auto const& profile : settings_.custom_profiles)
        if (profile.id == draft_.profile_id) { j["profileMeta"] = view::profile(profile); break; }
    j["pages"] = json::object();
    auto revision = std::to_string(draft_.revision);
    for (auto const* key : {"sources", "trackers", "webSeeds"}) {
        auto page = model_page_locked("draft", key, "", 0, 50, revision);
        j[key] = std::move(page["items"]);
        page.erase("items");
        j["pages"][key] = std::move(page);
    }
    j["textFields"] = json::object();
    for (auto const* key : {"name", "effectiveName", "comment", "creator", "source", "output"}) {
        auto const& text = j[key].get_ref<std::string const&>();
        if (text.size() > 512) {
            j["textFields"][key] = text.size();
            j[key] = view::preview(text);
        }
    }
    j["outputAuto"] = output_auto_;
    j["canUndo"] = !undo_.empty();
    j["scan"] = {{"state", scan_->state}, {"sourcesRevision", std::to_string(scan_->sources_revision)}};
    if (scan_->error) j["scan"]["error"] = {{"code", scan_->error->code}, {"message", scan_->error->message}};
    return j;
}

json AppService::draft_json() const
{
    std::lock_guard lock(mutex_);
    return draft_json_locked();
}

json AppService::new_draft()
{
    std::lock_guard lock(mutex_);
    Draft fresh;
    auto p = find_profile_locked(settings_.last_profile);
    fresh = service::plan_profile(fresh, p ? *p : builtin_profiles().front()).result;
    fresh.revision = draft_.revision + 1;
    undo_.push_back(draft_);
    if (undo_.size() > max_undo) undo_.erase(undo_.begin());
    draft_ = std::move(fresh);
    output_auto_ = true;
    batch_.reset();
    bump_locked(true);
    selected_torrent_ = nullptr;
    edits_.clear();
    return draft_json_locked();
}

json AppService::add_sources(std::vector<fs::path> const& paths)
{
    std::lock_guard lock(mutex_);
    std::set<std::string> existing;
    for (auto const& s : draft_.sources) existing.insert(core::fold_case(core::to_utf8(s.path)));
    std::size_t added = 0;
    for (auto const& raw : paths) {
        fs::path const p = fs::absolute(raw).lexically_normal();
        if (existing.contains(core::fold_case(core::to_utf8(p)))) continue;
        auto const info = core::native::stat_entry(p);
        if (!info) throw CoreError(ErrorCode::SourceMissing, "Not found: " + core::to_utf8(p));
        SourceSpec spec;
        spec.id = "src-" + std::to_string(next_source_++);
        spec.path = p;
        spec.is_directory = info->kind == core::native::EntryKind::Directory || info->kind == core::native::EntryKind::Junction;
        draft_.sources.push_back(std::move(spec));
        existing.insert(core::fold_case(core::to_utf8(p)));
        ++added;
    }
    if (added > 0) bump_locked(true);
    return draft_json_locked();
}

json AppService::remove_source(std::string const& id, std::optional<std::uint64_t> revision)
{
    std::lock_guard lock(mutex_);
    check_revision(revision);
    auto it = std::find_if(draft_.sources.begin(), draft_.sources.end(), [&](auto const& s) { return s.id == id; });
    if (it == draft_.sources.end()) throw ServiceError("NOT_FOUND", "No such source");
    draft_.sources.erase(it);
    bump_locked(true);
    return draft_json_locked();
}

json AppService::set_source_options(std::string const& id, json const& o, std::optional<std::uint64_t> revision)
{
    std::lock_guard lock(mutex_);
    check_revision(revision);
    auto it = std::find_if(draft_.sources.begin(), draft_.sources.end(), [&](auto const& s) { return s.id == id; });
    if (it == draft_.sources.end()) throw ServiceError("NOT_FOUND", "No such source");
    if (!o.is_object()) throw CoreError(ErrorCode::InvalidArgument, "options must be an object");
    SourceSpec next = *it;
    for (auto const& [key, v] : o.items()) {
        if (key == "recursive" && v.is_boolean()) next.recursive = v.get<bool>();
        else if (key == "followLinks" && v.is_boolean()) next.follow_links = v.get<bool>();
        else if (key == "skipCloud" && v.is_boolean()) next.skip_cloud = v.get<bool>();
        else if (key == "exclusions" && v.is_array() && v.size() <= 1000) {
            next.exclusions.clear();
            for (auto const& e : v) {
                if (!e.is_string() || e.get<std::string>().empty() || e.get<std::string>().size() > 1024)
                    throw CoreError(ErrorCode::InvalidArgument, "exclusions must be non-empty patterns");
                next.exclusions.push_back(e.get<std::string>());
            }
        } else {
            throw CoreError(ErrorCode::InvalidArgument, "invalid source option: " + key);
        }
    }
    *it = std::move(next);
    bump_locked(true);
    return draft_json_locked();
}

json AppService::update_draft(json const& patch, std::optional<std::uint64_t> revision)
{
    std::lock_guard lock(mutex_);
    check_revision(revision);
    std::string const old_name = draft_.name;
    apply_patch(draft_, patch);
    // A multi-source collection name is part of the scanned layout.
    bool const rescan = draft_.sources.size() > 1 && draft_.name != old_name;
    if (draft_.sources.size() == 1 && draft_.name != old_name && scan_->manifest) {
        scan_->manifest->name = draft_.name.empty() ? core::to_utf8(draft_.sources.front().path.filename()) : draft_.name;
    }
    bump_locked(rescan);
    return draft_json_locked();
}

json AppService::set_output(fs::path const& path)
{
    std::lock_guard lock(mutex_);
    draft_.output = fs::absolute(path).lexically_normal();
    output_auto_ = false;
    bump_locked(false);
    return draft_json_locked();
}

std::optional<Profile> AppService::find_profile_locked(std::string const& id) const
{
    for (auto const& p : builtin_profiles())
        if (p.id == id) return p;
    for (auto const& p : settings_.custom_profiles)
        if (p.id == id) return p;
    return std::nullopt;
}

json AppService::plan_profile(std::string const& id) const
{
    std::lock_guard lock(mutex_);
    auto p = find_profile_locked(id);
    if (!p) throw ServiceError("NOT_FOUND", "No such profile");
    ProfilePlan const plan = service::plan_profile(draft_, *p);
    json changes = json::array();
    for (auto const& c : plan.changes) {
        bool const large = c.before.dump().size() + c.after.dump().size() > 8192;
        changes.push_back({{"field", c.field}, {"before", large ? view::compact(c.before) : c.before},
            {"after", large ? view::compact(c.after) : c.after}, {"removesUserValue", c.removes_user_value},
            {"paged", large}, {"beforeTotal", c.before.is_array() ? c.before.size() : 0},
            {"afterTotal", c.after.is_array() ? c.after.size() : 0}});
    }
    return json{{"profile", view::profile(*p)}, {"changes", std::move(changes)}, {"draftRevision", std::to_string(draft_.revision)}};
}

json AppService::apply_profile(std::string const& id, std::optional<std::uint64_t> revision)
{
    std::lock_guard lock(mutex_);
    check_revision(revision);
    auto p = find_profile_locked(id);
    if (!p) throw ServiceError("NOT_FOUND", "No such profile");
    AppSettings next_settings = settings_;
    next_settings.last_profile = id;
    Draft next = service::plan_profile(draft_, *p).result;
    next.revision = draft_.revision;
    save_settings_locked(next_settings);
    undo_.push_back(draft_);
    if (undo_.size() > max_undo) undo_.erase(undo_.begin());
    draft_ = std::move(next);
    settings_ = std::move(next_settings);
    bump_locked(false);
    return draft_json_locked();
}

json AppService::undo()
{
    std::lock_guard lock(mutex_);
    if (undo_.empty()) throw ServiceError("NOTHING_TO_UNDO", "There is nothing to undo");
    Draft previous = std::move(undo_.back());
    undo_.pop_back();
    bool const sources_changed = previous.sources.size() != draft_.sources.size()
        || !std::equal(previous.sources.begin(), previous.sources.end(), draft_.sources.begin(),
            [](auto const& a, auto const& b) { return a.id == b.id && a.recursive == b.recursive && a.exclusions == b.exclusions; });
    previous.revision = draft_.revision;
    draft_ = std::move(previous);
    bump_locked(sources_changed);
    return draft_json_locked();
}

// ---- Scan and review ---------------------------------------------------------

json AppService::scan_state() const
{
    std::lock_guard lock(mutex_);
    json j{{"state", scan_->state}, {"sourcesRevision", std::to_string(scan_->sources_revision)}};
    if (scan_->error) j["error"] = {{"code", scan_->error->code}, {"message", scan_->error->message}};
    if (scan_->manifest) j["summary"] = summary_locked();
    return j;
}

json AppService::summary_locked() const
{
    json s = json::object();
    if (!scan_->manifest) return s;
    if (!scan_->summary_cache.is_null()) return scan_->summary_cache;
    core::Manifest const& m = *scan_->manifest;
    s["name"] = m.name;
    s["mode"] = m.mode == core::LayoutMode::SingleFile ? "single-file" : "directory";
    s["realFiles"] = m.entries.size();
    s["payloadBytes"] = std::to_string(m.total_length());
    s["skipped"] = m.skipped.size();
    s["unreadable"] = m.unreadable.size();
    std::size_t hydration = 0;
    for (auto const& e : m.entries) hydration += e.flags.requires_hydration ? 1 : 0;
    s["filesRequiringHydration"] = hydration;
    if (m.entries.empty()) return s;
    try {
        core::CreateOptions o = create_options(draft_, options_.now());
        o.accept_large_resource_use = true; // estimates only; acceptance is checked in validate
        o.allow_hydration = true;
        core::PreflightReport const r = core::preflight(m, o);
        core::PieceSizeDecision const& d = r.piece;
        std::size_t pad_files = 0;
        if (draft_.format == core::TorrentFormat::Hybrid)
            for (auto const& e : m.entries)
                if (e.length > 0 && e.length % static_cast<std::uint64_t>(d.piece_length) != 0) ++pad_files;
        s["pieceLength"] = d.piece_length;
        s["pieceLengthAutomatic"] = draft_.piece_length == 0;
        s["pieceCount"] = std::to_string(d.logical_pieces);
        s["paddingBytes"] = std::to_string(d.padding_bytes);
        s["paddingFiles"] = pad_files;
        s["logicalBytes"] = std::to_string(m.total_length() + d.padding_bytes);
        s["estimatedHashBytes"] = std::to_string(d.estimated_hash_bytes);
        s["paddingWarning"] = d.padding_warning;
        s["estimatedMetainfoBytes"] = std::to_string(r.estimate.metainfo_bytes);
        s["estimatedMemoryBytes"] = std::to_string(r.estimate.memory_bytes);
    } catch (CoreError const& e) {
        s["layoutError"] = {{"code", std::string(core::to_string(e.code()))}, {"message", e.what()}};
    }
    scan_->summary_cache = s;
    return s;
}

json AppService::creation_settings_issues_locked() const
{
    json issues = json::array();
    // Private torrents (section 10.4).
    auto const tiers = tracker_tiers(draft_.trackers);
    std::set<std::string> const preset(builtin_tracker_catalog().urls.begin(), builtin_tracker_catalog().urls.end());
    if (draft_.private_flag) {
        if (tiers.empty())
            issues.push_back(issue("PRIVATE_WITHOUT_TRACKER", "error", "A private torrent needs at least one tracker you are authorized to use"));
        for (auto const& t : draft_.trackers)
            if (t.enabled && preset.contains(t.url))
                issues.push_back(issue("PRIVATE_PUBLIC_TRACKER", "error",
                    "Public preset tracker in a private torrent: " + t.url + ". Remove it or turn off Private."));
        if (!draft_.dht_nodes.empty())
            issues.push_back(issue("PRIVATE_DHT_NODES", "warning", "DHT nodes are left out of private torrents"));
        auto const p = find_profile_locked(draft_.profile_id);
        if (!draft_.web_seeds.empty() && p && !p->allow_web_seeds)
            issues.push_back(issue("PRIVATE_WEB_SEEDS", "error",
                "The selected profile does not allow web seeds; remove them or use a profile that allows them"));
    }
    std::set<std::string> seen;
    for (auto const& t : draft_.trackers) {
        if (!t.enabled || t.url.empty()) continue;
        if (!supported_tracker_scheme(t.url))
            issues.push_back(issue("TRACKER_UNSUPPORTED_SCHEME", "warning",
                "Kept as written but not checkable: " + redact_url(t.url)));
        if (!seen.insert(t.url).second)
            issues.push_back(issue("TRACKER_DUPLICATE", "warning", "Listed more than once: " + redact_url(t.url)));
    }

    return issues;
}

json AppService::validate_draft() const
{
    std::lock_guard lock(mutex_);
    auto value = validate_draft_locked();
    for (auto const* key : {"issues", "review"}) {
        auto const& rows = value[key];
        auto page = view::page(rows.size(), 0, 50, std::to_string(draft_.revision),
            [&](auto i) { return view::compact(rows[i]); });
        value[std::string(key) + "Total"] = rows.size();
        value[key] = std::move(page["items"]);
    }
    return value;
}

json AppService::validate_draft_locked() const
{
    json issues = creation_settings_issues_locked();

    if (draft_.sources.empty()) issues.push_back(issue("NO_SOURCES", "error", "Add files or folders to share"));
    if (scan_->state == "scanning") issues.push_back(issue("SCAN_PENDING", "error", "The sources are still being scanned"));
    if (scan_->state == "failed" && scan_->error)
        issues.push_back(issue(scan_->error->code, "error", scan_->error->message, scan_->error->source_id));

    core::Manifest const* m = scan_->state == "ready" && scan_->manifest ? &*scan_->manifest : nullptr;
    if (m != nullptr) {
        for (auto const& u : m->unreadable)
            issues.push_back(issue("SOURCE_UNREADABLE", "error",
                core::to_utf8(u.path) + ": " + u.message + " Exclude it or fix access to continue."));
        if (!scan_->manifest_issues_cache) scan_->manifest_issues_cache = core::validate_manifest(*m);
        for (auto const& i : *scan_->manifest_issues_cache)
            issues.push_back(issue(std::string(core::to_string(i.code)), i.severity == core::Severity::Error ? "error" : "warning",
                i.message, i.source_id));
        if (!m->entries.empty()) {
            if (scan_->engine_issues_cache.is_null()) {
                json engine_issues = json::array();
                core::CreateOptions const o = create_options(draft_, options_.now());
                try {
                    core::PreflightReport const r = core::preflight(*m, o);
                    for (auto const& w : r.warnings)
                        engine_issues.push_back(issue(std::string(core::to_string(w.code)), "warning", w.message, w.source_id));
                } catch (CoreError const& e) {
                    // Manifest errors are already listed above.
                    if (e.code() != ErrorCode::PathCollision && e.code() != ErrorCode::InvalidPath && e.code() != ErrorCode::EmptyPayload)
                        engine_issues.push_back(issue(std::string(core::to_string(e.code())), "error", e.what(), e.source_id()));
                }
                scan_->engine_issues_cache = std::move(engine_issues);
            }
            for (auto const& entry : scan_->engine_issues_cache) issues.push_back(entry);
        }
    }

    // Output (section 14.1).
    if (draft_.output.empty()) {
        issues.push_back(issue("NO_OUTPUT", "error", "Choose where to save the torrent"));
    } else {
        std::error_code ec;
        if (!fs::is_directory(draft_.output.parent_path(), ec))
            issues.push_back(issue("OUTPUT_FOLDER_MISSING", "error", "The destination folder does not exist"));
        else if (fs::exists(draft_.output, ec) && !draft_.replace_existing)
            issues.push_back(issue("OUTPUT_EXISTS", "error", "A file with this name exists; choose Replace or another name"));
        if (m != nullptr) {
            try {
                core::check_output_target(draft_.output, *m);
            } catch (CoreError const& e) {
                issues.push_back(issue("OUTPUT_CONFLICT", "error", e.what(), e.source_id()));
            }
        }
    }

    bool const can_create = std::none_of(issues.begin(), issues.end(), [](json const& i) { return i["severity"] == "error"; });

    // Active advanced settings for the review summary (section 4.1).
    json review = json::array();
    auto add = [&](std::string field, json value) { review.push_back({{"field", std::move(field)}, {"value", std::move(value)}}); };
    add("profile", draft_.profile_id);
    add("format", std::string(core::to_string(draft_.format)));
    if (draft_.piece_length != 0) add("pieceLength", draft_.piece_length);
    if (draft_.private_flag) add("private", true);
    add("trackerTiers", tracker_tiers(draft_.trackers).size());
    if (!draft_.web_seeds.empty()) add("webSeeds", draft_.web_seeds.size());
    if (!draft_.dht_nodes.empty() && !draft_.private_flag) add("dhtNodes", draft_.dht_nodes.size());
    if (!draft_.comment.empty()) add("comment", draft_.comment);
    if (!draft_.creator.empty()) add("creator", draft_.creator);
    if (!draft_.source_tag.empty()) add("source", draft_.source_tag);
    if (draft_.date_policy != CreationDatePolicy::Now) add("creationDate", std::string(to_string(draft_.date_policy)));
    if (draft_.replace_existing) add("replaceExisting", true);
    if (draft_.allow_hydration) add("allowHydration", true);
    for (auto const& s : draft_.sources) {
        if (s.is_directory && !s.recursive) add("nonRecursive", core::to_utf8(s.path.filename()));
        if (!s.exclusions.empty()) add("exclusions", s.exclusions.size());
        if (s.follow_links) add("followLinks", core::to_utf8(s.path.filename()));
    }

    return json{{"draftRevision", std::to_string(draft_.revision)}, {"issues", std::move(issues)}, {"canCreate", can_create},
        {"summary", summary_locked()}, {"review", std::move(review)}};
}

std::string AppService::start_create()
{
    CreateJobSpec spec;
    {
        std::lock_guard lock(mutex_);
        json const v = validate_draft_locked();
        for (auto const& i : v["issues"])
            if (i["severity"] == "error") throw ServiceError("VALIDATION_FAILED", i["message"].get<std::string>());
        if (!scan_->manifest) throw ServiceError("VALIDATION_FAILED", "The sources are not scanned yet");
        spec.name = scan_->manifest->name;
        spec.manifest = *scan_->manifest;
        spec.options = create_options(draft_, options_.now());
        spec.source_tag = draft_.source_tag;
        spec.output = draft_.output;
        spec.replace_existing = draft_.replace_existing;
    }
    return jobs_->enqueue_create(std::move(spec));
}

json AppService::manifest_page(std::size_t offset, std::size_t limit, std::string const& filter) const
{
    std::lock_guard lock(mutex_);
    limit = std::min(limit, max_page);
    json entries = json::array();
    std::size_t total = 0;
    if (scan_->manifest) {
        core::Manifest const& m = *scan_->manifest;
        std::string const needle = core::fold_case(filter);
        if (needle.empty()) {
            total = m.entries.size();
            for (std::size_t i = offset; i < total && entries.size() < limit; ++i)
                entries.push_back(entry_json(m,m.entries[i]));
        } else {
            if (scan_->filter_cache_key != needle) {
                scan_->filter_cache_indices.clear();
                for (std::size_t i = 0; i < m.entries.size(); ++i)
                    if (core::fold_case(m.torrent_path_string(m.entries[i])).find(needle) != std::string::npos)
                        scan_->filter_cache_indices.push_back(i);
                scan_->filter_cache_key = needle;
            }
            total = scan_->filter_cache_indices.size();
            for (std::size_t i = offset; i < total && entries.size() < limit; ++i)
                entries.push_back(entry_json(m,m.entries[scan_->filter_cache_indices[i]]));
        }
    }
    return json{{"offset", offset}, {"total", total}, {"entries", std::move(entries)},
        {"sourcesRevision", std::to_string(scan_->sources_revision)}};
}

json AppService::skipped_page(std::size_t offset, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    limit = std::min(limit, max_page);
    json items = json::array();
    std::size_t total = 0;
    if (scan_->manifest) {
        for (auto const& u : scan_->manifest->unreadable) {
            if (total >= offset && items.size() < limit)
                items.push_back({{"path", core::to_utf8(u.path)}, {"kind", "unreadable"}, {"reason", u.message}, {"blocking", true}});
            ++total;
        }
        for (auto const& s : scan_->manifest->skipped) {
            if (total >= offset && items.size() < limit)
                items.push_back({{"path", core::to_utf8(s.path)}, {"kind", std::string(core::to_string(s.kind))},
                    {"reason", s.reason}, {"blocking", false}});
            ++total;
        }
    }
    return json{{"offset", offset}, {"total", total}, {"items", std::move(items)}};
}

// ---- Batch -------------------------------------------------------------------

json AppService::plan_batch(std::string const& mode, std::string const& policy, fs::path const& output_dir)
{
    auto const m = batch_mode_from(mode);
    auto const p = conflict_policy_from(policy);
    if (!m || !p) throw CoreError(ErrorCode::InvalidArgument, "invalid batch mode or conflict policy");
    std::lock_guard lock(mutex_);
    fs::path dir = output_dir;
    if (dir.empty() && !draft_.sources.empty()) dir = fs::absolute(draft_.sources.front().path).lexically_normal().parent_path();
    batch_ = service::plan_batch(draft_, *m, dir, *p);
    batch_overrides_ = json::object();
    ++batch_revision_;
    return batch_json_locked();
}

json AppService::update_batch(json const& overrides, std::string const& revision)
{
    std::lock_guard lock(mutex_);
    if (!batch_) throw ServiceError("NOT_FOUND", "No batch is planned");
    if (!revision.empty() && revision != std::to_string(batch_revision_)) throw ServiceError("STALE_REVISION", "The batch plan changed", true);
    if (!overrides.is_object()) throw CoreError(ErrorCode::InvalidArgument, "overrides must be an object");
    json next_overrides = batch_overrides_;
    for (auto const& [id, change] : overrides.items()) {
        auto found = std::find_if(batch_->items.begin(), batch_->items.end(), [&](auto const& row) { return row.id == id; });
        if (found == batch_->items.end() || !change.is_object()) throw ServiceError("INVALID_ARGUMENT", "Unknown batch item");
        for (auto const& [key, value] : change.items()) {
            if (key == "policy") {
                if (!value.is_string() || !conflict_policy_from(value.get<std::string>())) throw ServiceError("INVALID_ARGUMENT", "Invalid policy");
            } else if (key == "included") {
                if (!value.is_boolean()) throw ServiceError("INVALID_ARGUMENT", "Included must be boolean");
            } else throw ServiceError("INVALID_ARGUMENT", "Unknown batch override");
            next_overrides[id][key] = value;
        }
    }
    for (auto& item : batch_->items) {
        auto it = next_overrides.find(item.id);
        if (it == next_overrides.end()) continue;
        if (auto p = it->find("policy"); p != it->end()) {
            auto policy = p->is_string() ? conflict_policy_from(p->get<std::string>()) : std::nullopt;
            if (!policy) throw CoreError(ErrorCode::InvalidArgument, "invalid conflict policy");
            item.policy = *policy;
        }
    }
    resolve_conflicts(*batch_);
    for (auto& item : batch_->items) {
        auto it = next_overrides.find(item.id);
        if (it != next_overrides.end())
            if (auto inc = it->find("included"); inc != it->end() && inc->is_boolean() && !inc->get<bool>()) item.included = false;
    }
    batch_overrides_ = std::move(next_overrides);
    ++batch_revision_;
    return batch_json_locked();
}

std::vector<std::string> AppService::start_batch(std::string const& revision)
{
    std::vector<CreateJobSpec> specs;
    std::string batch_id;
    {
        std::lock_guard lock(mutex_);
        if (!batch_) throw ServiceError("NOT_FOUND", "No batch is planned");
        if (!revision.empty() && revision != std::to_string(batch_revision_)) throw ServiceError("STALE_REVISION", "The batch plan changed", true);
        // Check the same settings as single creation under the snapshot lock.
        // Manifest/output validation remains per item in the worker.
        for (auto const& i : creation_settings_issues_locked())
            if (i["severity"] == "error") throw ServiceError("VALIDATION_FAILED", i["message"].get<std::string>());
        batch_id = "batch-" + std::to_string(next_batch_++);
        // The profile and settings are snapshotted now (section 4.3).
        core::CreateOptions const options = create_options(draft_, options_.now());
        for (auto const& item : batch_->items) {
            if (!item.included) continue;
            CreateJobSpec spec;
            spec.name = item.name;
            spec.scan = ScanRequest{item.sources, batch_->mode == BatchMode::Single ? item.name : std::string{}};
            spec.options = options;
            spec.source_tag = draft_.source_tag;
            spec.output = item.output;
            spec.replace_existing = item.policy == ConflictPolicy::Replace;
            spec.batch_id = batch_id;
            specs.push_back(std::move(spec));
        }
        batch_.reset();
    }
    if (specs.empty()) throw ServiceError("VALIDATION_FAILED", "Every batch item is skipped");
    std::vector<std::string> ids;
    for (auto& s : specs) ids.push_back(jobs_->enqueue_create(std::move(s)));
    {
        std::lock_guard lock(mutex_);
        batch_job_ids_[batch_id] = ids;
        last_started_batch_ = batch_id;
    }
    return ids;
}

// ---- Existing torrents -------------------------------------------------------

json AppService::open_torrent(fs::path const& path)
{
    auto meta = std::make_shared<core::Metainfo const>(core::Metainfo::parse(read_small_file(path, 64u * 1024 * 1024)));
    std::vector<std::string> const problems = core::validate_metainfo(*meta);
    std::vector<core::MetainfoFile> files;
    try {
        files = core::metainfo_files(*meta);
    } catch (CoreError const&) {
        // Reported through `problems`.
    }
    std::uint64_t total = 0;
    std::size_t real = 0;
    std::size_t pads = 0;
    for (auto const& f : files) {
        if (f.pad) ++pads;
        else {
            ++real;
            total += f.length;
        }
    }
    std::string id;
    {
        std::lock_guard lock(mutex_);
        id = "t-" + std::to_string(next_torrent_++);
        torrents_[id] = OpenedTorrent{fs::absolute(path).lexically_normal(), meta, nullptr};
    }

    auto decoded = [](std::string const& bytes) -> json {
        json value(bytes);
        try { (void)value.dump(); }
        catch (json::type_error const&) { return nullptr; }
        return value;
    };
    auto text = [&](core::bencode::Value const& dict, char const* key) -> json {
        auto const* v = dict.find(key);
        return v != nullptr && v->type() == core::bencode::Type::String ? decoded(v->text()) : json(nullptr);
    };
    json trackers = json::array();
    if (auto const* al = meta->root().find("announce-list"); al != nullptr && al->type() == core::bencode::Type::List) {
        int tier = 0;
        for (auto const& t : al->items()) {
            if (t.type() == core::bencode::Type::List)
                for (auto const& u : t.items())
                    if (u.type() == core::bencode::Type::String && !decoded(u.text()).is_null()) trackers.push_back({{"url", u.text()}, {"tier", tier}});
            ++tier;
        }
    } else if (auto const* a = meta->root().find("announce"); a != nullptr && a->type() == core::bencode::Type::String) {
        if (!decoded(a->text()).is_null()) trackers.push_back({{"url", a->text()}, {"tier", 0}});
    }
    json web_seeds = json::array();
    if (auto const* ul = meta->root().find("url-list"); ul != nullptr) {
        if (ul->type() == core::bencode::Type::String) { if (!decoded(ul->text()).is_null()) web_seeds.push_back(ul->text()); }
        else if (ul->type() == core::bencode::Type::List)
            for (auto const& u : ul->items())
                if (u.type() == core::bencode::Type::String && !decoded(u.text()).is_null()) web_seeds.push_back(u.text());
    }
    auto const* priv = meta->info().find("private");
    auto const* piece = meta->info().find("piece length");
    json j{{"id", id}, {"path", core::to_utf8(path)}, {"name", decoded(meta->name())}, {"format", std::string(core::to_string(meta->format()))},
        {"private", priv != nullptr && priv->as_int64() == 1}, {"pieceLength", piece ? piece->text() : ""},
        {"realFiles", real}, {"paddingFiles", pads}, {"payloadBytes", std::to_string(total)},
        {"metainfoBytes", std::to_string(meta->bytes().size())}, {"problems", problems},
        {"legacyRootHash", meta->has_legacy_root_hash()}, {"comment", text(meta->root(), "comment")},
        {"createdBy", text(meta->root(), "created by")}, {"source", text(meta->info(), "source")},
        {"trackers", std::move(trackers)}, {"webSeeds", std::move(web_seeds)}, {"magnet", core::make_magnet(*meta)}};
    if (meta->info_hashes().v1) j["infohashV1"] = core::to_hex(*meta->info_hashes().v1);
    if (meta->info_hashes().v2) j["infohashV2"] = core::to_hex(*meta->info_hashes().v2);
    {
        std::lock_guard lock(mutex_);
        torrents_.at(id).overview = std::move(j);
        auto const& full = torrents_.at(id).overview;
        j = json::object();
        j["textFields"] = json::object();
        for (auto const& [key, value] : full.items()) {
            if (value.is_array()) {
                auto page = model_page_locked("torrent", key, id, 0, 50, "");
                j[key] = std::move(page["items"]);
                j[key + "Total"] = value.size();
            } else if (value.is_string() && value.get_ref<std::string const&>().size() > 512) {
                j[key] = view::compact(value);
                j["textFields"][key] = value.get_ref<std::string const&>().size();
            } else j[key] = value;
        }
        selected_torrent_ = j;
    }
    return j;
}

json AppService::torrent_files_page(std::string const& id, std::size_t offset, std::size_t limit) const
{
    std::shared_ptr<core::Metainfo const> meta;
    {
        std::lock_guard lock(mutex_);
        auto it = torrents_.find(id);
        if (it == torrents_.end()) throw ServiceError("NOT_FOUND", "No such torrent");
        meta = it->second.meta;
    }
    limit = std::min(limit, max_page);
    json files = json::array();
    std::size_t total = 0;
    for (auto const& f : core::metainfo_files(*meta)) {
        if (total >= offset && files.size() < limit)
            files.push_back({{"path", f.torrent_path}, {"length", std::to_string(f.length)}, {"pad", f.pad}});
        ++total;
    }
    return json{{"offset", offset}, {"total", total}, {"files", std::move(files)}};
}

std::string AppService::verify_torrent(std::string const& id, fs::path const& payload_root)
{
    std::shared_ptr<core::Metainfo const> meta;
    {
        std::lock_guard lock(mutex_);
        auto it = torrents_.find(id);
        if (it == torrents_.end()) throw ServiceError("NOT_FOUND", "No such torrent");
        meta = it->second.meta;
    }
    VerifyJobSpec spec;
    spec.name = meta->name();
    spec.torrent_bytes = meta->bytes();
    spec.mapping = core::map_to_root(*meta, payload_root);
    return jobs_->enqueue_verify(std::move(spec));
}

std::string AppService::magnet_for(std::string const& id) const
{
    {
        std::lock_guard lock(mutex_);
        if (auto it = torrents_.find(id); it != torrents_.end()) return core::make_magnet(*it->second.meta);
    }
    auto job = jobs_->find(id);
    if (!job || !job->result) throw ServiceError("NOT_FOUND", "No finished torrent with this ID");
    return job->result->magnet;
}

fs::path AppService::torrent_path(std::string const& id) const
{
    {
        std::lock_guard lock(mutex_);
        if (auto it = torrents_.find(id); it != torrents_.end()) return it->second.path;
    }
    auto job = jobs_->find(id);
    if (!job || !job->result) throw ServiceError("NOT_FOUND", "No finished torrent with this ID");
    return job->result->output;
}

// ---- Projects and settings ---------------------------------------------------

void AppService::save_project(fs::path const& path)
{
    std::lock_guard lock(mutex_);
    int resolved = draft_.piece_length;
    if (resolved == 0 && scan_->manifest && !scan_->manifest->entries.empty()) {
        try {
            resolved = core::choose_piece_size(*scan_->manifest, draft_.format).piece_length;
        } catch (CoreError const&) {
            resolved = 0;
        }
    }
    service::save_project(path, draft_, resolved);
}

json AppService::load_project(fs::path const& path)
{
    Draft loaded = service::load_project(path);
    std::lock_guard lock(mutex_);
    undo_.push_back(draft_);
    if (undo_.size() > max_undo) undo_.erase(undo_.begin());
    loaded.revision = draft_.revision;
    // Source IDs stay native-owned: renumber them for this session.
    for (auto& s : loaded.sources) s.id = "src-" + std::to_string(next_source_++);
    output_auto_ = loaded.output.empty();
    draft_ = std::move(loaded);
    bump_locked(true);
    return draft_json_locked();
}

void AppService::save_settings_locked(AppSettings const& candidate) const
{
    if (options_.settings_path.empty()) return;
    try {
        save_settings(options_.settings_path, candidate);
    } catch (CoreError const& error) {
        if (error.code() == ErrorCode::ResourceLimit) throw ServiceError("RESOURCE_LIMIT", "Settings exceed the 16 MiB file limit. Remove unused profiles or reduce their size. Your changes were not applied.");
        throw ServiceError("SETTINGS_WRITE_FAILED",
            "Could not save settings. Check free space and write access, then try again. Your changes were not applied.", true);
    }
}

json AppService::settings_json() const
{
    std::lock_guard lock(mutex_);
    json j = to_json(settings_, false);
    j["persistence"] = options_.settings_path.empty() ? "memory" : "disk";
    return j;
}

json AppService::update_settings(json const& patch)
{
    std::lock_guard lock(mutex_);
    AppSettings next = settings_;
    apply_settings_patch(next, patch);
    save_settings_locked(next);
    settings_ = std::move(next);
    jobs_->set_max_concurrent(settings_.max_concurrent_jobs);
    json j = to_json(settings_, false);
    j["persistence"] = options_.settings_path.empty() ? "memory" : "disk";
    return j;
}

json AppService::profiles_json() const
{
    std::lock_guard lock(mutex_);
    return profiles_json_locked();
}

json AppService::profiles_state() const
{
    std::lock_guard lock(mutex_);
    return {{"profiles", profiles_json_locked()}, {"profilesTotal", settings_.custom_profiles.size() + builtin_profiles().size()},
        {"profilesRevision", std::to_string(profiles_revision_)}};
}

json AppService::profiles_json_locked() const
{
    auto page = model_page_locked("profiles", "items", "", 0, 50, std::to_string(profiles_revision_));
    json list = std::move(page["items"]);
    // Keep the selected profile available even when it lies beyond the first page.
    if (std::none_of(list.begin(), list.end(), [&](auto const& p) { return p["id"] == draft_.profile_id; }))
        if (auto p = find_profile_locked(draft_.profile_id)) list.push_back(view::profile(*p));
    return list;
}

json AppService::save_custom_profile(std::string const& name)
{
    std::lock_guard lock(mutex_);
    if (name.empty() || name.size() > 200) throw CoreError(ErrorCode::InvalidArgument, "Give the profile a name");
    Profile p;
    p.id = "custom-" + std::to_string(settings_.custom_profiles.size() + 1);
    while (find_profile_locked(p.id)) p.id += "x";
    p.name = name;
    p.format = draft_.format;
    p.private_flag = draft_.private_flag;
    p.trackers = draft_.trackers;
    p.allow_web_seeds = true;
    p.source_tag = draft_.source_tag;
    AppSettings next = settings_;
    next.custom_profiles.push_back(p);
    next.last_profile = p.id;
    save_settings_locked(next);
    settings_ = std::move(next);
    draft_.profile_id = p.id;
    ++profiles_revision_;
    bump_locked(false);
    return view::profile(p);
}

json AppService::delete_custom_profile(std::string const& id)
{
    std::lock_guard lock(mutex_);
    AppSettings next = settings_;
    auto& list = next.custom_profiles;
    auto it = std::find_if(list.begin(), list.end(), [&](auto const& p) { return p.id == id; });
    if (it == list.end()) throw ServiceError("NOT_FOUND", "No such custom profile");
    list.erase(it);
    save_settings_locked(next);
    settings_ = std::move(next);
    ++profiles_revision_;
    return json{{"deleted", id}};
}

json AppService::export_profile(std::string const& id, bool include_secrets) const
{
    std::lock_guard lock(mutex_);
    auto p = find_profile_locked(id);
    if (!p) throw ServiceError("NOT_FOUND", "No such profile");
    return service::export_profile(*p, include_secrets);
}

json AppService::snapshot() const
{
    auto jobs = jobs_->bridge_page(0, 50);
    json j{{"draft", draft_json()}, {"scan", scan_state()}, {"jobs", std::move(jobs["jobs"])}, {"settings", settings_json()},
        {"profiles", profiles_json()}, {"diagnostics", diagnostics_->snapshot()}};
    j["jobsTotal"] = jobs["total"]; j["nextJobsOffset"] = jobs["nextOffset"]; j["jobsRevision"] = jobs["collectionRevision"];
    std::lock_guard lock(mutex_);
    j["profilesTotal"] = settings_.custom_profiles.size() + builtin_profiles().size();
    j["profilesRevision"] = std::to_string(profiles_revision_);
    if (batch_) j["batch"] = batch_json_locked();
    j["torrent"] = selected_torrent_;
    j["editorPreview"] = nullptr;
    if (!selected_torrent_.is_null()) {
        for (auto const& [token, edit] : edits_) {
            (void)token;
            if (edit.torrent_id == selected_torrent_["id"].get<std::string>()) j["editorPreview"] = edit.summary;
        }
    }
    return j;
}

std::string AppService::suggested_name() const
{
    std::lock_guard lock(mutex_);
    std::string const n = effective_name(draft_);
    return safe_file_name(n.empty() ? "Collection" : n);
}

fs::path AppService::suggested_folder() const
{
    std::lock_guard lock(mutex_);
    if (!draft_.output.empty()) return draft_.output.parent_path();
    if (!draft_.sources.empty()) return fs::absolute(draft_.sources.front().path).lexically_normal().parent_path();
    return {};
}

// ---- Explicit network diagnostics --------------------------------------------
std::vector<ProbeTarget> AppService::collect_diagnostic_targets_locked(std::string const& kind, std::string const& torrent_id) const
{
    if (!torrent_id.empty()) {
        auto it = torrents_.find(torrent_id);
        if (it == torrents_.end()) throw ServiceError("NOT_FOUND", "No such torrent");
        return torrent_probe_targets(*it->second.meta, kind);
    }
    std::vector<ProbeTarget> targets;
    if (kind == "trackers") {
        for (auto const& row : draft_.trackers) if (row.enabled && !row.url.empty())
            targets.push_back({"endpoint-" + std::to_string(targets.size() + 1), row.url, "tracker", {}, 0});
    } else if (kind == "web-seeds") {
        if (scan_->state != "ready" || !scan_->manifest) throw ServiceError("SCAN_REQUIRED", "Scan the payload before checking web seed paths");
        auto const& manifest = *scan_->manifest;
        std::vector<core::MetainfoFile> samples;
        if (!manifest.entries.empty()) {
            auto sample = [&](core::ManifestEntry const& e) {
                if (std::any_of(samples.begin(), samples.end(), [&](auto const& f) { return f.path == e.torrent_path; })) return;
                core::MetainfoFile f;
                f.path = e.torrent_path; f.torrent_path = manifest.torrent_path_string(e); f.length = e.length;
                samples.push_back(std::move(f));
            };
            sample(manifest.entries.front());
            auto nested = std::find_if(manifest.entries.begin(), manifest.entries.end(), [](auto const& e) { return e.torrent_path.size() > 1; });
            if (nested != manifest.entries.end()) sample(*nested);
            sample(manifest.entries.back());
        }
        for (auto const& url : draft_.web_seeds) for (auto const& file : samples) {
            std::string resolved;
            std::string probe_kind = "bep19";
            try { resolved = resolve_web_seed(url, manifest.name, file); }
            catch (ServiceError const&) { resolved = url; probe_kind = "bep19-invalid-base"; }
            targets.push_back({"endpoint-" + std::to_string(targets.size() + 1), resolved, probe_kind, file.torrent_path, file.length, manifest.entries.size()});
        }
    } else throw ServiceError("INVALID_ARGUMENT", "Diagnostic kind must be trackers or web-seeds");
    if (targets.size() > 256) throw ServiceError("RESOURCE_LIMIT", "At most 256 diagnostic targets per run");
    return targets;
}
json AppService::diagnostic_targets(std::string const& kind, std::string const& torrent_id) const
{
    std::lock_guard lock(mutex_);
    json rows = json::array();
    for (auto const& target : collect_diagnostic_targets_locked(kind, torrent_id))
        rows.push_back({{"id",target.id},{"url",redact_url(target.url)},{"file",target.torrent_path},{"kind",target.kind},{"totalFiles",target.total_files}});
    return {{"targets",std::move(rows)},{"kind",kind},{"torrentId",torrent_id}};
}
std::string AppService::start_diagnostics(std::string const& kind, std::string const& torrent_id, NetworkPolicy policy)
{
    std::vector<ProbeTarget> targets;
    { std::lock_guard lock(mutex_); targets = collect_diagnostic_targets_locked(kind, torrent_id); }
    return diagnostics_->start(std::move(targets), std::move(policy));
}
std::string AppService::update_tracker_catalog(NetworkPolicy policy)
{
    policy.refresh = true;
    return diagnostics_->start({{"catalog",builtin_tracker_catalog().source_url,"catalog",{},0}}, std::move(policy));
}
json AppService::plan_catalog_apply() const
{
    auto catalog = diagnostics_->catalog();
    std::lock_guard lock(mutex_);
    auto next = draft_.trackers;
    auto const& builtin = catalog_managed_urls_;
    std::set<std::string> incoming;
    for (auto const& url : catalog["urls"]) incoming.insert(url.get<std::string>());
    json added = json::array(), removed = json::array();
    if (!draft_.private_flag) {
        next.erase(std::remove_if(next.begin(), next.end(), [&](auto const& row) {
            bool remove = row.enabled && std::find(builtin.begin(), builtin.end(), row.url) != builtin.end() && !incoming.contains(row.url);
            if (remove) removed.push_back(redact_url(row.url));
            return remove;
        }), next.end());
        for (auto const& url : catalog["urls"]) {
            auto value = url.get<std::string>();
            if (std::none_of(next.begin(), next.end(), [&](auto const& row) { return row.url == value; })) added.push_back(redact_url(value));
        }
    }
    return {{"checksum",catalog["checksum"]},{"source",catalog["source"]},{"fetchedAt",catalog["fetchedAt"]},
        {"added",std::move(added)},{"removed",std::move(removed)},{"privateBlocked",draft_.private_flag},{"draftRevision",std::to_string(draft_.revision)}};
}
json AppService::apply_catalog(std::string const& checksum, std::optional<std::uint64_t> revision)
{
    auto catalog = diagnostics_->catalog();
    std::lock_guard lock(mutex_);
    check_revision(revision);
    if (!catalog["checksum"].is_string() || catalog["checksum"].get<std::string>() != checksum)
        throw ServiceError("STALE_CATALOG", "Review the latest fetched catalog before applying it");
    if (draft_.private_flag) throw ServiceError("PRIVATE_CATALOG", "Public catalog endpoints cannot be added to a private draft");
    undo_.push_back(draft_);
    if (undo_.size() > max_undo) undo_.erase(undo_.begin());
    std::set<std::string> incoming;
    for (auto const& url : catalog["urls"]) incoming.insert(url.get<std::string>());
    auto const& builtin = catalog_managed_urls_;
    draft_.trackers.erase(std::remove_if(draft_.trackers.begin(), draft_.trackers.end(), [&](auto const& row) {
        return row.enabled && std::find(builtin.begin(), builtin.end(), row.url) != builtin.end() && !incoming.contains(row.url);
    }), draft_.trackers.end());
    int tier = 0;
    for (auto const& row : draft_.trackers) tier = std::max(tier, row.tier + 1);
    for (auto const& url : catalog["urls"]) {
        auto value = url.get<std::string>();
        if (std::none_of(draft_.trackers.begin(), draft_.trackers.end(), [&](auto const& row) { return row.url == value; })) {
            if (std::find(catalog_managed_urls_.begin(), catalog_managed_urls_.end(), value) == catalog_managed_urls_.end())
                catalog_managed_urls_.push_back(value);
            draft_.trackers.push_back({std::move(value), tier++, true});
        }
    }
    bump_locked(false);
    return draft_json_locked();
}

} // namespace tc::service
