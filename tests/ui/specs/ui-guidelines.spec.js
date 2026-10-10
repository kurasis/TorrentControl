const { test, expect } = require("./fixture");

async function advanced(page) {
  await page.goto("/index.html");
  await page.locator('#mode-switch [data-mode="advanced"]').click();
  await page.locator("#tab-general").click();
}
async function editor(page) {
  await page.goto("/index.html");
  await page.locator("#btn-open-torrent").click();
  await expect(page.locator("#metadata-editor")).toBeVisible();
  await page.locator("#editor-add-known").selectOption("comment");
  await expect(page.locator("#editor-value")).toHaveValue("Original comment");
}
async function requests(page, operation) {
  return page.evaluate((op) => window.__mock.requests.filter((r) => r.operation === op), operation);
}

test("profile deletion names the profile and defaults to cancellation", async ({ page }) => {
  await advanced(page);
  await page.locator("#save-profile").click();
  await page.locator("#profile-name").fill("My saved profile");
  await page.locator("#profile-name-save").click();
  await expect(page.locator("#dialog")).not.toBeVisible();
  await page.locator("#delete-profile").click();
  await expect(page.locator("#dialog")).toContainText("My saved profile");
  await expect(page.locator("#confirm-no")).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(page.locator("#dialog")).not.toBeVisible();
  expect(await requests(page, "deleteProfile")).toHaveLength(0);
  await expect(page.locator("#draft-profile")).toHaveValue("custom-2");
  await page.locator("#delete-profile").click();
  await page.locator("#dialog").getByRole("button", { name: "Delete profile", exact: true }).click();
  await expect.poll(async () => (await requests(page, "deleteProfile")).length).toBe(1);
  await expect(page.locator('#draft-profile option[value="custom-2"]')).toHaveCount(0);
});

test("reset cancellation preserves a freshly typed draft; explicit reset clears it", async ({ page }) => {
  await page.goto("/index.html");
  await page.locator("#draft-name").fill("Keep my draft");
  await page.locator("#btn-new").click();
  await expect(page.locator("#confirm-no")).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(page.locator("#dialog")).not.toBeVisible();
  await expect(page.locator("#draft-name")).toHaveValue("Keep my draft");
  expect(await requests(page, "newDraft")).toHaveLength(0);
  await page.locator("#btn-new").click();
  await page.getByRole("button", { name: "Reset draft", exact: true }).click();
  await expect(page.locator("#draft-name")).toHaveValue("");
  await expect.poll(async () => (await requests(page, "newDraft")).length).toBe(1);
});

test.describe("pending draft text during confirmation", () => {
  test.use({ mockConfig: 'window.__mockConfig = { responseDelay: (m) => m.operation === "updateDraft" ? 1000 : 0 };' });
  test("a background render keeps typed text while modal focus is elsewhere", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#issues")).toContainText("Add files or folders");
    await page.locator("#draft-name").fill("Unacknowledged draft text");
    await page.locator("#btn-new").click();
    await expect(page.locator("#confirm-no")).toBeFocused();
    const value = await page.evaluate(async () => {
      const { state, invalidate } = await import("/app.js");
      state.validation.draftRevision = "background-validation";
      invalidate("workspace");
      await new Promise(requestAnimationFrame);
      return document.getElementById("draft-name").value;
    });
    expect(value).toBe("Unacknowledged draft text");
    await expect(page.locator("#confirm-no")).toBeFocused();
    await page.keyboard.press("Escape");
    await expect(page.locator("#dialog")).not.toBeVisible();
    await expect(page.locator("#draft-name")).toHaveValue("Unacknowledged draft text");
    expect(await requests(page, "newDraft")).toHaveLength(0);
  });
});

for (const phase of ["buffer", "staged", "preview"]) {
  test(`opening another torrent protects unsaved ${phase}`, async ({ page }) => {
    await editor(page);
    await page.locator("#editor-value").fill("Keep these edits");
    if (phase === "staged") {
      await page.locator("#editor-add-known").selectOption("httpseeds");
      await expect(page.locator("#editor-pending")).toContainText("1");
    }
    if (phase === "preview") {
      await page.locator("#editor-preview").click();
      await expect(page.locator("#editor-preview-panel")).toBeVisible();
    }
    await page.locator("#btn-open-torrent").click();
    await expect(page.locator("#confirm-no")).toBeFocused();
    expect(await requests(page, "openTorrent")).toHaveLength(1);
    await page.keyboard.press("Escape");
    await expect(page.locator("#dialog")).not.toBeVisible();
    if (phase === "buffer") await expect(page.locator("#editor-value")).toHaveValue("Keep these edits");
    if (phase === "preview") await expect(page.locator("#editor-preview-panel")).toBeVisible();
    if (phase === "staged") await expect(page.locator("#editor-pending")).toContainText("1");
    await page.locator("#btn-open-torrent").click();
    await page.getByRole("button", { name: "Discard changes", exact: true }).click();
    await expect.poll(async () => (await requests(page, "openTorrent")).length).toBe(2);
    await expect(page.locator("#editor-preview-panel")).toHaveCount(0);
    await expect(page.locator("#editor-value")).toHaveCount(0);
    expect(await requests(page, "saveTorrentEdit")).toHaveLength(0);
  });
}

