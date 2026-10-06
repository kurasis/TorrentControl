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
