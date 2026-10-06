// Application controller: holds the page state, talks to the native side and
// re-renders views. The native job and draft state is authoritative; the page
// only mirrors it and can always be rebuilt from getSnapshot (U03).
import { applyTranslations, setLocale, t } from "./i18n.js";
import { isAvailable, onEvent, request, requestWithFiles } from "./bridge.js";
import { debounce, toast } from "./dom.js";
import { renderWorkspace, renderReview, renderJobs, workspaceKey } from "./views.js";
import { showResult, showProfileChange, showBatch, showConfirm, showSaveProfile, showMagnetCopy, showVerifyFile, showJobText, showJobLayoutRow, showCollection, showModelText, closeDialog } from "./dialogs.js";
import { loadMetadata } from "./metadata-editor.js";

export const state = {
  draft: null,
  scan: { state: "empty" },
  validation: null,
  jobs: new Map(),
  jobHistory: { ids: [], offset: 0, offsetIntent: null, total: 0, nextOffset: null, followNewest: true, previous: [], serial: 0 },
  settings: { mode: "simple", theme: "system", language: "" },
  profiles: [],
  profilesTotal: 0,
  profilesRevision: "0",
  profileBaseIds: new Set(),
  selectedSourceData: null,
  sourceOptionsPending: null,
  diagnosticPolicy: { networkMode: "direct", httpProxy: "", refresh: false, udpRetry: false },
  diagnosticRun: null,
  diagnosticPage: null,
  diagnosticOffset: 0,
  diagnosticError: "",
  diagnosticVersion: 0,
  torrent: null,
  editor: null,
  editorPreview: null,
  tab: "files",
  filter: "",
  selectedJob: null,
  selectedSource: null,
  // Jobs started from this page whose result should open when they finish.
  watchResult: new Set(),
  // Draft fields typed but not yet confirmed by the native side.
  pendingFields: new Set(),
  batchJobs: new Map(),
};

const $ = (id) => document.getElementById(id);

// ---- Rendering --------------------------------------------------------------

let lastWorkspaceKey = "";
let frame = 0;
const dirty = { workspace: false, review: false, jobs: false };

export function invalidate(...parts) {
  for (const p of parts.length ? parts : ["workspace", "review", "jobs"]) dirty[p] = true;
  if (!frame) frame = requestAnimationFrame(flush);
}

function flush() {
  frame = 0;
  if (dirty.workspace) {
    const key = workspaceKey(state);
    // A replacement pager loads its rows asynchronously, so restoring focus
    // after rebuilding cannot find even a clean focused row. Defer until blur,
    // including the interval before the first input event; blur invalidates again.
    const active = document.activeElement;
    const editingRow = active?.dataset.rowEditor === "true" && $("workspace").contains(active);
    if (key !== lastWorkspaceKey && !editingRow) {
      lastWorkspaceKey = key;
      preserveFocus($("workspace"), () => renderWorkspace($("workspace"), state, actions));
    }
  }
  if (dirty.review) preserveFocus($("review"), () => renderReview($("review"), state, actions));
  if (dirty.jobs) preserveFocus($("jobs-panel"), () => renderJobs($("jobs-panel"), state, actions));
  dirty.workspace = dirty.review = dirty.jobs = false;
  document.body.dataset.mode = state.settings.mode;
  for (const b of document.querySelectorAll("#mode-switch [data-mode]")) {
    const on = b.dataset.mode === state.settings.mode;
    b.setAttribute("aria-checked", String(on));
    b.tabIndex = on ? 0 : -1;
  }
}

// Re-rendering must not steal focus or overwrite what the user is typing.
function preserveFocus(container, render) {
  const active = document.activeElement;
  const id = active && container.contains(active) ? active.id : null;
  const typing = id && "value" in active && state.pendingFields.has(active.dataset.field);
  const value = typing ? active.value : undefined;
  let start = null;
  let end = null;
  try {
    start = active?.selectionStart ?? null;
    end = active?.selectionEnd ?? null;
  } catch {
    // Not a text control.
  }
  const scroll = container.scrollTop;
  render();
  container.scrollTop = scroll;
  if (!id) return;
  const el = document.getElementById(id);
  if (!el) return;
  if (value !== undefined) el.value = value;
  el.focus({ preventScroll: true });
  if (start !== null) {
    try {
      el.setSelectionRange(start, end);
    } catch {
      // Selection is not supported for this input type.
    }
  }
}

// ---- Errors -------------------------------------------------------------------

