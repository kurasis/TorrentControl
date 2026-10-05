const { test, expect } = require("./fixture");

async function open(page) {
  await page.goto("/index.html");
  await page.locator("#btn-open-torrent").click();
  await expect(page.locator("#metadata-editor")).toBeVisible();
}
async function comment(page, text = "New comment") {
  await page.locator("#editor-add-known").selectOption("comment");
  await expect(page.locator("#editor-value")).toHaveValue("Original comment");
  await page.locator("#editor-value").fill(text);
  await page.locator("#editor-preview").click();
  await expect(page.locator("#editor-preview-panel")).toContainText("identifiers are preserved");
}

test("outer preview requires Save As and preserves input identifiers", async ({ page }) => {
  await open(page);
  await comment(page, '<img src=x onerror="window.__xss=1">');
  expect(await page.locator("#editor-save").count()).toBe(0);
  await page.locator("#editor-output").click();
  await page.locator("#editor-save").click();
  await expect(page.locator("#torrent-info")).toContainText("original.edited.torrent");
  const preview = await page.evaluate(() => window.__mock.requests.find((r) => r.operation === "previewTorrentEdit").payload);
  expect(preview.outer[0].value.utf8).toContain("<img");
  expect(preview.info).toEqual([]);
  expect(await page.locator("#workspace img").count()).toBe(0);
  expect(await page.evaluate(() => window.__xss)).toBeUndefined();
});

test.describe("explicit replacement and retry", () => {
  test.use({ mockConfig: 'window.__mockConfig = { editorExisting: true, editorSaveError: "Disk is full" };' });
  test("overwrite stays disabled until checked and failure retains the preview", async ({ page }) => {
    await open(page);
    await comment(page);
    await page.locator("#editor-output").click();
    await expect(page.locator("#editor-save")).toBeDisabled();
    await page.locator("#editor-replace").check();
    await page.locator("#editor-save").click();
    await expect(page.locator("#editor-error")).toContainText("Disk is full");
    await expect(page.locator("#editor-save")).toBeEnabled();
    await page.evaluate(() => { window.__mock.config.editorSaveError = ""; });
    await page.locator("#editor-save").click();
    await expect(page.locator("#torrent-info")).toContainText("original.edited.torrent");
  });
});

test("binary extensions keep integer text and protected fields stay read only", async ({ page }) => {
  await open(page);
  await page.locator("#editor-extension-key").fill("0x00ff");
  await page.locator("#editor-add-extension").click();
  await expect(page.locator("#editor-value")).toBeVisible();
  await page.locator("#editor-value").fill('{"t":"int","v":"900719925474099312345"}');
  await page.locator("#editor-preview").click();
  await expect(page.locator("#editor-preview-panel")).toBeVisible();
  const preview = await page.evaluate(() => window.__mock.requests.find((r) => r.operation === "previewTorrentEdit").payload);
  expect(preview.outer[0]).toEqual({ key: { t: "bytes", hex: "00ff" }, value: { t: "int", v: "900719925474099312345" } });
  await page.locator("#editor-scope").selectOption("info");
  await page.locator(".editor-fields button", { hasText: "pieces" }).click();
  await expect(page.locator("#editor-value")).toHaveAttribute("readonly", "");
  await expect(page.locator("#editor-remove")).toBeDisabled();
});

test.describe("signature decision", () => {
  test.use({ mockConfig: 'window.__mockConfig = { signedTorrent: true };' });
  test("info preview shows old and new hashes and explicit signature removal", async ({ page }) => {
    await open(page);
    await page.locator("#editor-scope").selectOption("info");
    await page.locator("#editor-add-known").selectOption("source");
    await expect(page.locator("#editor-value")).toHaveValue("old");
    await page.locator("#editor-value").fill("new source");
    await page.locator("#editor-remove-signatures").check();
    await page.locator("#editor-preview").click();
    await expect(page.locator("#editor-preview-panel")).toContainText("new torrent identifiers");
    await expect(page.locator("#editor-preview-panel")).toContainText("omit the invalidated signatures");
    await expect(page.locator("#editor-preview-panel")).toContainText("a".repeat(40));
    await expect(page.locator("#editor-preview-panel")).toContainText("c".repeat(40));
    const preview = await page.evaluate(() => window.__mock.requests.find((r) => r.operation === "previewTorrentEdit").payload);
    expect(preview.removeSignatures).toBe(true);
    expect(preview.info[0].value.utf8).toBe("new source");
  });
});

test.describe("cancel", () => {
  test.use({ mockConfig: 'window.__mockConfig = { editorCancel: true };' });
  test("canceling Save As leaves the reviewed preview intact", async ({ page }) => {
    await open(page);
    await comment(page);
    await page.locator("#editor-output").click();
    await expect(page.locator("#editor-preview-panel")).toBeVisible();
    expect(await page.locator("#editor-save").count()).toBe(0);
    expect(await page.evaluate(() => window.__mock.requests.some((r) => r.operation === "saveTorrentEdit"))).toBe(false);
  });
});

test("tracker tiers and BEP 17 seeds are staged as distinct ordered lists", async ({ page }) => {
  await open(page);
  await page.locator("#editor-add-known").selectOption("announce-list");
  await expect(page.locator("#editor-value")).toBeVisible();
  await page.locator("#editor-value").fill("https://one.example/announce\nudp://two.example:80\n\nhttps://three.example/announce");
  await page.locator("#editor-add-known").selectOption("httpseeds");
  await expect(page.locator("#editor-value")).toHaveValue("");
  await page.locator("#editor-value").fill("https://seed.example/bep17");
  await page.locator("#editor-preview").click();
  await expect(page.locator("#editor-preview-panel")).toBeVisible();
  const p = await page.evaluate(() => window.__mock.requests.find((r) => r.operation === "previewTorrentEdit").payload);
  expect(p.outer[0].key.utf8).toBe("announce-list");
  expect(p.outer[0].value.items.map((tier) => tier.items.length)).toEqual([2, 1]);
  expect(p.outer[1].key.utf8).toBe("httpseeds");
  expect(p.outer[1].value.items[0].utf8).toBe("https://seed.example/bep17");
});

test.describe("reviewed candidate recovery", () => {
  test.use({ mockConfig: `window.__mockConfig = {
    settings: { mode: "advanced" },
    torrent: { id: "t-9", name: "Recovered", path: "original.torrent", format: "v1", problems: [], payloadBytes: "100", realFiles: 1 },
    editorPreview: { token: "edit-9", torrentId: "t-9", infoChanged: false, rawInfoPreserved: true,
      oldHashes: { v1: "oldhash" }, newHashes: { v1: "oldhash" }, outputChosen: false,
      changes: [{ scope: "top", key: { t: "str", utf8: "comment" }, before: { t: "str", utf8: "before crash" }, after: { t: "str", utf8: "reviewed pending change" } }] }
  };` });
  test("snapshot restores the reviewed field diff without replaying save", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#editor-preview-panel")).toBeVisible();
    await page.locator("#editor-preview-panel summary").click();
    await expect(page.locator("#editor-preview-panel")).toContainText("reviewed pending change");
    expect(await page.evaluate(() => window.__mock.requests.some((r) => /^(openTorrent|previewTorrentEdit|saveTorrentEdit)$/.test(r.operation)))).toBe(false);
    await page.locator("#editor-output").click();
    await expect(page.locator("#editor-save")).toBeVisible();
    expect(await page.evaluate(() => window.__mock.requests.find((r) => r.operation === "chooseEditorOutput").payload.token)).toBe("edit-9");
  });
});
