// Developer-only real WebView2 workflow. Fixture paths are native-owned;
// ordinary snapshot, create, progress and clear operations update the UI.
import { request } from "./bridge.js";

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function checkpoint(phase) {
  await request("memorySelfTestCheckpoint", { phase });
}

export async function runMemoryFlow(actions, state) {
  await checkpoint("idle");
  await delay(300);
  await checkpoint("scan");
  const fixture = await request("prepareSelfTestMemory");
  await actions.reloadSnapshot();
  if (state.scan.state !== "ready") throw new Error("Memory fixture was not scanned");
  await delay(300);
  await checkpoint("review");
  const review = await request("validateDraft");
  if (!review.canCreate) throw new Error("Memory fixture was rejected by preflight");
  await delay(300);
  await checkpoint("create");
  const { jobId } = await request("startCreate");
  const deadline = performance.now() + 480000;
  while (!/^(Succeeded|SucceededWithWarnings|Failed|Cancelled)$/.test(state.jobs.get(jobId)?.state)) {
    if (performance.now() >= deadline) throw new Error("Memory creation did not terminate");
    await delay(50);
  }
  const result = state.jobs.get(jobId);
  if (!result.state.startsWith("Succeeded")) throw new Error(JSON.stringify(result.error));
  await request("joinSelfTestMemory");
  await actions.reloadSnapshot();
  await delay(100);
  await checkpoint("completed");
  await delay(300);
  await request("clearFinishedJobs");
  await actions.reloadSnapshot();
  if (state.jobs.size !== 0) throw new Error("Clearing job history retained jobs in the renderer");
  await checkpoint("cleared");
  await delay(300);
  return { ...fixture, jobState: result.state, historyCleared: true, domElements: document.getElementsByTagName("*").length };
}