async function guarded(promise) {
  try {
    return await promise;
  } catch (err) {
    if (err.code === "STALE_REVISION") {
      await refresh();
      toast(t("staleRevision"));
    } else {
      toast(`${t("errorPrefix")}: ${err.message}`, { error: true });
    }
    return null;
  }
}

// ---- Actions --------------------------------------------------------------------

function applyDraft(draft) {
  if (!draft) return;
  state.draft = draft;
  includeSelectedProfile(draft.profileMeta);
  hydrateDraftText(draft);
  if (state.selectedSource) actions.selectSource(state.selectedSource);
  if (draft.scan) state.scan = { ...state.scan, ...draft.scan };
  invalidate();
  validateSoon();
}

function includeSelectedProfile(profile) {
  if (!profile) return;
  state.profiles = state.profiles.filter((item) => state.profileBaseIds.has(item.id));
  if (!state.profiles.some((item) => item.id === profile.id)) state.profiles.push(profile);
}

function applyProfileList(result) {
  state.profiles = result.profiles;
  state.profileBaseIds = new Set(result.profiles.slice(0, 50).map((profile) => profile.id));
  state.profilesTotal = result.profilesTotal ?? result.profiles.length;
  state.profilesRevision = result.profilesRevision ?? "0";
}

async function hydrateDraftText(draft) {
  for (const key of Object.keys(draft.textFields ?? {})) {
    // Normal draft text is at most 64 KiB. Oversized legacy profile values
    // remain readable in chunks and can be replaced explicitly in Metadata.
    if (draft.textFields[key] > 65536) continue;
    try {
      let text = "", offset = 0;
      do {
        const page = await request("getModelText", { model: "draft", key, offset, revision: draft.revision });
        if (state.draft !== draft) return;
        text += page.text;
        offset = page.nextOffset;
      } while (offset !== null);
      draft[key] = text;
      delete draft.textFields[key];
      // Invalidate even while the native draft revision stays the same.
      lastWorkspaceKey = "";
      invalidate("workspace", "review");
    } catch (error) { if (error.code === "STALE_REVISION") return; }
  }
}

const validateSoon = debounce(async () => {
  const v = await request("validateDraft").catch(() => null);
  if (v) {
    state.validation = v;
    invalidate("review", "workspace");
  }
}, 120);

const patchQueue = { patch: {}, timer: null };

async function sendPatch() {
  const patch = patchQueue.patch;
  patchQueue.patch = {};
  if (!Object.keys(patch).length) return;
  const result = await guarded(request("updateDraft", { patch }, { draftRevision: state.draft?.revision }));
  for (const key of Object.keys(patch)) {
    if (!(key in patchQueue.patch)) state.pendingFields.delete(key);
  }
  if (result) applyDraft(result.draft);
}

const sendPatchSoon = debounce(sendPatch, 350);

