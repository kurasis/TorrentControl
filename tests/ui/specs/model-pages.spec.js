const { test, expect } = require("./fixture");

test.describe("large source collection", () => {
  test.use({ mockConfig: `window.__mockConfig = { modelPaging: true, modelPageSize: 7, sourcesCount: 100000, settings: { mode: "advanced" } };` });
  test("reaches the last source with bounded DOM and opens unloaded source options", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#source-list-rows")).toContainText("Source-6");
    await page.locator("#source-list-next").click();
    await expect(page.locator("#source-list-rows")).toContainText("Source-7");
    await page.locator("#source-list-previous").click();
    await expect(page.locator("#source-list-rows")).toContainText("Source-0");
    await page.locator("#source-list-last").click();
    await page.getByRole("button", { name: "Source-99999", exact: true }).click();
    await expect(page.locator("#source-recursive")).toBeVisible();
    expect(await page.locator("#source-list-rows .collection-row").count()).toBeLessThanOrEqual(50);
    const reads = await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "getModelPage"));
    expect(reads.some((r) => r.payload.offset === 7)).toBe(true);
    expect(reads.some((r) => r.payload.key === "source" && r.payload.owner === "source-99999")).toBe(true);
    expect(reads.filter((r) => r.payload.key === "sources").length).toBeLessThanOrEqual(8);
  });
});

test.describe("native row editing", () => {
  test.use({ mockConfig: `window.__mockConfig = { modelPaging: true, modelPageSize: 7, sourcesCount: 1, trackersCount: 1000, webSeedsCount: 1000, exclusionsCount: 1000, settings: { mode: "advanced" } };` });
  test("editing last tracker and seed retains the first unloaded row", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#tab-trackers").click();
    await page.locator("#paged-trackers-last").click();
    await page.locator("#paged-trackers-999").fill("https://replacement.example/");
    await page.locator("#paged-trackers-999").press("Tab");
    await expect.poll(() => page.evaluate(() => window.__mock.draft.trackers[999].url)).toBe("https://replacement.example/");
    expect(await page.evaluate(() => window.__mock.draft.trackers[0].url)).toBe("https://tracker-0.example/announce");
    expect(await page.evaluate(() => window.__mock.draft.trackers.length)).toBe(1000);
    await page.locator("#tab-webSeeds").click();
    await page.locator("#paged-webSeeds-last").click();
    await page.locator("#paged-webSeeds-999").fill("https://new-seed.example/");
    await page.locator("#paged-webSeeds-999").press("Tab");
    await expect.poll(() => page.evaluate(() => window.__mock.draft.webSeeds[999])).toBe("https://new-seed.example/");
    expect(await page.evaluate(() => window.__mock.draft.webSeeds[0])).toBe("https://seed-0.example/");
    const patches = await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "updateDraft"));
    expect(patches.some((r) => "trackers" in r.payload.patch || "webSeeds" in r.payload.patch)).toBe(false);
  });
  test("source switches preserve unseen exclusion patterns", async ({ page }) => {
    await page.goto("/index.html");
    await page.getByRole("button", { name: "Source-0", exact: true }).click();
    await page.locator("#paged-exclusions-last").click();
    await page.locator("#paged-exclusions-999").fill("changed-pattern");
    // A real background scan update must not detach an uncommitted row input.
    await page.evaluate(() => window.__mock.emit("scan", { state: "scanning", sourcesRevision: "0" }));
    await expect.poll(() => page.evaluate(async () => { const { state } = await import("/app.js"); return state.scan.state; })).toBe("scanning");
    await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve))));
    await expect(page.locator("#paged-exclusions-999")).toBeFocused();
    await expect(page.locator("#paged-exclusions-999")).toHaveValue("changed-pattern");
    await page.locator("#paged-exclusions-999").press("Tab");
    await expect.poll(() => page.evaluate(() => window.__mock.draft.sources[0].exclusions[999])).toBe("changed-pattern");
    await expect.poll(() => page.evaluate(async () => { const { state } = await import("/app.js"); return state.draft.revision; })).toBe("2");
    await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve))));
    await page.locator("#source-recursive").uncheck();
    // A validation redraw must preserve the user's pending checkbox choice.
    await page.evaluate(async () => { const { invalidate } = await import("/app.js"); window.__mock.emit("scan", { state: "ready", sourcesRevision: "0" }); invalidate(); });
    await expect(page.locator("#source-recursive")).not.toBeChecked();
    await page.locator("#apply-source-options").click();
    await expect.poll(() => page.evaluate(() => window.__mock.draft.sources[0].recursive)).toBe(false);
    expect(await page.evaluate(() => window.__mock.draft.sources[0].exclusions.length)).toBe(1000);
    expect(await page.evaluate(() => window.__mock.draft.sources[0].exclusions[0])).toBe("pattern-0");
  });
});

