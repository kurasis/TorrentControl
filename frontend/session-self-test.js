// Developer-only bulk orchestration of the real AppService/WebView2 session.
import { request } from "./bridge.js";
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const check = (ok, message) => { if (!ok) throw new Error(message); };

export async function runSessionFlow(actions, state) {
  let frames = 0, maxGap = 0, last = performance.now(), stopped = false;
  function frame(now) {
    ++frames; maxGap = Math.max(maxGap, now - last); last = now;
    if (!stopped) requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
  await actions.updateSettings({ mode: "advanced" });
  const checkpoints = [], cycles = [];
  async function checkpoint(phase) {
    await delay(100);
    const renderer = { jobs: state.jobs.size, rows: document.querySelectorAll("#job-list > li").length,
      watched: state.watchResult.size, batches: state.batchJobs.size, elements: document.getElementsByTagName("*").length };
    check(renderer.jobs <= 51 && renderer.rows <= 50, "Renderer history exceeded its page budget");
    const value = await request("sessionCheckpoint", { phase, renderer });
    checkpoints.push(value); await delay(300); return value;
  }
  try {
    await checkpoint("idle");
    for (const format of ["v1", "v2", "hybrid"]) {
      await request("sessionPrepare", { format });
      await actions.reloadSnapshot();
      await checkpoint(`${format}-warm`);
      const began = performance.now();
      for (let wave = 0; wave < 10; ++wave) {
        await checkpoint(`${format}-wave-${wave}`);
        await request("sessionEnqueue", { kind: ["create", "verify", "batch"][wave % 3], count: 100 });
        await request("sessionJoin");
        await actions.latestJobsPage();
      }
      const result = await request("sessionFinish");
      result.elapsedMs = performance.now() - began;
      await actions.firstJobsPage();
      check(state.jobHistory.ids.length === 50 && state.jobHistory.offset === 0, "First history page missing");
      const pinned = state.jobHistory.ids[0]; actions.selectJob(pinned);
      actions.setTab("jobs");
      await actions.loadJobsPage(450);
      check(state.jobHistory.offset === 450 && state.jobs.has(pinned), "Off-page selection was lost");
      const verifiedId = state.jobHistory.ids[0];
      const verifiedFiles = await actions.verifyFilesPage(verifiedId, 0, 50, false);
      check(verifiedFiles?.total === 32 && verifiedFiles.rows.length === 32, "Native verification detail page missing");
      const layout = await actions.jobLayoutPage(pinned, 0, 50);
      check(layout?.total >= 32, "Native creation layout page missing");
      await actions.nextJobsPage();
      check(state.jobHistory.offset === 500, "Next history cursor was lost");
      await actions.previousJobsPage();
      check(state.jobHistory.offset === 450, "Previous history cursor was lost");
      await actions.latestJobsPage();
      check(state.jobHistory.offset === 950 && state.jobHistory.ids.length === 50, "Latest history page missing");
      const completed = await checkpoint(`${format}-completed`);
      check(completed.jobs === 1000 && completed.createSpecs === 0 && completed.verifySpecs === 0, "Completed input retention");
      await actions.clearFinished(); await actions.reloadSnapshot();
      const cleared = await checkpoint(`${format}-cleared`);
      for (const key of ["jobs", "createSpecs", "verifySpecs", "batches", "appBatches", "appBatchJobIds", "running"])
        check(cleared[key] === 0, `Clear retained native ${key}`);
      check(cleared.archivedBatches <= 64, "Aggregate report archive is unbounded");
      check(cleared.renderer.jobs === 0 && cleared.renderer.rows === 0 && cleared.renderer.watched === 0
        && cleared.renderer.batches === 0 && state.selectedJob === null, "Clear retained renderer history");
      cycles.push(result);
    }
    const final = checkpoints.at(-1);
    check(frames > 10 && maxGap < 2000 && final.nativeTicks > 10 && final.nativeMaxGapMs < 2000,
      `Session heartbeat stalled: ${JSON.stringify({ frames, maxGap, final })}`);
    return { jobs: 3000, cycles, checkpoints, historyCleared: true,
      responsiveness: { frames, maxGapMs: maxGap, nativeTicks: final.nativeTicks, nativeMaxGapMs: final.nativeMaxGapMs } };
  } finally { stopped = true; }
}