export const actions = {
  renderEditor: () => invalidate("workspace"),
  renderDiagnostics() { state.diagnosticVersion++; invalidate("workspace"); },
  reloadSnapshot: () => refresh(),
  async loadDiagnostics(offset = state.diagnosticOffset) {
    if (!state.diagnosticRun) return;
    const id = state.diagnosticRun.id;
    const page = await request("getDiagnosticPage", { runId: id, offset, limit: 50 });
    if (state.diagnosticRun?.id !== id) return;
    state.diagnosticPage = page; state.diagnosticRun = page.run; state.diagnosticOffset = offset;
    actions.renderDiagnostics();
  },
  async startDiagnostics(kind, torrentId = "") {
    state.diagnosticError = "";
    try {
      await sendPatch();
      const policy = { ...state.diagnosticPolicy };
      if (policy.networkMode === "direct") policy.httpProxy = "";
      const result = await request("startDiagnostics", { kind, torrentId, ...policy });
      state.diagnosticRun = { id: result.runId, state: "running", completed: 0, total: 0, network: policy.networkMode };
      state.diagnosticOffset = 0;
      await actions.loadDiagnostics(0);
    } catch (error) { state.diagnosticError = `${error.code}: ${error.message}`; }
    actions.renderDiagnostics();
  },
  async cancelDiagnostics() {
    if (state.diagnosticRun) await guarded(request("cancelDiagnostics", { runId: state.diagnosticRun.id }));
  },
  async updateCatalog() {
    state.diagnosticError = "";
    try {
      const policy = { ...state.diagnosticPolicy };
      if (policy.networkMode === "direct") policy.httpProxy = "";
      const r = await request("updateTrackerCatalog", policy);
      state.diagnosticRun = { id: r.runId, state: "running", completed: 0, total: 1, network: policy.networkMode };
      await actions.loadDiagnostics(0);
    } catch (error) { state.diagnosticError = `${error.code}: ${error.message}`; }
    actions.renderDiagnostics();
  },
  async reviewCatalog() {
    const plan = await guarded(request("planCatalogApply"));
    if (!plan) return;
    if (plan.privateBlocked) return toast(t("diagnosticCatalogPrivate"), { error: true });
    if (!plan.checksum) return toast(t("diagnosticCatalogNotFetched"));
    const text = `${plan.source}\n+ ${plan.added.join("\n+ ")}\n− ${plan.removed.join("\n− ")}`;
    if (!await showConfirm(t("diagnosticCatalogReview"), text)) return;
    const result = await guarded(request("applyTrackerCatalog", { checksum: plan.checksum }, { draftRevision: plan.draftRevision }));
    if (result?.draft) applyDraft(result.draft);
  },
  async selectSources(kind) {
    const r = await guarded(request("selectSources", { kind }));
    if (r?.draft) applyDraft(r.draft);
  },
  async dropFiles(files) {
    if (!files?.length) return;
    const r = await guarded(requestWithFiles("addDroppedSources", Array.from(files)));
    if (r?.draft) applyDraft(r.draft);
  },
  async removeSource(id, revision = state.draft.revision) {
    const r = await guarded(request("removeSource", { sourceId: id }, { draftRevision: revision }));
    if (r?.draft) {
      if (state.selectedSource === id) { state.selectedSource = null; state.selectedSourceData = null; }
      applyDraft(r.draft);
    }
  },
  async setSourceOptions(id, options, revision = state.draft.revision) {
    const r = await guarded(request("setSourceOptions", { sourceId: id, options }, { draftRevision: revision }));
    if (r?.draft) { state.sourceOptionsPending = null; applyDraft(r.draft); }
  },
  stageSourceOption(id, key, value) {
    const values = state.sourceOptionsPending?.sourceId === id ? state.sourceOptionsPending.values : {};
    state.sourceOptionsPending = { sourceId: id, values: { ...values, [key]: value } };
  },
  // Text fields are debounced; choices are sent at once.
  edit(field, value, { immediate = false } = {}) {
    state.pendingFields.add(field);
    patchQueue.patch[field] = value;
    if (immediate) sendPatchSoon.flush();
    else sendPatchSoon();
  },
  async chooseOutput() {
    await sendPatch();
    const r = await guarded(request("chooseOutput"));
    if (r?.draft) applyDraft(r.draft);
  },
  async newDraft() {
    const r = await guarded(request("newDraft"));
    if (r?.draft) {
      state.torrent = null;
      state.editor = null;
      state.editorPreview = null;
      applyDraft(r.draft);
    }
  },
  async pickProfile(profileId) {
    if (profileId === state.draft?.profile) return;
    await sendPatch();
    const plan = await guarded(request("planProfile", { profileId }));
    if (!plan) return invalidate("workspace");
    const apply = async () => {
      const r = await guarded(request("applyProfile", { profileId }, { draftRevision: plan.draftRevision }));
      if (r?.draft) {
        includeSelectedProfile(plan.profile);
        applyDraft(r.draft);
        toast(t("profileApplied"), { action: t("undo"), onAction: () => actions.undo() });
      }
    };
    if (plan.changes.length <= 1) return apply(); // only the profile name changes
    showProfileChange(plan, apply, () => invalidate("workspace"), actions);
  },
  async undo() {
    const r = await guarded(request("undoDraft"));
    if (r?.draft) applyDraft(r.draft);
  },
  async saveProfile() {
    await sendPatch();
    showSaveProfile(async (name) => {
      const result = await request("saveProfile", { name });
      applyProfileList(result);
      applyDraft(result.draft);
    });
  },
  async deleteProfile(profileId) {
    const r = await guarded(request("deleteProfile", { profileId }));
    if (r) {
      applyProfileList(r);
      if (r.draft) applyDraft(r.draft);
      else invalidate();
    }
  },
  async exportProfile(profileId) {
    await guarded(request("exportProfile", { profileId, includeSecrets: false }));
  },
  async create() {
    await sendPatch();
    const r = await guarded(request("startCreate"));
    if (r?.jobId) {
      state.watchResult.add(r.jobId);
      toast(t("creating"));
      await settleWatch(r.jobId);
    }
  },
  async planBatch(mode, policy, chooseFolder = false) {
    await sendPatch();
    return guarded(request("planBatch", { mode, policy, chooseFolder }));
  },
  updateBatch(overrides, revision) {
    return guarded(request("updateBatch", { overrides, revision }));
  },
  async startBatch(revision) {
    const r = await guarded(request("startBatch", { revision }));
    if (r?.batchId) {
      state.batchJobs.set(r.batchId, { batchId: r.batchId, total: r.total });
      checkBatchSoon();
    } else if (r?.jobIds?.length) state.batchJobs.set(r.jobIds[0], r.jobIds);
    return r;
  },
  openBatch() {
    showBatch(actions);
  },
  pauseJob: (jobId) => guarded(request("pauseJob", { jobId })),
  resumeJob: (jobId) => guarded(request("resumeJob", { jobId })),
  async cancelJob(jobId) {
    if (await showConfirm(t("confirmCancelTitle"))) await guarded(request("cancelJob", { jobId }));
  },
  async clearFinished() {
    if (await guarded(request("clearFinishedJobs")) === null) return;
    for (const [id, job] of state.jobs) if (isTerminal(job.state)) {
      state.jobs.delete(id); state.watchResult.delete(id);
    }
    if (!state.jobs.has(state.selectedJob)) state.selectedJob = null;
    state.jobHistory.previous.length = 0;
    await actions.loadJobsPage();
    checkBatchSoon();
  },
  loadJobsPage: (offset) => guarded(loadJobsPage(offset)),
  firstJobsPage() { state.jobHistory.previous.length = 0; return actions.loadJobsPage(0); },
  previousJobsPage() { return actions.loadJobsPage(state.jobHistory.previous.pop() ?? Math.max(0, state.jobHistory.offset - 50)); },
  nextJobsPage() {
    if (state.jobHistory.nextOffset === null) return;
    rememberJobsOffset(); return actions.loadJobsPage(state.jobHistory.nextOffset);
  },
  latestJobsPage() { rememberJobsOffset(); state.jobHistory.followNewest = true; return actions.loadJobsPage(); },
  selectJob(jobId) {
    state.selectedJob = jobId;
    invalidate("jobs", "workspace");
  },
  showResult(jobId) {
    const job = state.jobs.get(jobId);
    if (job?.result) showResult(job, actions, state.settings);
  },
  showInFolder: (id) => guarded(request("showInFolder", { id })),
  async openInClient(id, dontAsk) {
    if (dontAsk) await actions.updateSettings({ openClientWithoutAsking: true });
    return guarded(request("openInClient", { id }));
  },
  async copyMagnet(id) {
    const r = await guarded(request("exportMagnet", { id }));
    if (!r) return;
    try {
      await navigator.clipboard.writeText(r.magnet);
      toast(t("copied"));
    } catch {
      showMagnetCopy(r.magnet);
    }
  },
  saveMagnet: (id) => guarded(request("saveMagnet", { id })),
  async openTorrent() {
    const r = await guarded(request("openTorrent"));
    if (r?.torrent) {
      state.torrent = r.torrent;
      state.editor = null;
      state.editorPreview = null;
      await guarded(loadMetadata(state, invalidate));
      state.tab = "expert";
      await actions.updateSettings({ mode: "advanced" });
      invalidate();
    }
  },
  async openJobResult(jobId) {
    const r = await guarded(request("openJobResult", { jobId }));
    if (r?.torrent) {
      state.torrent = r.torrent;
      state.editor = null;
      state.editorPreview = null;
      await guarded(loadMetadata(state, invalidate));
      state.tab = "expert";
      closeDialog();
      await actions.updateSettings({ mode: "advanced" });
      invalidate();
    }
  },
  async verifyTorrent(torrentId) {
    const r = await guarded(request("verifyPayload", { torrentId }));
    if (r?.jobId) state.selectedJob = r.jobId;
  },
  async metadataSaved(result) {
    state.torrent = result.torrent;
    state.editor = null;
    state.editorPreview = null;
    if (result.guaranteeNote) toast(result.guaranteeNote);
    await loadMetadata(state, invalidate);
    invalidate();
  },
  async saveProject() {
    await sendPatch();
    await guarded(request("saveProject"));
  },
  async openProject() {
    const r = await guarded(request("openProject"));
    if (r?.draft) applyDraft(r.draft);
  },
  async updateSettings(patch) {
    const r = await guarded(request("updateSettings", { patch }));
    if (r) {
      state.settings = r;
    }
    applySettings(); // Restore selectors to the committed state after a failed save.
  },
  setTab(tab) {
    state.tab = tab;
    invalidate("workspace");
  },
  setFilter(filter) {
    state.filter = filter;
    invalidate("workspace");
  },
  async selectSource(id) {
    if (state.selectedSource !== id) state.sourceOptionsPending = null;
    state.selectedSource = id;
    const revision = state.draft?.revision;
    const local = state.draft?.sources.find((s) => s.id === id);
    state.selectedSourceData = local ?? null;
    if (!local && state.draft?.pages) {
      const page = await guarded(request("getModelPage", { model: "draft", key: "source", owner: id, offset: 0, limit: 1, revision }));
      if (state.selectedSource !== id || state.draft?.revision !== revision) return;
      state.selectedSourceData = page?.items[0] ?? null;
    }
    lastWorkspaceKey = "";
    invalidate("workspace");
  },
  modelPage: (model, key, owner, revision, offset, limit = 50) => request("getModelPage", { model, key, owner, revision, offset, limit }),
  modelText: (model, key, owner, revision, offset) => request("getModelText", { model, key, owner, revision, offset }),
  async editRow(key, owner, index, action, value, revision) {
    const result = await guarded(request("editDraftRow", { key, owner, index, action, value }, { draftRevision: revision }));
    if (result) applyDraft(result.draft);
  },
  browseCollection(model, key, owner, revision, label) {
    showCollection(label, (offset, limit) => actions.modelPage(model, key, owner, revision, offset, limit),
      (index, path) => actions.readModelText(model, `/${key}/${index}${path}`, owner, revision, label));
  },
  readModelText(model, key, owner, revision, label) {
    showModelText(label, (offset) => request("getModelText", { model, key, owner, revision, offset }));
  },
  browseProfiles() {
    showCollection(t("profileLabel"), (offset, limit) => actions.modelPage("profiles", "items", "", state.profilesRevision, offset, limit),
      null, (profile) => { closeDialog(); actions.pickProfile(profile.id); });
  },
  openLink: (url) => guarded(request("openExternalLink", { url })),
  // Paged reads for the virtualized lists; failures leave the page unloaded.
  manifestPage: (offset, limit, filter) =>
    request("getManifestPage", { offset, limit, filter }).then((r) => ({ total: r.total, items: r.entries })),
  skippedPage: (offset, limit) => request("getSkippedPage", { offset, limit }),
  torrentFilesPage: (torrentId, offset, limit) =>
    request("getTorrentFiles", { torrentId, offset, limit }).then((r) => ({ total: r.total, items: r.files })),
  verifyFilesPage: (jobId, offset, limit, errorsOnly) =>
    request("getVerifyFilesPage", { jobId, offset, limit, errorsOnly }).then((r) => ({ total: r.total, items: r.rows })),
  jobLayoutPage: (jobId, offset, limit) => request("getJobLayoutPage", { jobId, offset, limit }),
  async jobLayoutDetails(jobId, index) {
    const result = await guarded(request("getJobLayoutRow", { jobId, index }));
    if (result) showJobLayoutRow(result);
  },
  jobTextPage: (jobId, kind, offset, limit) => request("getJobTextPage", { jobId, kind, offset, limit }),
  async jobTextDetails(jobId, kind, index, version) {
    const result = await guarded(request("getJobText", { jobId, kind, index, version }));
    if (result) showJobText(kind, result.text);
  },
  async verifyFileDetails(jobId, index) {
    const result = await guarded(request("getVerifyFile", { jobId, index }));
    if (result) showVerifyFile(result.file);
  },
};

