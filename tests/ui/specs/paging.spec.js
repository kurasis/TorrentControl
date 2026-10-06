const { test, expect } = require("./fixture");

test.describe("job snapshot pagination", () => {
  test.use({ mockConfig: `window.__mockConfig = { settings: { mode: "advanced" }, pagedJobs: true, jobs: Array.from({ length: 73 }, (_, i) => ({
    id: "job-" + (i + 1), name: "Recovered-" + (i + 1), kind: "verify", state: "Failed", version: "1",
    bytesDone: "0", bytesTotal: "0", filesDone: 0, filesTotal: 0, log: Array.from({ length: 45 }, (_, line) => "Log-" + line),
  })) };` });
  test("a complete snapshot releases native-cleared job history", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#job-list")).toContainText("Recovered-73");
    await page.evaluate(async () => {
      const { request } = await import("/bridge.js");
      await request("clearFinishedJobs");
      const { actions } = await import("/app.js");
      await actions.reloadSnapshot();
    });
    await expect.poll(() => page.evaluate(async () => { const { state } = await import("/app.js"); return state.jobs.size; })).toBe(0);
    await expect(page.locator("#jobs-panel")).not.toContainText("Recovered-73");
  });
  test("snapshot reconciliation retains a new job event received during the read", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#job-list")).toContainText("Recovered-73");
    await page.evaluate(async () => {
      const { request } = await import("/bridge.js");
      await request("clearFinishedJobs");
      const original = window.__mock.ops.getSnapshot;
      window.__mock.ops.getSnapshot = () => {
        const snapshot = original();
        window.__mock.emit("job", { job: { id: "job-new", name: "New during snapshot", kind: "create", state: "Queued",
          version: "1", bytesDone: "0", bytesTotal: "0", filesDone: 0, filesTotal: 0, log: [] } });
        return snapshot;
      };
      const { actions } = await import("/app.js");
      await actions.reloadSnapshot();
    });
    await expect(page.locator("#job-list")).toContainText("New during snapshot");
    await expect.poll(() => page.evaluate(async () => { const { state } = await import("/app.js"); return [...state.jobs.keys()]; })).toEqual(["job-new"]);
  });
  test("recovers jobs beyond the first page without replaying mutations", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#job-list")).toContainText("Recovered-73");
    await page.locator("#details-job-73").click();
    await expect(page.locator("#job-log")).toContainText("Log-19");
    await page.locator("#job-log-next").click();
    await expect(page.locator("#job-log")).toContainText("Log-39");
    await page.locator("#job-log-next").click();
    await expect(page.locator("#job-log")).toContainText("Log-44");
    const operations = await page.evaluate(() => window.__mock.requests.map((r) => r.operation));
    expect(operations.filter((op) => op === "getJobsPage")).toHaveLength(1);
    expect(operations.filter((op) => ["startCreate", "startBatch", "verifyPayload"].includes(op))).toHaveLength(0);
    await page.evaluate(() => window.__mock.emit("resyncRequired", { reason: "EVENT_TOO_LARGE" }));
    await expect.poll(() => page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "getSnapshot").length)).toBe(2);
    await expect(page.locator("#job-list")).toContainText("Recovered-73");
  });
});

test.describe("large native verification report", () => {
  test.use({ mockConfig: `window.__mockConfig = {
    settings: { mode: "advanced" }, verifyFiles: 100000, verifyErrors: 100000,
    verifyDetailPath: '<img src=x onerror="window.__xss=1">/full-original-path',
    jobs: [{ id: "job-1", name: "Payload check", kind: "verify", state: "Failed", version: "2",
      bytesDone: "0", bytesTotal: "100000", filesDone: 100000, filesTotal: 100000, log: [],
      verify: { ok: false, files: [], filesTotal: 100000, filesTruncated: true } }],
  };` });
  test("pages to the last error, shows original detail as text and can include successful files", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#details-job-1").click();
    const list = page.locator("#verify-files");
    await expect(list.locator(".vlist-row").first()).toContainText("file-0.bin");
    await list.focus();
    await page.keyboard.press("End");
    const last = list.locator(".vlist-row", { hasText: "file-99999.bin" });
    await expect(last).toBeVisible();
    expect(await list.locator(".vlist-row").count()).toBeLessThan(100);
    await last.locator("button").click();
    await expect(page.locator("#verify-file-path")).toHaveText('<img src=x onerror="window.__xss=1">/full-original-path');
    expect(await page.locator("#dialog img").count()).toBe(0);
    expect(await page.evaluate(() => window.__xss)).toBeUndefined();
    await page.keyboard.press("Escape");
    await page.locator("#verify-errors-only").uncheck();
    await expect(list.locator(".vlist-row").first()).toContainText("file-0.bin: ok");
    const reads = await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "getVerifyFilesPage"));
    expect(reads.some((r) => r.payload.offset === 99750 && r.payload.errorsOnly)).toBe(true);
    expect(reads.some((r) => !r.payload.errorsOnly)).toBe(true);
  });
});

