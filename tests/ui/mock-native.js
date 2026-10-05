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
  let scan = { state: "empty", sourcesRevision: "0" };
  const settings = Object.assign({ theme: "system", language: "en", mode: "simple", openClientWithoutAsking: false }, config.settings);
  const jobs = [];

  function snapshotDraft() {
    return { ...draft, revision: String(revision), scan, outputAuto: true, canUndo: false };
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
    getSelfTestState: () => ({ checkpoint: config.nativeCheckpoint ?? null,
      rendererRecoveries: config.rendererRecoveries ?? 0, settingsOnly: config.settingsOnly ?? false }),
    reportSelfTest: () => ({}),
    checkSelfTestProfile: (p) => ({ persisted: config.selfTestPersistence !== false
      && profiles.some((profile) => profile.id === p.profileId && profile.name === p.name) }),
    getEngineInfo: () => ({ appVersion: "0.0.0-test", engineVersion: "libtorrent 2.1.2", protocolVersion: 1 }),
    getSnapshot: () => ({ draft: snapshotDraft(), scan, jobs, settings, profiles }),
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
      return { profiles };
    },
    exportMagnet: () => ({ magnet: "magnet:?xt=urn:btih:0123456789012345678901234567890123456789&dn=Example" }),
    updateSettings: (p) => Object.assign(settings, p.patch),
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
      setTimeout(() => listeners.forEach((l) => l({ data })), 0);
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
  window.__mock = { requests, emit, addSource, draft, ops };
})();