function isTerminal(s) {
  return s === "Succeeded" || s === "SucceededWithWarnings" || s === "Failed" || s === "Cancelled";
}

const checkBatchSoon = debounce(async () => {
  for (const [id, batch] of state.batchJobs) {
    if (Array.isArray(batch)) continue;
    try {
      const status = await request("getBatchStatus", { batchId: id });
      if (status.total === 0) { state.batchJobs.delete(id); continue; }
      if (status.finished && status.total === batch.total) {
        state.batchJobs.delete(id);
        toast(t("batchReport", { id, done: status.done, failed: status.failed, cancelled: status.cancelled }));
      }
    } catch { /* Later job events or a snapshot refresh retry the read. */ }
  }
}, 150);

// ---- Events -----------------------------------------------------------------------

let jobEventSerial = 0, readJobEventSerial = 0;
function resolveWatch(job) {
  if (isTerminal(job.state) && state.watchResult.has(job.id)) {
    state.watchResult.delete(job.id);
    if (job.result) showResult(job, actions, state.settings);
  }
}

async function settleWatch(id) {
  if (!state.watchResult.has(id)) return;
  try { resolveWatch(await request("getJobSummary", { jobId: id })); }
  catch (error) { if (error.code === "JOB_NOT_FOUND") state.watchResult.delete(id); }
}

