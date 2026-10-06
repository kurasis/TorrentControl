const { test, expect } = require("./fixture");

test.describe("bounded long-session history", () => {
  test.use({ mockConfig: `window.__mockConfig = { pagedJobs: true, settings: { mode: "advanced" }, jobs: Array.from({ length: 3000 }, (_, i) => ({
    id: "job-" + (i + 1), name: "History-" + (i + 1), kind: "verify", state: "Succeeded", version: "2",
    bytesDone: "1", bytesTotal: "1", filesDone: 1, filesTotal: 1, log: ["Completed"]
  })) };` });
  test("recovers and browses thousands of jobs with one page and one pinned selection", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#job-list > li")).toHaveCount(50);
    await expect(page.locator("#job-list")).toContainText("History-3000");
    const recovered = await page.evaluate(async () => {
      const { state, actions } = await import("/app.js");
      state.watchResult.add("job-1");
      await actions.reloadSnapshot();
      return [state.watchResult.has("job-1"), state.jobs.size];
    });
    expect(recovered).toEqual([false, 50]);
    await page.locator("#details-job-3000").click();
    await page.locator("#jobs-first").click();
    await expect(page.locator("#job-list")).toContainText("History-1");
    await expect(page.locator("#job-list")).not.toContainText("History-3000");
    await expect(page.locator("#workspace")).toContainText("History-3000");
    expect(await page.evaluate(async () => { const { state } = await import("/app.js"); return state.jobs.size; })).toBe(51);
    await page.locator("#jobs-next").click();
    await expect(page.locator("#job-list")).toContainText("History-51");
    await page.locator("#jobs-previous").click();
    await expect(page.locator("#job-list")).toContainText("History-1");
    await page.locator("#jobs-latest").click();
    await expect(page.locator("#job-list")).toContainText("History-3000");
    await page.locator("#clear-finished").click();
    await expect(page.locator("#job-list > li")).toHaveCount(0);
    expect(await page.evaluate(async () => { const { state } = await import("/app.js"); return [state.jobs.size, state.selectedJob]; })).toEqual([0, null]);
  });
  test("events remain bounded and off-page completions still resolve watched results", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#job-list > li")).toHaveCount(50);
    const bounded = await page.evaluate(async () => {
      const { state } = await import("/app.js");
      let peak = 0;
      let measuring = true;
      window.chrome.webview.addEventListener("message", () => { if (measuring) peak = Math.max(peak, state.jobs.size); });
      for (let i = 3001; i <= 6000; ++i) {
        window.__mock.emit("job", { job: { id: `job-${i}`, name: `Event-${i}`, kind: "create", state: "Queued", version: "1", bytesDone: "0", bytesTotal: "1" } });
      }
      await new Promise((resolve) => setTimeout(resolve, 0));
      measuring = false;
      return peak;
    });
    expect(bounded).toBeLessThanOrEqual(51);
    expect(bounded).toBeGreaterThanOrEqual(50);
    await page.locator("#jobs-first").click();
    await expect(page.locator("#job-list")).toContainText("History-1");
    const completed = await page.evaluate(async () => {
      const { state } = await import("/app.js");
      state.watchResult.add("off-page");
      window.__mock.emit("job", { job: { id: "off-page", name: "Off page completion", kind: "create", state: "Failed", version: "2", bytesDone: "0", bytesTotal: "1" } });
      await new Promise((resolve) => setTimeout(resolve, 0));
      return [state.watchResult.has("off-page"), state.jobs.has("off-page"), state.jobs.size];
    });
    expect(completed).toEqual([false, false, 51]);
  });
  test("Latest includes the final row when native byte budgets shorten pages", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#job-list")).toContainText("History-3000");
    await page.evaluate(async () => {
      window.__mock.config.jobPageLimit = 17;
      const { actions } = await import("/app.js");
      await actions.reloadSnapshot();
    });
    await expect(page.locator("#job-list > li")).toHaveCount(1);
    await expect(page.locator("#job-list")).toContainText("History-3000");
    await page.locator("#jobs-first").click();
    await expect(page.locator("#job-list > li")).toHaveCount(17);
    await expect(page.locator("#job-list")).toContainText("History-17");
    await page.locator("#jobs-next").click();
    await expect(page.locator("#job-list")).toContainText("History-18");
    await page.locator("#jobs-previous").click();
    await expect(page.locator("#job-list")).toContainText("History-1");
    await page.locator("#jobs-latest").click();
    await expect(page.locator("#job-list > li")).toHaveCount(1);
    await expect(page.locator("#job-list")).toContainText("History-3000");
  });
  test("awaited user navigation completes before the background refresh resumes", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#job-list")).toContainText("History-3000");
    const offset = await page.evaluate(async () => {
      const { actions, state } = await import("/app.js");
      window.__mock.config.responseDelay = (message) => message.operation === "getJobsPage" ? 100 : 0;
      window.__mock.emit("job", { job: { id: "new-during-navigation", name: "Queued during navigation", kind: "create", state: "Queued", version: "1", bytesDone: "0", bytesTotal: "1", log: [] } });
      await new Promise((resolve) => setTimeout(resolve, 0));
      await actions.firstJobsPage();
      return state.jobHistory.offset;
    });
    expect(offset).toBe(0);
    await expect(page.locator("#job-list")).toContainText("History-1");
  });
});

