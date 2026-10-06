// A stand-in for the native host, injected before the page loads. It speaks
// the same request/response/event protocol as src/bridge and keeps just
// enough state for the UI tests. Tests drive it through window.__mock.
(() => {
  const config = Object.assign({ entries: 3, entryName: (i) => `file-${i}.bin` }, window.__mockConfig ?? {});
  const listeners = [];
  let sequence = 0;
  let revision = 1;
  const requests = [];

  const profiles = [
    { id: "public", name: "Public", builtin: true, format: "hybrid" },
    { id: "private", name: "Private", builtin: true, format: "v1" },
  ];
  const draft = {
    revision: "1",
    sources: [],
    name: "",
    effectiveName: "",
    profile: "public",
    format: "hybrid",
    pieceLength: 0,
    private: false,
    trackers: [{ url: "udp://tracker.example:1337/announce", tier: 0, enabled: true }],
    webSeeds: [],
    dhtNodes: [],
    comment: "",
    creator: "",
    source: "",
    creationDate: "now",
    fixedDate: "0",
    output: "",
    replaceExisting: false,
    allowHydration: false,
    acceptLargeResourceUse: false,
  };
  if (config.sourcesCount) draft.sources = Array.from({ length: config.sourcesCount }, (_, i) => ({
    id: `source-${i}`, name: `Source-${i}`, path: `C:\\data\\Source-${i}`, isDirectory: true,
    recursive: true, followLinks: false, skipCloud: false, exclusions: [],
  }));
  if (config.trackersCount) draft.trackers = Array.from({ length: config.trackersCount }, (_, i) => ({ url: `https://tracker-${i}.example/announce`, tier: i % 999, enabled: true }));
  if (config.truncatedTracker) draft.trackers = [{ url: "https://legacy.example/" + "x".repeat(100000), tier: 0, enabled: true }];
  if (config.webSeedsCount) draft.webSeeds = Array.from({ length: config.webSeedsCount }, (_, i) => `https://seed-${i}.example/`);
  if (config.profilesCount) profiles.push(...Array.from({ length: config.profilesCount }, (_, i) => ({ id: `custom-${i}`, name: `Profile-${i}`, builtin: false, format: "hybrid" })));
  if (config.exclusionsCount && draft.sources.length) draft.sources[0].exclusions = Array.from({ length: config.exclusionsCount }, (_, i) => `pattern-${i}`);
  let batchPlan = null, batchRevision = 0;
  let scan = { state: "empty", sourcesRevision: "0" };
  const settings = Object.assign({ theme: "system", language: "en", mode: "simple", openClientWithoutAsking: false }, config.settings);
  const jobs = config.jobs ?? [];
  let torrent = config.torrent ?? null;
  let editorPreview = config.editorPreview ?? null;
  const registry = [
    { scope: "top", key: "comment", support: "form", type: "str", editable: true, validation: "text", reference: "BEP 3" },
    { scope: "top", key: "announce-list", support: "form", type: "list", editable: true, validation: "tracker-tiers", reference: "BEP 12" },
    { scope: "top", key: "httpseeds", support: "form", type: "list", editable: true, validation: "http-seeds", reference: "BEP 17" },
    { scope: "info", key: "source", support: "form", type: "str", editable: true, validation: "text", reference: "nonstandard" },
    { scope: "info", key: "pieces", support: "computed", type: "str", editable: false, validation: "rebuild", reference: "BEP 3" },
  ];
  const values = { "top:comment": { t: "str", utf8: "Original comment" }, "info:source": { t: "str", utf8: "old" },
    "info:pieces": { t: "bytes", hex: "00ff" } };
  let editorCounter = 0;
  let diagnosticRun = config.diagnosticRun ?? null;
  const diagnosticRows = config.diagnosticRows ?? [{ endpoint: "http://tracker.example:80", kind: "tracker", operation: "scrape-random-hash", state: "protocol-responding", checkedAt: "1791158400", cached: false, integrity: "not-verified", attempts: [{ family: 4, state: "protocol-responding", httpStatus: 200, latencyMs: 4 }] }];
  let diagnosticCounter = 0;

  function boundedPage(items, offset = 0, limit = 50, version = String(revision)) {
    const size = Math.min(limit, config.modelPageSize ?? 50);
    const rows = items.slice(offset, offset + size);
    return { offset, items: structuredClone(rows), total: items.length, revision: version,
      nextOffset: offset + rows.length < items.length ? offset + rows.length : null };
  }
  function sourceRow(source) {
    return { ...source, exclusions: source.exclusions.length > 3 ? [] : source.exclusions,
      exclusionsTotal: source.exclusions.length, exclusionsPaged: source.exclusions.length > 3 };
  }
  function trackerRow(row) {
    return row.url.length > 32768 ? { ...row, url: row.url.slice(0, 512), displayTruncated: true } : row;
  }
  function snapshotDraft() {
    const result = structuredClone({ ...draft, revision: String(revision), scan, profileMeta: profiles.find((profile) => profile.id === draft.profile) });
    if (config.modelPaging) {
      result.pages = {};
      for (const key of ["sources", "trackers", "webSeeds"]) {
        const page = boundedPage(key === "sources" ? draft.sources.map(sourceRow) : key === "trackers" ? draft.trackers.map(trackerRow) : draft[key]);
        result[key] = page.items; delete page.items; result.pages[key] = page;
      }
    }
    return result;
  }

  function bump() {
    revision += 1;
    draft.effectiveName = draft.name || (draft.sources[0]?.name ?? "");
  }

  function entry(i) {
    const name = config.entryName(i);
    return { sourceId: "s1", torrentPath: `${draft.effectiveName}/${name}`, source: `C:\\data\\${name}`, length: String(1024 * (i + 1)), reason: "included" };
  }

  function emit(type, payload) {
    sequence += 1;
    const data = JSON.stringify({ protocolVersion: 1, event: type, sequence: String(sequence), payload: { type, ...payload } });
    setTimeout(() => listeners.forEach((l) => l({ data })), 0);
  }

  function addSource(name) {
    draft.sources.push({ id: `s${draft.sources.length + 1}`, path: `C:\\data\\${name}`, name, isDirectory: true, recursive: true, exclusions: [], followLinks: false, skipCloud: false });
    draft.output = `C:\\data\\${draft.sources[0].name}.torrent`;
    bump();
    scan = { state: "ready", sourcesRevision: String(draft.sources.length) };
    emit("scan", { state: "ready", sourcesRevision: scan.sourcesRevision, draftRevision: String(revision) });
  }

  const ops = {
    getModelPage: (p) => {
      if (config.failModelPageOnce) { config.failModelPageOnce = false; throw Object.assign(new Error("Read failed"), { code: "IO_ERROR" }); }
      if (p.model === "draft") {
        if (p.revision !== String(revision)) throw Object.assign(new Error("Changed"), { code: "STALE_REVISION" });
        if (p.key === "source") return boundedPage([sourceRow(draft.sources.find((s) => s.id === p.owner))], p.offset, p.limit);
        if (p.key === "exclusions") return boundedPage(draft.sources.find((s) => s.id === p.owner).exclusions, p.offset, p.limit);
        return boundedPage(p.key === "sources" ? draft.sources.map(sourceRow) : p.key === "trackers" ? draft.trackers.map(trackerRow) : draft[p.key], p.offset, p.limit);
      }
      if (p.model === "profiles") return boundedPage(profiles, p.offset, p.limit, "0");
      if (p.model === "batch") return boundedPage(batchPlan[p.key], p.offset, p.limit, batchPlan.revision);
      if (p.model === "torrent") return boundedPage(config.torrentCollections[p.key], p.offset, p.limit, p.owner);
      throw new Error("Unknown model");
    },
    getModelText: (p) => {
      const root = p.model === "torrent" ? config.torrentFull ?? torrent : draft;
      const value = p.key.startsWith("/") ? p.key.split("/").slice(1).reduce((row, key) => row[key], root) : root[p.key];
      const text = value.slice(p.offset, p.offset + 8192);
      const next = p.offset + text.length;
      return { text, offset: p.offset, totalBytes: value.length, nextOffset: next < value.length ? next : null };
    },
    editDraftRow: (p, message) => {
      if (message.draftRevision !== String(revision)) throw Object.assign(new Error("Changed"), { code: "STALE_REVISION" });
      const rows = p.key === "exclusions" ? draft.sources.find((s) => s.id === p.owner).exclusions : draft[p.key];
      if (p.action === "set") rows[p.index] = p.value;
      else if (p.action === "remove") rows.splice(p.index, 1);
      else if (p.action === "append") rows.push(p.value);
      else if (p.action === "up" && p.index) [rows[p.index], rows[p.index-1]] = [rows[p.index-1], rows[p.index]];
      bump(); return { draft: snapshotDraft() };
    },
    setSourceOptions: (p) => { Object.assign(draft.sources.find((s) => s.id === p.sourceId), p.options); bump(); return { draft: snapshotDraft() }; },
    planBatch: () => {
      const items = Array.from({ length: config.batchCount ?? 100 }, (_, i) => ({ id: `item-${i}`, name: `Batch-${i}`, output: `C:\\out\\${i}.torrent`, policy: "rename", included: true }));
      batchPlan = { items, notes: [], outputDir: "C:\\out", revision: String(++batchRevision) };
      return { ...batchPlan, items: boundedPage(items).items, total: items.length, includedTotal: items.length };
    },
    updateBatch: (p) => {
      for (const [id, change] of Object.entries(p.overrides)) Object.assign(batchPlan.items.find((row) => row.id === id), change);
      batchPlan.revision = String(++batchRevision);
      return { ...batchPlan, items: boundedPage(batchPlan.items).items, total: batchPlan.items.length, includedTotal: batchPlan.items.filter((row) => row.included).length };
    },
    startBatch: () => {
      const count = batchPlan.items.filter((row) => row.included).length;
      return { batchId: "batch-1", jobIds: Array.from({ length: Math.min(50, count) }, (_, i) => `job-${i}`), total: count, nextOffset: count > 50 ? 50 : null };
    },
    getBatchStatus: () => {
      const count = batchPlan.items.filter((row) => row.included).length;
      return { batchId: "batch-1", total: count, done: count, failed: 0, cancelled: 0, finished: true };
    },
    getSelfTestState: () => ({ checkpoint: config.nativeCheckpoint ?? null,
      rendererRecoveries: config.rendererRecoveries ?? 0, settingsOnly: config.settingsOnly ?? false }),
    reportSelfTest: () => ({}),
    checkSelfTestProfile: (p) => ({ persisted: config.selfTestPersistence !== false
      && profiles.some((profile) => profile.id === p.profileId && profile.name === p.name) }),
    getEngineInfo: () => ({ appVersion: "0.0.0-test", engineVersion: "libtorrent 2.1.2", protocolVersion: 1 }),
    getSnapshot: () => {
      if (config.failSnapshotAfterStart && requests.filter((r) => r.operation === "getSnapshot").length > 1)
        throw Object.assign(new Error("Snapshot read failed"), { code: "IO_ERROR" });
      return { draft: snapshotDraft(), scan, jobs: config.pagedJobs ? jobs.slice(0, 50) : jobs,
      nextJobsOffset: config.pagedJobs && jobs.length > 50 ? 50 : null,
      jobsTotal: jobs.length, jobsRevision: "1", settings, profiles: config.modelPaging ? profiles.slice(0, 50) : profiles, profilesTotal: profiles.length, profilesRevision: "0", torrent, editorPreview, diagnostics: diagnosticRun ? [diagnosticRun] : [] };
    },
    getJobsPage: (p) => ({ jobs: jobs.slice(p.offset, p.offset + p.limit), total: jobs.length, collectionRevision: "1",
      nextOffset: p.offset + p.limit < jobs.length ? p.offset + p.limit : null }),
    clearFinishedJobs: () => {
      for (let i = jobs.length - 1; i >= 0; --i)
        if (/^(Succeeded|SucceededWithWarnings|Failed|Cancelled)$/.test(jobs[i].state)) jobs.splice(i, 1);
      return {};
    },
    getVerifyFilesPage: (p) => {
      const total = p.errorsOnly ? (config.verifyErrors ?? 0) : (config.verifyFiles ?? 0);
      return { total, rows: Array.from({ length: Math.min(p.limit, Math.max(0, total - p.offset)) }, (_, i) => {
        const index = i + p.offset;
        return { index, path: `Payload/file-${index}.bin`, status: p.errorsOnly ? "missing" : "ok", message: "" };
      }) };
    },
    getVerifyFile: (p) => ({ file: { path: config.verifyDetailPath ?? `Payload/file-${p.index}.bin`, status: "missing", message: "Full native detail", badV1Pieces: "0", badV2Pieces: "0" } }),
    getJobLayoutPage: (p) => {
      const layout = jobs.find((job) => job.id === p.jobId)?.result?.layout ?? [];
      return { rows: layout.slice(p.offset, p.offset + p.limit), total: layout.length, realFiles: layout.length, complete: true };
    },
    getJobTextPage: (p) => {
      const job = jobs.find((job) => job.id === p.jobId);
      const lines = p.kind === "log" ? job?.log ?? [] : config.warningLines ?? job?.result?.warnings ?? [];
      return { rows: lines.slice(p.offset, p.offset + p.limit).map((text, i) => ({ text, index: p.offset + i, displayTruncated: false })), total: lines.length, version: job?.version ?? "1" };
    },
    getJobText: (p) => {
      const job = jobs.find((job) => job.id === p.jobId);
      return { text: (p.kind === "log" ? job.log : config.warningLines ?? job.result.warnings)[p.index], version: job.version };
    },
    startDiagnostics: (p) => {
      if (p.networkMode === "http-proxy" && !p.httpProxy) throw Object.assign(new Error("Specify the proxy origin"), { code: "INVALID_PROXY" });
      diagnosticRun = { id: `diagnostic-${++diagnosticCounter}`, sequence: String(diagnosticCounter), state: config.diagnosticBusy ? "running" : "completed", total: diagnosticRows.length, completed: config.diagnosticBusy ? 0 : diagnosticRows.length, network: p.networkMode };
      return { runId: diagnosticRun.id };
    },
    getDiagnosticPage: (p) => ({ run: diagnosticRun, rows: diagnosticRun.state === "running" ? [] : diagnosticRows.slice(p.offset, p.offset + p.limit), total: diagnosticRun.completed }),
    cancelDiagnostics: () => { diagnosticRun = { ...diagnosticRun, state: "cancelled" }; emit("diagnostics", { run: diagnosticRun }); return {}; },
    planCatalogApply: () => ({ checksum: "a".repeat(64), source: "https://catalog.example/list", added: ["udp://new.example:80/announce"], removed: [], privateBlocked: draft.private, draftRevision: String(revision) }),
    applyTrackerCatalog: () => { draft.trackers.push({ url: "udp://new.example:80/announce", enabled: true, tier: 1 }); bump(); return { draft: snapshotDraft() }; },
    openTorrent: () => {
      torrent = { id: "t-1", name: "Original", format: "hybrid", path: "C:\\data\\original.torrent", problems: [],
        realFiles: 1, paddingFiles: 0, payloadBytes: "100", pieceLength: "16384", infohashV1: "a".repeat(40), infohashV2: "b".repeat(64) };
      return { torrent };
    },
    getFieldRegistry: () => ({ fields: registry }),
    getTorrentFields: (p) => {
      const fields = config.fieldRows ?? registry.filter((f) => f.scope === p.scope && values[`${p.scope}:${f.key}`]).map((f) =>
        ({ key: { t: "str", utf8: f.key }, value: values[`${p.scope}:${f.key}`], descriptor: f, editable: f.editable }));
      const offset = p.offset ?? 0;
      const end = offset + Math.min(p.limit ?? 50, config.fieldPageSize ?? 50);
      return { total: fields.length, rows: fields.slice(offset, end), nextOffset: end < fields.length ? end : null };
    },
    getTorrentField: (p) => {
      const f = registry.find((f) => f.scope === p.scope && f.key === p.key.utf8);
      return { key: p.key, value: values[`${p.scope}:${p.key.utf8}`] ?? null, descriptor: f ?? null,
        editable: f?.editable ?? true, signed: config.signedTorrent ?? false };
    },
    previewTorrentEdit: (p) => {
      if (config.editorPreviewError) throw new Error(config.editorPreviewError);
      const infoChanged = p.info.length > 0;
      editorPreview = { token: `edit-${++editorCounter}`, torrentId: torrent.id, infoChanged, rawInfoPreserved: !infoChanged,
        oldHashes: { v1: "a".repeat(40), v2: "b".repeat(64) }, newHashes: { v1: (infoChanged ? "c" : "a").repeat(40), v2: (infoChanged ? "d" : "b").repeat(64) },
        signaturesRemoved: !!p.removeSignatures && infoChanged, outputChosen: false, requiresReplace: false };
      return editorPreview;
    },
    chooseEditorOutput: () => config.editorCancel ? { cancelled: true }
      : (editorPreview = { ...editorPreview, outputChosen: true, output: "C:\\data\\original.edited.torrent", requiresReplace: !!config.editorExisting }),
    saveTorrentEdit: () => {
      if (config.editorSaveError) throw new Error(config.editorSaveError);
      torrent = { ...torrent, id: "t-2", path: editorPreview.output, infohashV1: editorPreview.newHashes.v1, infohashV2: editorPreview.newHashes.v2 };
      editorPreview = null;
      return { torrent, guaranteeNote: "" };
    },
    getTorrentFiles: () => ({ total: 0, files: [] }),
    selectSources: () => {
      addSource(config.sourceName ?? "Holiday photos");
      return { draft: snapshotDraft() };
    },
    removeSource: (p) => {
      draft.sources = draft.sources.filter((s) => s.id !== p.sourceId);
      bump();
      return { draft: snapshotDraft() };
    },
    updateDraft: (p, message) => {
      if (message.draftRevision && message.draftRevision !== String(revision)) {
        throw Object.assign(new Error("The draft changed"), { code: "STALE_REVISION", retryable: true });
      }
      Object.assign(draft, p.patch);
      bump();
      return { draft: snapshotDraft() };
    },
    newDraft: () => {
      draft.sources = [];
      draft.name = "";
      bump();
      scan = { state: "empty", sourcesRevision: "0" };
      return { draft: snapshotDraft() };
    },
    validateDraft: () => {
      const ready = draft.sources.length > 0 && scan.state === "ready";
      return {
        draftRevision: String(revision),
        canCreate: ready,
        issues: ready ? [] : [{ code: "NO_SOURCES", severity: "error", message: "Add files or folders to share", sourceId: "" }],
        summary: ready ? { name: draft.effectiveName, mode: "directory", realFiles: config.entries, payloadBytes: "1048576", skipped: 0, unreadable: 0, pieceLength: 16384, pieceLengthAutomatic: true, pieceCount: "64", paddingBytes: "0", paddingFiles: 0, logicalBytes: "1048576", estimatedMetainfoBytes: "4096" } : {},
        review: [{ field: "profile", value: draft.profile }, { field: "format", value: draft.format }],
      };
    },
    getManifestPage: (p) => {
      const items = [];
      for (let i = p.offset; i < Math.min(config.entries, p.offset + p.limit); i++) items.push(entry(i));
      return { offset: p.offset, total: config.entries, entries: items, sourcesRevision: scan.sourcesRevision };
    },
    getSkippedPage: () => ({ offset: 0, total: 0, items: [] }),
    planProfile: (p) => ({ profile: profiles.find((x) => x.id === p.profileId), changes: [{ field: "profile", before: draft.profile, after: p.profileId, removesUserValue: false }, { field: "private", before: false, after: true, removesUserValue: false }], draftRevision: String(revision) }),
    applyProfile: (p) => {
      draft.profile = p.profileId;
      draft.private = p.profileId === "private";
      bump();
      return { draft: snapshotDraft() };
    },
    saveProfile: (p) => {
      if (config.saveProfileError) throw new Error(config.saveProfileError);
      const profile = { id: `custom-${profiles.length}`, name: p.name, builtin: false, format: draft.format };
      profiles.push(profile);
      draft.profile = profile.id;
      bump();
      return { profileId: profile.id, profiles, draft: snapshotDraft() };
    },
    deleteProfile: (p) => {
      const index = profiles.findIndex((profile) => profile.id === p.profileId);
      if (index >= 0) profiles.splice(index, 1);
      return { profiles, draft: snapshotDraft() };
    },
    exportMagnet: () => ({ magnet: "magnet:?xt=urn:btih:0123456789012345678901234567890123456789&dn=Example" }),
    updateSettings: (p) => {
      if (config.settingsSaveError) throw Object.assign(new Error(config.settingsSaveError), { code: "SETTINGS_WRITE_FAILED", retryable: true });
      return Object.assign(settings, p.patch);
    },
    startCreate: () => {
      const job = { id: "job-1", kind: "create", name: draft.effectiveName, batchId: "", state: "Queued", bytesDone: "0", bytesTotal: "1048576", bytesPerSecond: 0, currentFile: "", filesDone: 0, filesTotal: config.entries, log: [], version: "1", etaSeconds: null };
      jobs.push(job);
      emit("job", { job });
      return { jobId: job.id };
    },
  };

  function handle(text, attached) {
    const message = JSON.parse(text);
    requests.push({ operation: message.operation, payload: message.payload, attached: attached?.length ?? 0 });
    const reply = (body) => {
      const data = JSON.stringify({ protocolVersion: 1, requestId: message.requestId, ...body });
      setTimeout(() => listeners.forEach((l) => l({ data })), config.responseDelay?.(message) ?? 0);
    };
    const op = ops[message.operation];
    if (!op) return reply({ ok: false, error: { code: "UNKNOWN_OPERATION", message: message.operation, retryable: false } });
    try {
      reply({ ok: true, result: op(message.payload ?? {}, message) });
    } catch (err) {
      reply({ ok: false, error: { code: err.code ?? "INTERNAL", message: err.message, retryable: Boolean(err.retryable) } });
    }
  }

  window.chrome = window.chrome ?? {};
  window.chrome.webview = {
    addEventListener: (type, listener) => {
      if (type === "message") listeners.push(listener);
    },
    postMessage: (text) => handle(text, null),
    postMessageWithAdditionalObjects: (text, objects) => handle(text, objects),
  };
  window.__mock = { requests, emit, addSource, draft, ops, config };
})();