function onJob(job) {
  resolveWatch(job);
  const known = state.jobs.get(job.id);
  if (known && BigInt(known.version) >= BigInt(job.version)) return; // older update
  ++jobEventSerial;
  const history = state.jobHistory;
  if (known || history.followNewest || !state.selectedJob || state.selectedJob === job.id) state.jobs.set(job.id, job);
  if (!known && history.followNewest && !history.ids.includes(job.id)) {
    history.ids.push(job.id);
    if (history.ids.length > 50) history.ids.shift();
  }
  if (!state.selectedJob) state.selectedJob = job.id;
  trimJobs();
  invalidate("jobs");
  if (state.tab === "jobs") invalidate("workspace");
  reportBatches();
  if (!known) refreshJobsSoon();
}

function trimJobs() {
  const keep = new Set(state.jobHistory.ids);
  if (state.selectedJob) keep.add(state.selectedJob);
  for (const id of state.jobs.keys()) if (!keep.has(id)) state.jobs.delete(id);
}

function rememberJobsOffset() {
  state.jobHistory.previous.push(state.jobHistory.offset);
  if (state.jobHistory.previous.length > 64) state.jobHistory.previous.shift();
}

function applyJobsPage(page, offset, beforeVersions) {
  const history = state.jobHistory;
  const newer = [...state.jobs.values()].filter((job) => beforeVersions.get(job.id) !== job.version);
  const selected = state.jobs.get(state.selectedJob);
  const oldJobs = new Map(state.jobs);
  state.jobs.clear();
  for (const job of page.jobs) {
    const known = oldJobs.get(job.id);
    state.jobs.set(job.id, known && BigInt(known.version) > BigInt(job.version) ? known : job);
    resolveWatch(state.jobs.get(job.id));
  }
  if (page.total === 0) history.followNewest = true;
  history.ids = page.jobs.map((job) => job.id);
  for (const job of newer) {
    const row = state.jobs.get(job.id);
    if (row && BigInt(row.version) >= BigInt(job.version)) continue;
    if (row || history.followNewest || job.id === state.selectedJob) {
      state.jobs.set(job.id, job);
      if (history.followNewest && !history.ids.includes(job.id)) history.ids.push(job.id);
    }
  }
  history.ids = history.ids.slice(-50);
  if (selected && !state.jobs.has(selected.id)) state.jobs.set(selected.id, selected);
  history.offset = offset; history.total = page.total; history.nextOffset = page.nextOffset;
  history.offsetIntent = null;
  readJobEventSerial = jobEventSerial;
  if (!state.selectedJob) state.selectedJob = history.ids[0] ?? null;
  trimJobs();
  invalidate("jobs", "workspace");
}

