const { test, expect } = require("./fixture");

async function trackers(page) {
  await page.goto("/index.html");
  await page.locator('#mode-switch [data-mode="advanced"]').click();
  await page.locator("#tab-trackers").click();
}

test("opening a torrent never starts probes; an explicit check uses the native torrent ID", async ({ page }) => {
  await page.goto("/index.html");
  await page.locator("#btn-open-torrent").click();
  await expect(page.locator("#diagnostics-panel")).toBeVisible();
  expect(await page.evaluate(() => window.__mock.requests.some((r) => /^(startDiagnostics|updateTrackerCatalog)$/.test(r.operation)))).toBe(false);
  await page.locator("#diagnostics-check-trackers").click();
  await expect(page.locator("#diagnostics-results")).toContainText("Protocol responding");
  expect(await page.evaluate(() => window.__mock.requests.find((r) => r.operation === "startDiagnostics").payload)).toMatchObject({ torrentId: "t-1", kind: "trackers", networkMode: "direct", httpProxy: "" });
  await expect(page.locator("#diagnostics-panel")).toContainText("do not establish acceptance");
});

test.describe("explicit proxy and cancellation", () => {
  test.use({ mockConfig: 'window.__mockConfig = { diagnosticBusy: true };' });
  test("empty proxy stays an error; configured proxy is retained through cancellation", async ({ page }) => {
    await trackers(page);
    await page.locator("#diagnostics-network").selectOption("http-proxy");
    await page.locator("#diagnostics-check-trackers").click();
    await expect(page.locator("#diagnostics-error")).toContainText("INVALID_PROXY");
    await page.locator("#diagnostics-proxy").fill("http://proxy.example:8080");
    await page.locator("#diagnostics-check-trackers").click();
    await expect(page.locator("#diagnostics-check-trackers")).toBeDisabled();
    await page.locator("#diagnostics-cancel").click();
    await expect(page.locator("#diagnostics-progress")).toContainText("Cancelled");
    await expect(page.locator("#diagnostics-network")).toHaveValue("http-proxy");
    expect(await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "startDiagnostics").at(-1).payload.httpProxy)).toBe("http://proxy.example:8080");
  });
});

test.describe("native snapshot recovery", () => {
  test.use({ mockConfig: 'window.__mockConfig = { settings: { mode: "advanced" }, diagnosticRun: { id: "diagnostic-12", sequence: "12", state: "completed", completed: 1, total: 1, network: "http-proxy" } };' });
  test("restores results and connection mode without replaying requests", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#tab-trackers").click();
    await expect(page.locator("#diagnostics-results")).toContainText("Protocol responding");
    await expect(page.locator("#diagnostics-network")).toHaveValue("http-proxy");
    expect(await page.evaluate(() => window.__mock.requests.some((r) => r.operation === "startDiagnostics"))).toBe(false);
    await page.evaluate(() => window.__mock.emit("diagnostics", { run: { id: "diagnostic-9", sequence: "9", state: "running", completed: 0, total: 1, network: "direct" } }));
    await expect(page.locator("#diagnostics-progress")).toContainText("Completed");
  });
});

test("catalog changes require a visible review before applying", async ({ page }) => {
  await trackers(page);
  await page.locator("#catalog-review").click();
  await expect(page.locator("dialog[open]")).toContainText("udp://new.example:80/announce");
  expect(await page.evaluate(() => window.__mock.requests.some((r) => r.operation === "applyTrackerCatalog"))).toBe(false);
  await page.locator("#confirm-yes").click();
  await expect.poll(() => page.evaluate(() => window.__mock.requests.find((r) => r.operation === "applyTrackerCatalog")?.payload.checksum)).toBe("a".repeat(64));
});