test.describe("long-session developer driver contract", () => {
  test.use({ mockConfig: `window.__mockConfig = { jobs: [], pagedJobs: true, verifyFiles: 32, verifyErrors: 0 };` });
  test("runs all cycles through the production controller's page adapters", async ({ page }) => {
    test.setTimeout(45000);
    await page.goto("/index.html");
    await expect(page.locator("#jobs-heading")).toBeVisible();
    const result = await page.evaluate(async () => {
      const { actions, state } = await import("/app.js");
      const { runSessionFlow } = await import("/session-self-test.js");
      const { ops, config, emit } = window.__mock;
      const jobs = config.jobs;
      let format = "v1", id = 0, ticks = 0;
      const layout = Array.from({ length: 32 }, (_, index) => ({ index, torrentPath: `Collection/file-${index}.bin`, local: `file-${index}.bin` }));
      const stats = () => ({ jobs: jobs.length, succeeded: jobs.length,
        created: jobs.filter((job) => job.kind === "create").length,
        verified: jobs.filter((job) => job.kind === "verify").length,
        createSpecs: 0, verifySpecs: 0, inputManifestEntries: 0, verifyInputBytes: 0, jobThreads: 0,
        workers: 1, batches: 0, appBatches: 0, appBatchJobIds: 0, archivedBatches: 64, running: 0, failed: 0, cancelled: 0 });
      ops.sessionPrepare = (p) => { format = p.format; return { files: 32, format }; };
      ops.sessionEnqueue = (p) => {
        for (let i = 0; i < p.count; ++i) {
          const job = { id: `session-${++id}`, name: `Session-${id}`, kind: p.kind === "verify" ? "verify" : "create",
            state: "Succeeded", version: "1", bytesDone: "32", bytesTotal: "32", filesDone: 32, filesTotal: 32, log: ["Completed"] };
          if (job.kind === "verify") job.verify = { ok: true, files: [], filesTotal: 32 };
          else job.result = { output: "session.torrent", format, layout: layout.slice(0, 5), layoutTotal: 32, layoutTruncated: true, warnings: [] };
          jobs.push(job); emit("job", { job });
        }
        return { enqueued: p.count, kind: p.kind };
      };
      ops.sessionJoin = () => ({ joined: true });
      ops.sessionFinish = () => ({ ...stats(), format });
      ops.sessionCheckpoint = (p) => ({ ...stats(), ...p, nativeTicks: ticks += 10, nativeMaxGapMs: 100 });
      ops.getJobLayoutPage = (p) => ({ rows: layout.slice(p.offset, p.offset + p.limit), total: 32, realFiles: 32, complete: true });
      const evidence = await runSessionFlow(actions, state);
      return { jobs: evidence.jobs, formats: evidence.cycles.map((cycle) => cycle.format),
        cleared: evidence.historyCleared, cached: state.jobs.size, native: jobs.length };
    });
    expect(result).toEqual({ jobs: 3000, formats: ["v1", "v2", "hybrid"], cleared: true, cached: 0, native: 0 });
  });
});
