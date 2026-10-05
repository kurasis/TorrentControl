// --self-test-flow only: real Windows common item dialogs, native jobs and a
// deliberate renderer crash. The native host owns all fixture paths.
import { request } from "./bridge.js";
import { runSelfTest } from "./self-test.js";

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

async function job(state, id) {
  await until(() => /^(Succeeded|SucceededWithWarnings|Failed|Cancelled)$/.test(state.jobs.get(id)?.state), `job ${id}`);
  const result = state.jobs.get(id);
  check(result.state.startsWith("Succeeded"), `${id}: ${JSON.stringify(result.error)}`);
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
    await until(() => document.documentElement.lang === "ru" && document.documentElement.dataset.theme === "dark", "restored appearance");
    return { ...saved.evidence, rendererRecovery: true, recoveredJobs: state.jobs.size };
  }

  const evidence = await runSelfTest(actions, state, info);
  await actions.newDraft();
  const revision = state.draft.revision;
  await request("selfTestStep", { name: "cancel" });
  check((await request("selectSources", { kind: "files" })).cancelled, "Dialog cancel did not reach the bridge");
  check((await request("getSnapshot")).draft.revision === revision, "Cancel mutated the draft");
  await dialog("file", "selectSources", { kind: "files" });
  let snapshot = await request("getSnapshot");
  check(snapshot.draft.sources.length === 1 && snapshot.draft.sources[0].name === "данные #1.bin", "Unicode file selection failed");
  await actions.newDraft();
  await dialog("folder", "selectSources", { kind: "folder" });
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
    const verify = await dialog("payload", "verifyPayload", { torrentId: opened.id });
    check((await job(state, verify.jobId)).verify.ok, `${format} payload mismatch`);
    created.push(finished);
  }
  await dialog("project", "saveProject");
  await actions.newDraft();
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

  await actions.updateSettings({ language: "ru", theme: "dark", mode: "advanced" });
  snapshot = await request("getSnapshot");
  Object.assign(evidence, { nativeDialogs: true, dialogCancel: true, unicodePaths: true,
    formats: ["v1", "v2", "hybrid"], verified: 3, projectIdentical: true, magnetSaved: true });
  // Store the checkpoint natively before crashing; a recovered page must get
  // its state from getSnapshot and must not repeat any earlier operation.
  await request("crashSelfTestRenderer", { revision: snapshot.draft.revision,
    jobs: snapshot.jobs.map((j) => j.id).sort(), evidence });
  return null;
}