let jobReadsInFlight = 0;
async function loadJobsPage(requested) {
  ++jobReadsInFlight;
  try { return await readJobsPage(requested); }
  finally { --jobReadsInFlight; }
}

async function readJobsPage(requested) {
  const history = state.jobHistory;
  const serial = ++history.serial;
  if (requested !== undefined) { history.followNewest = false; history.offsetIntent = requested; }
  for (let attempt = 0; attempt < 3; ++attempt) {
    const beforeVersions = new Map([...state.jobs].map(([id, job]) => [id, job.version]));
    const meta = await request("getJobsPage", { offset: 0, limit: 0 });
    let offset = Math.min(requested ?? (history.followNewest ? Math.max(0, meta.total - 50) : history.offsetIntent ?? history.offset), Math.max(0, meta.total - 1));
    let page = await request("getJobsPage", { offset, limit: 50 });
    if (history.followNewest && page.nextOffset !== null) {
      // A byte-budget page can stop before the final job. Latest must include
      // the actual tail; First/Next still traverse every intervening row.
      offset = Math.max(0, page.total - 1);
      page = await request("getJobsPage", { offset, limit: 50 });
    }
    if (serial !== history.serial) return;
    if (page.collectionRevision !== meta.collectionRevision) continue;
    applyJobsPage(page, offset, beforeVersions);
    return page;
  }
  throw new Error("Jobs changed while reading this page; try again");
}