test.describe("large profile picker and batch", () => {
  test.use({ mockConfig: `window.__mockConfig = { modelPaging: true, modelPageSize: 7, sourcesCount: 1, profilesCount: 1000, batchCount: 1000, settings: { mode: "advanced" } };` });
  test("selects a profile beyond the summary without constructing all options", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#tab-general").click();
    await page.locator("#browse-profiles").click();
    await page.locator("#model-page-last").click();
    await expect(page.locator("#model-page-rows")).toContainText("Profile-999");
    await page.locator("#model-choose-1001").click();
    await page.locator("#profile-apply").click();
    await expect(page.locator("#draft-profile")).toHaveValue("custom-999");
    expect(await page.locator("#draft-profile option").count()).toBeLessThanOrEqual(51);
    await page.evaluate(async () => { const { actions } = await import("/app.js"); await actions.pickProfile("custom-50"); });
    await page.locator("#profile-apply").click();
    await expect(page.locator("#draft-profile")).toHaveValue("custom-50");
    expect(await page.locator("#draft-profile option").count()).toBeLessThanOrEqual(51);
  });
  test("batch pages keep exclusions made on separate pages", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#btn-batch").click();
    await page.locator("#batch-include-item-0").uncheck();
    await expect(page.locator("#batch-start")).toContainText("999");
    await page.locator("#batch-items-last").click();
    await page.locator("#batch-include-item-999").uncheck();
    await expect(page.locator("#batch-start")).toContainText("998");
    expect(await page.locator("#batch-items-rows .collection-row").count()).toBeLessThanOrEqual(50);
    const changes = await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "updateBatch"));
    expect(changes).toHaveLength(2);
    expect(Object.keys(changes[1].payload.overrides)).toEqual(["item-999"]);
    await page.locator("#batch-start").click();
    await expect(page.locator("#dialog")).not.toBeVisible();
    await expect(page.locator("#toast")).toContainText("998");
    const operations = await page.evaluate(() => window.__mock.requests);
    expect(operations.some((r) => r.operation === "getBatchStatus")).toBe(true);
    expect(operations.some((r) => r.operation === "getModelPage" && r.payload.model === "batchJobs")).toBe(false);
  });
});

test.describe("oversized legacy tracker", () => {
  test.use({ mockConfig: `window.__mockConfig = { modelPaging: true, truncatedTracker: true, settings: { mode: "advanced" } };` });
  test("cannot save a clipped preview and offers explicit replacement", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#tab-trackers").click();
    await expect(page.locator("#paged-trackers-0")).toBeDisabled();
    await page.locator("#paged-full-trackers-0").click();
    await expect(page.locator("#model-text-value")).toContainText("https://legacy.example/");
    expect(await page.evaluate(() => window.__mock.draft.trackers[0].url.length)).toBeGreaterThan(100000);
    await page.keyboard.press("Escape");
    await page.locator("#paged-replace-trackers-0").fill("https://replacement.example/");
    await page.locator("#paged-apply-trackers-0").click();
    await expect.poll(() => page.evaluate(() => window.__mock.draft.trackers[0].url)).toBe("https://replacement.example/");
  });
});

test.describe("profile save acknowledgement", () => {
  test.use({ mockConfig: `window.__mockConfig = { failSnapshotAfterStart: true, settings: { mode: "advanced" } };` });
  test("a later snapshot read failure cannot turn a successful save into a retry", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#tab-general").click();
    await page.locator("#save-profile").click();
    await page.locator("#profile-name").fill("Saved once");
    await page.locator("#profile-name-save").click();
    await expect(page.locator("#dialog")).not.toBeVisible();
    await expect(page.locator("#draft-profile")).toHaveValue("custom-2");
    const operations = await page.evaluate(() => window.__mock.requests);
    expect(operations.filter((r) => r.operation === "saveProfile")).toHaveLength(1);
    expect(operations.filter((r) => r.operation === "getSnapshot")).toHaveLength(1);
  });
});

test.describe("paging failures", () => {
  test.use({ mockConfig: `window.__mockConfig = { modelPaging: true, sourcesCount: 100 };` });
  test("shows a failed read and retries without replaying a mutation", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#source-list-rows")).toContainText("Source-0");
    await expect.poll(() => page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "validateDraft").length)).toBeGreaterThan(0);
    await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve))));
    await page.evaluate(() => { window.__mock.config.failModelPageOnce = true; });
    await page.locator("#source-list-next").click();
    await expect(page.locator("#source-list [role=alert]")).toContainText("IO_ERROR");
    await page.locator("#source-list-retry").click();
    await expect(page.locator("#source-list-rows")).toContainText("Source-50");
    expect(await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "editDraftRow").length)).toBe(0);
  });
});

test.describe("large imported overview", () => {
  test.use({ mockConfig: `
    const trackers = Array.from({ length: 123 }, (_, i) => ({ url: '<img src=x onerror="window.__xss=1">/tracker-' + i, tier: 0 }));
    const comment = 'first-' + 'x'.repeat(20000) + '-last';
    window.__mockConfig = { settings: { mode: "advanced" }, modelPageSize: 7,
      torrent: { id: "t-1", name: "Payload", format: "hybrid", path: "C:/payload.torrent", problems: [],
        trackers: trackers.slice(0, 3), trackersTotal: trackers.length, webSeeds: [], realFiles: 0, paddingFiles: 0,
        payloadBytes: "0", comment: "first-…", textFields: { comment: comment.length } },
      torrentCollections: { trackers }, torrentFull: { trackers, comment }
    };` });
  test("reads the last tracker and complete long text as text without markup", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#torrent-all-trackers").click();
    await page.locator("#model-page-last").click();
    await expect(page.locator("#model-page-rows")).toContainText("tracker-122");
    await page.locator("#model-page-rows").getByRole("button", { name: "url", exact: true }).click();
    await expect(page.locator("#model-text-value")).toContainText('<img src=x onerror="window.__xss=1">/tracker-122');
    expect(await page.locator("#dialog img").count()).toBe(0);
    expect(await page.evaluate(() => window.__xss)).toBeUndefined();
    await page.keyboard.press("Escape");
    await page.locator("#torrent-full-comment").click();
    await expect(page.locator("#model-text-value")).toContainText("first-");
    await page.locator("#model-text-next").click();
    await page.locator("#model-text-next").click();
    await expect(page.locator("#model-text-value")).toContainText("-last");
    await expect(page.locator("#model-text-next")).toBeDisabled();
    const reads = await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "getModelText" && r.payload.key === "comment"));
    expect(reads.map((r) => r.payload.offset)).toEqual([0, 8192, 16384]);
  });
});