test("reset also protects an unsaved torrent preview", async ({ page }) => {
  await editor(page);
  await page.locator("#editor-value").fill("Keep this preview");
  await page.locator("#editor-preview").click();
  await expect(page.locator("#editor-preview-panel")).toBeVisible();
  await page.locator("#btn-new").click();
  await page.locator("#confirm-no").click();
  await expect(page.locator("#dialog")).not.toBeVisible();
  await expect(page.locator("#editor-preview-panel")).toBeVisible();
  expect(await requests(page, "newDraft")).toHaveLength(0);
  await page.locator("#btn-new").click();
  await page.locator("#confirm-yes").click();
  await expect(page.locator("#metadata-editor")).toHaveCount(0);
  await expect.poll(async () => (await requests(page, "newDraft")).length).toBe(1);
});

test("opening a job result uses the same editor guard", async ({ page }) => {
  await editor(page);
  await page.locator("#editor-value").fill("Keep my buffer");
  await page.evaluate(async () => {
    const { actions } = await import("/app.js");
    void actions.openJobResult("job-1");
  });
  await expect(page.locator("#confirm-no")).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(page.locator("#dialog")).not.toBeVisible();
  expect(await requests(page, "openJobResult")).toHaveLength(0);
  await expect(page.locator("#editor-value")).toHaveValue("Keep my buffer");
});

test.describe("asynchronous field selection", () => {
  test.use({ mockConfig: 'window.__mockConfig = { responseDelay: (m) => m.operation === "getTorrentField" ? 150 : 0 };' });
  test("keyboard field selection focuses the value after the busy render", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#btn-open-torrent").click();
    const field = page.locator(".editor-fields button", { hasText: /^comment$/ });
    await field.focus();
    await page.keyboard.press("Enter");
    await expect(page.locator("#editor-value")).toHaveValue("Original comment");
    await expect(page.locator("#editor-value")).toBeFocused();
    await page.keyboard.type("! typed after selection");
    await expect(page.locator("#editor-value")).toHaveValue("Original comment! typed after selection");
  });
  test("late field responses do not steal focus from the toolbar", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#btn-open-torrent").click();
    await page.locator(".editor-fields button", { hasText: /^comment$/ }).click();
    await page.locator("#theme-select").focus();
    await expect(page.locator("#editor-value")).toHaveValue("Original comment");
    await expect(page.locator("#theme-select")).toBeFocused();
  });
});

test("technical editor controls have names and disable spellchecking", async ({ page }) => {
  await editor(page);
  await expect(page.locator("#editor-add-known")).toHaveAccessibleName("Add or edit a known field…");
  await expect(page.locator("#editor-extension-key")).toHaveAccessibleName("Extension field key");
  await page.locator("#editor-add-known").selectOption("announce-list");
  await expect(page.locator("textarea#editor-value")).toBeVisible();
  expect(await page.locator("#editor-value").evaluate((input) => input.spellcheck)).toBe(false);
});

test("fixed date is labelled, UTC and editable without changing the date policy", async ({ page }) => {
  await advanced(page);
  const date = page.locator("#draft-fixed-date");
  await expect(date).toHaveAccessibleName("Fixed creation date (UTC)");
  await page.locator("#draft-date").selectOption("fixed");
  await expect(date).toBeEnabled();
  await date.fill("2026-10-10T12:34:56");
  await expect.poll(() => page.evaluate(() => window.__mock.draft.fixedDate)).toBe(1791635696);
  await expect(page.locator("#draft-date")).toHaveValue("fixed");
});

test.describe("paged batch", () => {
  test.use({ mockConfig: 'window.__mockConfig = { batchCount: 100 };' });
test("batch row policies identify the task and modal scroll is contained", async ({ page }) => {
  await advanced(page);
  await page.locator("#add-folder").click();
  await page.locator("#btn-batch").click();
  await expect(page.locator("#batch-policy-item-0")).toHaveAccessibleName("When a file exists: Batch-0");
  await expect(page.locator("#batch-policy-item-1")).toHaveAccessibleName("When a file exists: Batch-1");
  for (const selector of ["#dialog", ".dialog-body"]) {
    expect(await page.locator(selector).evaluate((el) => getComputedStyle(el).overscrollBehaviorY)).toBe("contain");
  }
});

});
test.describe("table batch", () => {
  test.use({ mockConfig: 'window.__mockConfig = { batchCount: 10 };' });
test("batch row policies identify the task and modal scroll is contained", async ({ page }) => {
  await advanced(page);
  await page.locator("#add-folder").click();
  await page.locator("#btn-batch").click();
  await expect(page.locator("#batch-policy-item-0")).toHaveAccessibleName("When a file exists: Batch-0");
  await expect(page.locator("#batch-policy-item-1")).toHaveAccessibleName("When a file exists: Batch-1");
  for (const selector of ["#dialog", ".dialog-body"]) {
    expect(await page.locator(selector).evaluate((el) => getComputedStyle(el).overscrollBehaviorY)).toBe("contain");
  }
});

});