test("visiting many virtual pages bounds cached data and reloads evicted pages", async ({ page }) => {
  await page.goto("/index.html");
  const result = await page.evaluate(async () => {
    const { VirtualList } = await import("/virtual-list.js");
    const container = document.createElement("div");
    container.style.height = "300px";
    document.body.append(container);
    const reads = new Map();
    const list = new VirtualList(container, {
      fetchPage: async (offset, limit) => {
        reads.set(offset, (reads.get(offset) ?? 0) + 1);
        return { total: 100000, items: Array.from({ length: limit }, (_, i) => ({ name: `File-${offset + i}` })) };
      },
      renderRow: (item) => { const row = document.createElement("div"); row.textContent = item.name; return row; },
    });
    await list.reset();
    for (let i = 1; i < 30; ++i) await list.load(i);
    const before = reads.get(250);
    await list.load(1);
    const result = { cachedRows: [...list.pages.values()].reduce((sum, rows) => sum + rows.length, 0),
      domRows: container.querySelectorAll(".vlist-row").length, reloaded: reads.get(250) > before };
    container.remove(); list.render();
    return result;
  });
  expect(result.cachedRows).toBeLessThanOrEqual(2000);
  expect(result.domRows).toBeLessThan(100);
  expect(result.reloaded).toBe(true);
});

test.describe("creation warnings remain accessible beyond the compact preview", () => {
  test.use({ mockConfig: `window.__mockConfig = {
    warningLines: Array.from({ length: 45 }, (_, i) => "Warning-" + i),
    jobs: [{ id: "job-1", name: "Created", kind: "create", state: "SucceededWithWarnings", version: "2",
      bytesDone: "1", bytesTotal: "1", filesDone: 1, filesTotal: 1, log: [], result: {
        name: "Created", output: "C:\\\\out.torrent", format: "v1", payloadBytes: "1", realFiles: 1,
        pieceLength: 16384, numPieces: 1, infohashV1: "0123456789012345678901234567890123456789", layout: [],
        warnings: Array.from({ length: 5 }, (_, i) => "Warning-" + i), warningsTotal: 45,
      } }],
  };` });
  test("pages through every retained warning", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#details-job-1").click();
    await expect(page.locator("#result-warning-rows")).toContainText("Warning-19");
    await page.locator("#result-warning-next").click();
    await expect(page.locator("#result-warning-rows")).toContainText("Warning-39");
    await page.locator("#result-warning-next").click();
    await expect(page.locator("#result-warning-rows")).toContainText("Warning-44");
    await expect(page.locator("#result-warning-next")).toBeDisabled();
  });
});

test.describe("metadata pages use native byte-budget cursors", () => {
  test.use({ mockConfig: `window.__mockConfig = {
    settings: { mode: "advanced" }, fieldPageSize: 7,
    torrent: { id: "t-1", name: "Paged", format: "hybrid", problems: [], realFiles: 1, payloadBytes: "1" },
    fieldRows: Array.from({ length: 23 }, (_, i) => ({ key: { t: "str", utf8: "extension-" + i },
      value: { t: "str", utf8: "Native" }, editable: true, descriptor: null })),
  };` });
  test("next and previous visit actual cursor boundaries without skipping fields", async ({ page }) => {
    await page.goto("/index.html");
    const fields = page.locator(".editor-fields");
    await expect(fields).toContainText("extension-6");
    const next = page.getByRole("button", { name: "Next fields", exact: true });
    const previous = page.getByRole("button", { name: "Previous fields", exact: true });
    await next.click(); await expect(fields).toContainText("extension-13");
    await next.click(); await expect(fields).toContainText("extension-20");
    await previous.click(); await expect(fields).toContainText("extension-7");
    await previous.click(); await expect(fields).toContainText("extension-0");
    const offsets = await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "getTorrentFields").map((r) => r.payload.offset));
    expect(offsets).toEqual([0, 7, 14, 7, 0]);
  });
});
