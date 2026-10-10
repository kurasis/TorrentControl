// --self-test-flow only: real Windows common item dialogs, native jobs and a
// deliberate renderer crash. The native host owns all fixture paths.
import { request } from "./bridge.js";
import { confirmSelfTestAction, runSelfTest } from "./self-test.js";

function check(condition, message) {
  if (!condition) throw new Error(message);
}

async function until(predicate, description) {
  const deadline = performance.now() + 30000;
  while (!await predicate()) {
    if (performance.now() >= deadline) throw new Error(`Timed out: ${description}`);
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}

async function scan() {
  await until(async () => (await request("getSnapshot")).scan.state === "ready", "native manifest scan");
}

async function dialog(name, operation, payload = {}) {
  await request("selfTestStep", { name });
  const result = await request(operation, payload);
  check(!result.cancelled, `${operation} was unexpectedly cancelled`);
  return result;
}

async function job(state, id, expectSuccess = true) {
  await until(() => /^(Succeeded|SucceededWithWarnings|Failed|Cancelled)$/.test(state.jobs.get(id)?.state), `job ${id}`);
  const result = state.jobs.get(id);
  if (expectSuccess) check(result.state.startsWith("Succeeded"), `${id}: ${JSON.stringify(result.error)}`);
  return result;
}

export async function runNativeFlow(actions, state, info) {
  const native = await request("getSelfTestState");
  if (native.settingsOnly) {
    check(state.settings.language === "ru" && state.settings.theme === "dark"
      && state.settings.mode === "advanced", "Settings did not survive a process restart");
    check(document.documentElement.lang === "ru" && document.documentElement.dataset.theme === "dark", "Restart did not apply appearance settings");
    return { appVersion: info.appVersion, engineVersion: info.engineVersion, settingsRestart: true };
  }
  if (native.checkpoint) {
    const saved = native.checkpoint;
    check(native.rendererRecoveries === 1, "The renderer failure callback was not exercised exactly once");
    check(state.draft.revision === saved.revision, "Recovery changed the draft");
    check(JSON.stringify([...state.jobs.keys()].sort()) === JSON.stringify(saved.jobs), "Recovery replayed or lost jobs");
    check(state.settings.language === "ru" && state.settings.theme === "dark", "Recovery lost settings");
    if (saved.diagnosticId) {
      check(state.diagnosticRun?.id === saved.diagnosticId && state.diagnosticRun.state === "completed", "Recovery lost diagnostics");
      check(state.diagnosticPage?.rows.length === 2, "Recovery lost seed observations");
    }
    if (saved.editorToken) {
      check(state.torrent?.id === saved.torrentId, "Recovery lost the opened editor torrent");
      check(state.editorPreview?.token === saved.editorToken, "Recovery lost the native metadata preview");
      await until(() => document.getElementById("editor-preview-panel"), "restored editor preview");
    }
    await until(() => document.documentElement.lang === "ru" && document.documentElement.dataset.theme === "dark", "restored appearance");
    return { ...saved.evidence, rendererRecovery: true, recoveredJobs: state.jobs.size };
  }

  const evidence = await runSelfTest(actions, state, info);
  if (native.performanceFixture) {
    let frames = 0, maxGap = 0, last = performance.now(), stopped = false;
    const heartbeat = (now) => {
      if (stopped) return;
      maxGap = Math.max(maxGap, now - last);
      last = now;
      frames++;
      requestAnimationFrame(heartbeat);
    };
    requestAnimationFrame(heartbeat);
    let measurement;
    try {
      measurement = await request("runSelfTestResponsiveness");
    } finally {
      stopped = true;
    }
    check(measurement.files === 100000 && measurement.canCreate && measurement.filteredFiles === 1000,
      "Native responsiveness test did not process the complete real fixture");
    check(frames >= 2 && maxGap < 1000 && measurement.nativeTicks >= 2 && measurement.nativeMaxGapMs < 1000,
      `UI heartbeat stalled: ${JSON.stringify({ frames, maxGap, ...measurement })}`);
    evidence.responsiveness = { webViewFrames: frames, webViewMaxGapMs: maxGap, ...measurement };
    evidence.nativeResponsiveness = true;
    await actions.reloadSnapshot();
  }
  actions.edit("name", "Unsaved native draft");
  await until(() => state.draft.name === "Unsaved native draft" && !state.pendingFields.size, "typed draft acknowledgement");
  const cancelledReset = actions.newDraft();
  await until(() => document.getElementById("confirm-no"), "draft reset confirmation");
  document.getElementById("confirm-no").click();
  await cancelledReset;
  check((await request("getSnapshot")).draft.name === "Unsaved native draft", "Cancelling reset cleared the draft");
  evidence.draftResetConfirmation = true;
  await confirmSelfTestAction(() => actions.newDraft());
  const revision = state.draft.revision;
  await request("selfTestStep", { name: "cancel" });
  check((await request("selectSources", { kind: "files" })).cancelled, "Dialog cancel did not reach the bridge");
  check((await request("getSnapshot")).draft.revision === revision, "Cancel mutated the draft");
  await request("selfTestStep", { name: "file" });
  await actions.selectSources("files");
  check(state.draft.sources[0]?.name === "данные #1.bin", "The frontend did not apply the selected file");
  let snapshot = await request("getSnapshot");
  check(snapshot.draft.sources.length === 1 && snapshot.draft.sources[0].name === "данные #1.bin", "Unicode file selection failed");
  await confirmSelfTestAction(() => actions.newDraft());
  await request("selfTestStep", { name: "folder" });
  await actions.selectSources("folder");
  check(state.draft.sources[0]?.name === "набор данных", "The frontend did not apply the selected folder");
  await scan();
  check((await request("getManifestPage")).total === 2, "Folder selection did not scan both files");
  await request("updateDraft", { patch: { trackers: [], creationDate: "omit" } });

  const created = [];
  for (const format of ["v1", "v2", "hybrid"]) {
    await request("updateDraft", { patch: { format } });
    await dialog(format, "chooseOutput");
    check((await request("validateDraft")).canCreate, `Preflight blocked ${format}`);
    const started = await request("startCreate");
    const finished = await job(state, started.jobId);
    check(finished.result.format === format && finished.result.realFiles === 2 && finished.result.payloadBytes === "89994", `Wrong ${format} result`);
    const opened = (await dialog(format, "openTorrent")).torrent;
    check(opened.problems.length === 0 && opened.format === format, `Invalid ${format} metainfo`);
    for (const key of ["infohashV1", "infohashV2"])
      check((opened[key] ?? "") === (finished.result[key] ?? ""), `Changed ${key}`);
    if (format === "v1") {
      const wrong = await dialog("wrong-payload", "verifyPayload", { torrentId: opened.id });
      const failed = await job(state, wrong.jobId, false);
      check(failed.state === "Failed" && failed.error?.code === "PAYLOAD_MISMATCH"
        && failed.verify.files.length === 2 && failed.verify.files.every((file) => file.status === "missing"),
      "Verification accepted a folder with missing payload files");
    }
    const verify = await dialog("payload", "verifyPayload", { torrentId: opened.id });
    check((await job(state, verify.jobId)).verify.ok, `${format} payload mismatch`);
    const report = await request("getVerifyFilesPage", { jobId: verify.jobId, offset: 0, limit: 250, errorsOnly: false });
    check(report.total === 2 && report.rows.length === 2 && report.rows.every((file) => file.status === "ok"),
      `${format} paged native verification lost files`);
    const detail = await request("getVerifyFile", { jobId: verify.jobId, index: report.rows[1].index });
    check(detail.file.path === report.rows[1].path && detail.file.status === "ok", "Native verification detail changed its file");
    created.push(finished);
  }
  await dialog("project", "saveProject");
  await confirmSelfTestAction(() => actions.newDraft());
  const project = await dialog("project", "openProject");
  check(project.draft.pieceLength === created[2].result.pieceLength, "Project lost resolved piece size");
  await scan();
  await dialog("reopened", "chooseOutput");
  const recreated = await job(state, (await request("startCreate")).jobId);
  check(recreated.result.infohashV1 === created[2].result.infohashV1 && recreated.result.infohashV2 === created[2].result.infohashV2, "Project changed infohashes");
  const magnet = (await request("exportMagnet", { id: recreated.id })).magnet;
  await dialog("magnet", "saveMagnet", { id: recreated.id });
  const disk = await request("checkSelfTestOutput");
  check(disk.identical && disk.magnet === `${magnet}\n`, "Project bytes or exported magnet changed on disk");

  await request("prepareSelfTestDiagnostics");
  await actions.reloadSnapshot();
  await actions.updateSettings({ mode: "advanced" });
  actions.setTab("trackers");
  await until(() => document.getElementById("diagnostics-check-trackers"), "tracker diagnostics control");
  document.getElementById("diagnostics-check-trackers").click();
  await until(() => state.diagnosticRun?.state === "completed" && state.diagnosticPage?.rows.length === 1, "HTTP tracker probe");
  check(state.diagnosticPage.rows[0].state === "protocol-responding", "Zero-peer tracker response was rejected");
  check(!JSON.stringify(state.diagnosticPage).includes("fixture-secret"), "Diagnostics exported a URL secret");
  const trackerRun = state.diagnosticRun.id;
  actions.setTab("webSeeds");
  await until(() => document.getElementById("diagnostics-check-web-seeds"), "seed diagnostics control");
  document.getElementById("diagnostics-check-web-seeds").click();
  await until(() => state.diagnosticRun?.id !== trackerRun && state.diagnosticRun?.state === "completed"
    && state.diagnosticPage?.rows.length === 2, "sampled seed probes");
  check(state.diagnosticPage.rows.every((row) => row.state === "range-supported" && row.integrity === "not-verified"), "Seed range checks failed");
  Object.assign(evidence, { networkDiagnostics: true, seedSamplesVerified: 2 });

  // Use the actual HTML controls and Save As dialogs for metadata edits.
  await request("selfTestStep", { name: "hybrid" });
  await actions.openTorrent();
  const originalTorrent = state.torrent;
  const control = (id) => {
    const element = document.getElementById(id);
    check(element, `Missing editor control ${id}`);
    return element;
  };
  const change = (id, value, event = "change") => {
    const element = control(id);
    element.value = value;
    element.dispatchEvent(new Event(event, { bubbles: true }));
  };
  const saveEditor = async (name) => {
    await until(() => document.getElementById("editor-preview")?.disabled === false, "metadata preview button");
    control("editor-preview").click();
    await until(() => state.editorPreview && !state.editor.busy, "metadata preview");
    const before = state.torrent.id;
    await request("selfTestStep", { name });
    // State is updated before the scheduled animation-frame render. A fast
    // native acknowledgement does not guarantee the Save As control exists.
    await until(() => document.getElementById("editor-output")?.disabled === false, "metadata Save As button");
    control("editor-output").click();
    await until(() => state.editorPreview.outputChosen && !state.editor.busy, "metadata destination");
    await until(() => document.getElementById("editor-save")?.disabled === false, "metadata save button");
    control("editor-save").click();
    await until(() => state.torrent.id !== before && state.editor?.torrentId === state.torrent.id, "saved metadata reopen");
    await until(() => document.getElementById("metadata-editor")?.dataset.torrentId === state.torrent.id, "saved editor controls");
  };
  await until(() => document.getElementById("editor-add-known"), "metadata editor");
  change("editor-add-known", "comment");
  await until(() => state.editor.selection?.descriptor?.key === "comment" && !state.editor.busy, "comment field");
  await until(() => document.getElementById("editor-value"), "comment input");
  change("editor-value", "Нативная проверка редактора", "input");
  await saveEditor("outer-edited");
  check(state.torrent.infohashV1 === originalTorrent.infohashV1 && state.torrent.infohashV2 === originalTorrent.infohashV2,
    "Outer edit changed the torrent identifiers");
  change("editor-scope", "info");
  await until(() => state.editor.scope === "info" && !state.editor.busy, "info dictionary");
  await until(() => document.getElementById("metadata-editor")?.dataset.scope === "info", "info controls");
  change("editor-add-known", "source");
  await until(() => state.editor.selection?.descriptor?.key === "source" && !state.editor.busy, "source field");
  await until(() => document.getElementById("editor-value"), "source input");
  change("editor-value", "native-m3", "input");
  await saveEditor("info-edited");
  check(state.torrent.infohashV1 !== originalTorrent.infohashV1 && state.torrent.infohashV2 !== originalTorrent.infohashV2,
    "Info edit did not change both hybrid identifiers");
  const editingDisk = await request("checkSelfTestEdits");
  check(editingDisk.rawInfoPreserved && editingDisk.payloadHashesPreserved && editingDisk.commentPreserved
    && editingDisk.sourceEdited, "Saved metadata bytes failed independent native inspection");
  // Leave one reviewed candidate unsaved, then recover it from the native
  // snapshot following a real renderer failure. Do not replay a save.
  const pending = await request("previewTorrentEdit", { torrentId: state.torrent.id,
    outer: [{ key: { t: "str", utf8: "comment" }, value: { t: "str", utf8: "Pending after recovery" } }], info: [] });

  await actions.updateSettings({ language: "ru", theme: "dark", mode: "advanced" });
  snapshot = await request("getSnapshot");
  Object.assign(evidence, { nativeDialogs: true, dialogCancel: true, unicodePaths: true,
    formats: ["v1", "v2", "hybrid"], verified: 3, missingPayloadRejected: true,
    projectIdentical: true, magnetSaved: true, metadataEditor: true, verificationPaging: true,
    outerEditPreserved: true, infoEditChanged: true, editedOutputsVerified: 2 });
  // Store the checkpoint natively before crashing; a recovered page must get
  // its state from getSnapshot and must not repeat any earlier operation.
  await request("crashSelfTestRenderer", { revision: snapshot.draft.revision,
    jobs: snapshot.jobs.map((j) => j.id).sort(), torrentId: state.torrent.id, editorToken: pending.token,
    diagnosticId: state.diagnosticRun.id, evidence });
  return null;
}