const refreshJobsSoon = debounce(() => {
  if (jobEventSerial <= readJobEventSerial) return;
  if (jobReadsInFlight) refreshJobsSoon();
  else actions.loadJobsPage();
}, 150);

function reportBatches() {
  for (const [first, ids] of state.batchJobs) {
    if (!Array.isArray(ids)) { checkBatchSoon(); continue; }
    const jobs = ids.map((id) => state.jobs.get(id));
    if (jobs.every((j) => j && isTerminal(j.state))) {
      state.batchJobs.delete(first);
      const count = (pred) => jobs.filter(pred).length;
      toast(t("batchReport", {
        id: jobs[0].batchId,
        done: count((j) => j.state.startsWith("Succeeded")),
        failed: count((j) => j.state === "Failed"),
        cancelled: count((j) => j.state === "Cancelled"),
      }));
    }
  }
}

onEvent((type, payload) => {
  if (type === "job" && payload.job) onJob(payload.job);
  else if (type === "diagnostics" && payload.run) {
    if (state.diagnosticRun?.sequence && payload.run.sequence && BigInt(payload.run.sequence) < BigInt(state.diagnosticRun.sequence)) return;
    if (state.diagnosticRun?.id === payload.run.id && state.diagnosticRun.completed > payload.run.completed) return;
    state.diagnosticRun = payload.run;
    actions.loadDiagnostics().catch(() => {});
  }
  else if (type === "scan") {
    if (state.draft?.scan && payload.sourcesRevision !== state.draft.scan.sourcesRevision && payload.state !== "scanning") {
      // A result for an older source set; a refresh brings the current one.
      refresh();
      return;
    }
    state.scan = payload;
    if (state.draft) state.draft.scan = { state: payload.state, sourcesRevision: payload.sourcesRevision, error: payload.error };
    invalidate();
    validateSoon();
  }
  else if (type === "resyncRequired") refresh().catch((error) => toast(error.message, "error"));
});

// ---- Settings, snapshot and start ------------------------------------------------

function applySettings() {
  const s = state.settings;
  setLocale(s.language);
  if (s.theme === "light" || s.theme === "dark") document.documentElement.dataset.theme = s.theme;
  else delete document.documentElement.dataset.theme;
  $("theme-select").value = s.theme;
  $("language-select").value = s.language ?? "";
  $("settings-persistence").hidden = s.persistence !== "memory";
  applyTranslations();
  lastWorkspaceKey = "";
  invalidate();
}

async function refresh() {
  ++jobReadsInFlight;
  try { await readSnapshot(); }
  finally { --jobReadsInFlight; }
}

async function readSnapshot() {
  let snap;
  let beforeVersions;
  let page, offset;
  const history = state.jobHistory;
  const serial = ++history.serial;
  for (let attempt = 0; attempt < 3; ++attempt) {
    beforeVersions = new Map([...state.jobs].map(([id, job]) => [id, job.version]));
    snap = await request("getSnapshot");
    const total = snap.jobsTotal ?? snap.jobs.length;
    offset = Math.min(history.followNewest ? Math.max(0, total - 50) : history.offsetIntent ?? history.offset, Math.max(0, total - 1));
    page = offset === 0 ? { jobs: snap.jobs.slice(0, 50), total, nextOffset: snap.nextJobsOffset ?? null, collectionRevision: snap.jobsRevision }
      : await request("getJobsPage", { offset, limit: 50 });
    if (history.followNewest && page.nextOffset !== null) {
      offset = Math.max(0, total - 1);
      page = await request("getJobsPage", { offset, limit: 50 });
    }
    if (page.collectionRevision === snap.jobsRevision) break;
    snap = null;
  }
  if (!snap) throw new Error("Jobs changed while refreshing; try again");
  state.draft = snap.draft;
  hydrateDraftText(snap.draft);
  state.scan = snap.scan;
  state.settings = snap.settings;
  applyProfileList(snap);
  includeSelectedProfile(snap.draft.profileMeta);
  state.diagnosticRun = (snap.diagnostics ?? []).at(-1) ?? null;
  if (state.diagnosticRun) {
    state.diagnosticPolicy.networkMode = state.diagnosticRun.network;
    await actions.loadDiagnostics(0);
  }
  state.torrent = snap.torrent ?? null;
  state.editorPreview = snap.editorPreview ?? null;
  if (state.torrent) {
    state.tab = "expert";
    await loadMetadata(state, invalidate);
  } else state.editor = null;
  if (serial === history.serial) {
    // Only the selected off-page summary is pinned. Full reports stay native.
    if (state.selectedJob && !page.jobs.some((job) => job.id === state.selectedJob)) {
      const selectedId = state.selectedJob;
      try {
        const selected = await request("getJobSummary", { jobId: selectedId });
        const known = state.jobs.get(selectedId);
        if (state.selectedJob === selectedId && (!known || BigInt(known.version) < BigInt(selected.version))) state.jobs.set(selectedId, selected);
      } catch (error) {
        if (error.code !== "JOB_NOT_FOUND") throw error;
        state.jobs.delete(selectedId); state.watchResult.delete(selectedId);
        if (state.selectedJob === selectedId) state.selectedJob = null;
      }
    }
    if (serial === history.serial) applyJobsPage(page, offset, beforeVersions);
    // A resync can cover a lost terminal event for a watched off-page job.
    // Read only these IDs; never rebuild the complete job cache.
    for (const id of state.watchResult) await settleWatch(id);
    reportBatches();
  }
  state.pendingFields.clear();
  applySettings();
  validateSoon();
}

