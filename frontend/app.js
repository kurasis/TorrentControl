// Application controller: holds the page state, talks to the native side and
// re-renders views. The native job and draft state is authoritative; the page
// only mirrors it and can always be rebuilt from getSnapshot (U03).
import { applyTranslations, setLocale, t } from "./i18n.js";
import { isAvailable, onEvent, request, requestWithFiles } from "./bridge.js";
import { debounce, toast } from "./dom.js";
import { renderWorkspace, renderReview, renderJobs, workspaceKey } from "./views.js";
import { showResult, showProfileChange, showBatch, showConfirm, showSaveProfile, showMagnetCopy, closeDialog } from "./dialogs.js";

export const state = {
  draft: null,
  scan: { state: "empty" },
  validation: null,
  jobs: new Map(),
  settings: { mode: "simple", theme: "system", language: "" },
  profiles: [],
  torrent: null,
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
    if (key !== lastWorkspaceKey) {
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
  if (draft.scan) state.scan = { ...state.scan, ...draft.scan };
  invalidate();
  validateSoon();
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
  async selectSources(kind) {
    const r = await guarded(request("selectSources", { kind }));
    if (r?.draft) applyDraft(r.draft);
  },
  async dropFiles(files) {
    if (!files?.length) return;
    const r = await guarded(requestWithFiles("addDroppedSources", Array.from(files)));
    if (r?.draft) applyDraft(r.draft);
  },
  async removeSource(id) {
    const r = await guarded(request("removeSource", { sourceId: id }, { draftRevision: state.draft.revision }));
    if (r?.draft) applyDraft(r.draft);
  },
  async setSourceOptions(id, options) {
    const r = await guarded(request("setSourceOptions", { sourceId: id, options }, { draftRevision: state.draft.revision }));
    if (r?.draft) applyDraft(r.draft);
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
        applyDraft(r.draft);
        toast(t("profileApplied"), { action: t("undo"), onAction: () => actions.undo() });
      }
    };
    if (plan.changes.length <= 1) return apply(); // only the profile name changes
    showProfileChange(plan, apply, () => invalidate("workspace"));
  },
  async undo() {
    const r = await guarded(request("undoDraft"));
    if (r?.draft) applyDraft(r.draft);
  },
  async saveProfile() {
    await sendPatch();
    showSaveProfile(async (name) => {
      const r = await request("saveProfile", { name });
      state.profiles = r.profiles;
      applyDraft(r.draft);
    });
  },
  async deleteProfile(profileId) {
    const r = await guarded(request("deleteProfile", { profileId }));
    if (r) {
      state.profiles = r.profiles;
      invalidate();
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
    }
  },
  async planBatch(mode, policy, chooseFolder = false) {
    await sendPatch();
    return guarded(request("planBatch", { mode, policy, chooseFolder }));
  },
  updateBatch(overrides) {
    return guarded(request("updateBatch", { overrides }));
  },
  async startBatch() {
    const r = await guarded(request("startBatch"));
    if (r?.jobIds?.length) state.batchJobs.set(r.jobIds[0], r.jobIds);
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
    await guarded(request("clearFinishedJobs"));
    for (const [id, job] of state.jobs) if (isTerminal(job.state)) state.jobs.delete(id);
    invalidate("jobs");
  },
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
      state.tab = "expert";
      await actions.updateSettings({ mode: "advanced" });
      invalidate();
    }
  },
  async openJobResult(jobId) {
    const r = await guarded(request("openJobResult", { jobId }));
    if (r?.torrent) {
      state.torrent = r.torrent;
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
      applySettings();
    }
  },
  setTab(tab) {
    state.tab = tab;
    invalidate("workspace");
  },
  setFilter(filter) {
    state.filter = filter;
    invalidate("workspace");
  },
  selectSource(id) {
    state.selectedSource = id;
    invalidate("workspace");
  },
  openLink: (url) => guarded(request("openExternalLink", { url })),
  // Paged reads for the virtualized lists; failures leave the page unloaded.
  manifestPage: (offset, limit, filter) =>
    request("getManifestPage", { offset, limit, filter }).then((r) => ({ total: r.total, items: r.entries })),
  skippedPage: (offset, limit) => request("getSkippedPage", { offset, limit }),
  torrentFilesPage: (torrentId, offset, limit) =>
    request("getTorrentFiles", { torrentId, offset, limit }).then((r) => ({ total: r.total, items: r.files })),
};

function isTerminal(s) {
  return s === "Succeeded" || s === "SucceededWithWarnings" || s === "Failed" || s === "Cancelled";
}

// ---- Events -----------------------------------------------------------------------

function onJob(job) {
  const known = state.jobs.get(job.id);
  if (known && BigInt(known.version) >= BigInt(job.version)) return; // older update
  state.jobs.set(job.id, job);
  if (!state.selectedJob) state.selectedJob = job.id;
  invalidate("jobs");
  if (state.tab === "jobs") invalidate("workspace");
  if (isTerminal(job.state) && state.watchResult.has(job.id)) {
    state.watchResult.delete(job.id);
    if (job.result) showResult(job, actions, state.settings);
  }
  for (const [first, ids] of state.batchJobs) {
    if (!ids.includes(job.id)) continue;
    const jobs = ids.map((id) => state.jobs.get(id));
    if (jobs.every((j) => j && isTerminal(j.state))) {
      state.batchJobs.delete(first);
      const count = (pred) => jobs.filter(pred).length;
      toast(t("batchReport", {
        id: job.batchId,
        done: count((j) => j.state.startsWith("Succeeded")),
        failed: count((j) => j.state === "Failed"),
        cancelled: count((j) => j.state === "Cancelled"),
      }));
    }
  }
}

onEvent((type, payload) => {
  if (type === "job" && payload.job) onJob(payload.job);
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
});

// ---- Settings, snapshot and start ------------------------------------------------

function applySettings() {
  const s = state.settings;
  setLocale(s.language);
  if (s.theme === "light" || s.theme === "dark") document.documentElement.dataset.theme = s.theme;
  else delete document.documentElement.dataset.theme;
  $("theme-select").value = s.theme;
  $("language-select").value = s.language ?? "";
  applyTranslations();
  lastWorkspaceKey = "";
  invalidate();
}

async function refresh() {
  const snap = await request("getSnapshot");
  state.draft = snap.draft;
  state.scan = snap.scan;
  state.settings = snap.settings;
  state.profiles = snap.profiles;
  for (const job of snap.jobs) onJob(job);
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
    if (selfTest) await request("reportSelfTest", { ok: false, error: String(err?.code ?? err) }).catch(() => {});
  }
}

start();