test("profile previews wrap long values without horizontal overflow", async ({ page }) => {
  await page.goto("/index.html");
  await page.evaluate(async () => {
    const { showProfileChange } = await import("/dialogs.js");
    showProfileChange({ profile: { name: "Long values" }, changes: [
      { field: "comment", before: "x".repeat(1000), after: "y".repeat(1000), removesUserValue: true },
    ] }, () => {}, () => {}, {});
  });
  for (const width of [1200, 600]) {
    await page.setViewportSize({ width, height: 800 });
    await expect(page.locator("#profile-changes")).toContainText("x".repeat(1000));
    const dimensions = await page.locator(".dialog-body").evaluate((el) => ({ client: el.clientWidth, scroll: el.scrollWidth }));
    expect(dimensions.scroll).toBeLessThanOrEqual(dimensions.client + 1);
  }
});

for (const theme of ["light", "dark"]) {
  test(`error text has WCAG AA contrast in ${theme} theme`, async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#theme-select").selectOption(theme);
    await expect(page.locator("html")).toHaveAttribute("data-theme", theme);
    const ratios = await page.evaluate(async () => {
      const { toast } = await import("/dom.js");
      toast("Example failure", { error: true });
      document.getElementById("unavailable").hidden = false;
      function luminance(rgb) {
        const v = rgb.match(/\d+/g).slice(0, 3).map(Number).map((c) => {
          const s = c / 255;
          return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
        });
        return v[0] * 0.2126 + v[1] * 0.7152 + v[2] * 0.0722;
      }
      return ["unavailable", "toast"].map((id) => {
        const css = getComputedStyle(document.getElementById(id));
        const a = luminance(css.color), b = luminance(css.backgroundColor);
        return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
      });
    });
    for (const ratio of ratios) expect(ratio).toBeGreaterThanOrEqual(4.5);
  });
}

test.describe("large manifest reading order", () => {
  test.use({ mockConfig: 'window.__mockConfig = { entries: 100000 };' });
  test("scrolling forward then backward preserves DOM order and absolute positions", async ({ page }) => {
    await advanced(page);
    await page.locator("#tab-files").click();
    await page.locator("#add-folder").click();
    await expect(page.locator("#btn-create")).toBeEnabled();
    const list = page.locator("#manifest-list");
    await expect(list.locator(".vlist-row").first()).toContainText("file-0.bin");
    await list.evaluate((el) => { el.scrollTop = 2800; });
    await expect(list.locator('.vlist-row[data-index="100"]')).toBeVisible();
    await list.evaluate((el) => { el.scrollTop = 2660; });
    await expect(list.locator('.vlist-row[data-index="85"]')).toHaveCount(1);
    const rows = await list.locator(".vlist-row").evaluateAll((nodes) => nodes.map((el) => ({
      index: Number(el.dataset.index), position: Number(el.getAttribute("aria-posinset")), size: Number(el.getAttribute("aria-setsize")),
    })));
    expect(rows.map((row) => row.index)).toEqual(rows.map((row) => row.index).sort((a, b) => a - b));
    for (const row of rows) {
      expect(row.position).toBe(row.index + 1);
      expect(row.size).toBe(100000);
    }
    expect(rows.length).toBeLessThan(100);
  });
});

test("size formatting follows the selected UI language and keeps exact BigInt bytes", async ({ page }) => {
  await page.goto("/index.html");
  await page.locator("#language-select").selectOption("ru");
  await expect(page.locator("html")).toHaveAttribute("lang", "ru");
  const sizes = await page.evaluate(async () => {
    const { formatBytes, exactBytes } = await import("/format.js");
    const bytes = "900719925474099312345";
    return { short: formatBytes(1234567), exact: exactBytes(bytes), expected: new Intl.NumberFormat("ru").format(BigInt(bytes)) + " B" };
  });
  expect(sizes.short).toBe("1,18 MiB");
  expect(sizes.exact).toBe(sizes.expected);
  await expect(page.locator("#skip-to-workspace")).toHaveAccessibleName("Перейти к рабочей области");
});

test("skip link is first in keyboard order and focuses main without navigating", async ({ page }) => {
  await page.goto("/index.html");
  await expect(page.locator("#add-files")).toBeVisible();
  await page.keyboard.press("Tab");
  await expect(page.locator("#skip-to-workspace")).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(page.locator("#workspace")).toBeFocused();
  expect(new URL(page.url()).hash).toBe("");
  await page.keyboard.press("Tab");
  await expect(page.locator("#add-files")).toBeFocused();
  await expect(page.locator("#draft-name")).toHaveAttribute("name", "name");
  await expect(page.locator("#draft-name")).toHaveAttribute("autocomplete", "off");
});