function wireChrome() {
  for (const b of document.querySelectorAll("#mode-switch [data-mode]")) {
    b.addEventListener("click", () => actions.updateSettings({ mode: b.dataset.mode }));
  }
  $("mode-switch").addEventListener("keydown", (e) => {
    if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
    const next = state.settings.mode === "simple" ? "advanced" : "simple";
    actions.updateSettings({ mode: next }).then(() => document.querySelector(`#mode-switch [data-mode="${next}"]`)?.focus());
    e.preventDefault();
  });
  $("theme-select").addEventListener("change", (e) => actions.updateSettings({ theme: e.target.value }));
  $("language-select").addEventListener("change", (e) => actions.updateSettings({ language: e.target.value }));
  $("btn-new").addEventListener("click", () => actions.newDraft());
  $("btn-open-project").addEventListener("click", () => actions.openProject());
  $("btn-save-project").addEventListener("click", () => actions.saveProject());
  $("btn-open-torrent").addEventListener("click", () => actions.openTorrent());

  // Drops anywhere in the window add sources; the browser never navigates.
  window.addEventListener("dragover", (e) => {
    e.preventDefault();
    document.body.classList.add("dragging");
  });
  window.addEventListener("dragleave", (e) => {
    if (e.relatedTarget === null) document.body.classList.remove("dragging");
  });
  window.addEventListener("drop", (e) => {
    e.preventDefault();
    document.body.classList.remove("dragging");
    actions.dropFiles(e.dataTransfer?.files);
  });
}

async function start() {
  setLocale("");
  applyTranslations();
  const selfTest = new URLSearchParams(location.search).get("selfTest") === "1";
  if (!isAvailable()) {
    $("unavailable").hidden = false;
    return;
  }
  wireChrome();
  try {
    const info = await request("getEngineInfo");
    await refresh();
    if (selfTest) {
      if (new URLSearchParams(location.search).get("sessionFlow") === "1") {
        const { runSessionFlow } = await import("./session-self-test.js");
        await request("reportSelfTest", { ok: true, ...await runSessionFlow(actions, state) });
        return;
      }
      if (new URLSearchParams(location.search).get("smbFlow") === "1") {
        const { runSmbFlow } = await import("./smb-self-test.js");
        await runSmbFlow(actions, state);
        return;
      }
      if (new URLSearchParams(location.search).get("memoryFlow") === "1") {
        const { runMemoryFlow } = await import("./memory-self-test.js");
        await request("reportSelfTest", { ok: true, ...await runMemoryFlow(actions, state) });
        return;
      }
      if (new URLSearchParams(location.search).get("nativeFlow") === "1") {
        const { runNativeFlow } = await import("./windows-self-test.js");
        const result = await runNativeFlow(actions, state, info);
        if (result) await request("reportSelfTest", { ok: true, ...result });
        return;
      }
      const { runSelfTest } = await import("./self-test.js");
      await request("reportSelfTest", { ok: true, ...await runSelfTest(actions, state, info) });
    }
  } catch (err) {
    $("unavailable").hidden = false;
    if (selfTest) await request("reportSelfTest", { ok: false, error: String(err?.code ?? err),
      message: String(err?.message ?? err) }).catch(() => {});
  }
}

start();
