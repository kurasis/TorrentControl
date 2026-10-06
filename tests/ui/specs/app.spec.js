// Specification scenarios U01, U02 and U06 plus the startup self-test path.
const { test, expect } = require("./fixture");

test.describe("startup", () => {
  test("loads, reports engine info and renders the simple mode", async ({ page }) => {
    await page.goto("/index.html");
    await expect(page.locator("#add-files")).toBeVisible();
    await expect(page.locator("#unavailable")).toBeHidden();
    await expect(page.locator("#btn-create")).toBeDisabled();
    await expect(page.locator("#issues")).toContainText("Add files or folders");
  });

  test("self-test reports success to the host", async ({ page }) => {
    await page.addInitScript({ content: "window.__mockConfig = { selfTest: true };" });
    await page.goto("/index.html?selfTest=1");
    await expect.poll(() => page.evaluate(() => window.__mock.requests.find((r) => r.operation === "reportSelfTest")?.payload))
      .toMatchObject({ ok: true, engineVersion: "libtorrent 2.1.2", profiles: 2,
        profileDialog: true, profilePersisted: true, magnetDialog: true });
  });
});

test.describe("U01 hostile names render as text", () => {
  const hostile = '<img src=x onerror="window.__xss=1">';
  test.use({
    mockConfig: `window.__mockConfig = { entries: 5, sourceName: ${JSON.stringify(hostile)}, entryName: (i) => ${JSON.stringify(hostile)} + i + "<script>window.__xss=2</script>" };`,
  });

  test("source, review and file names never become markup", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#add-folder").click();
    await expect(page.locator("#source-list")).toContainText(hostile);
    await page.locator('#mode-switch [data-mode="advanced"]').click();
    await expect(page.locator("#manifest-list .vlist-row").first()).toContainText("<script>");
    expect(await page.locator("img").count()).toBe(0);
    expect(await page.locator("#workspace script, #review script").count()).toBe(0);
    expect(await page.evaluate(() => window.__xss)).toBeUndefined();
    await expect(page.locator(".review-name")).toHaveText(hostile);
  });
});

test.describe("U02 large manifests stay virtual", () => {
  test.use({ mockConfig: "window.__mockConfig = { entries: 100000 };" });

  test("100 000 entries keep the DOM small and scroll to the end", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#add-folder").click();
    await page.locator('#mode-switch [data-mode="advanced"]').click();
    const list = page.locator("#manifest-list");
    await expect(list.locator(".vlist-row").first()).toContainText("file-0.bin");
    expect(await list.locator(".vlist-row").count()).toBeLessThan(100);
    await list.focus();
    await page.keyboard.press("End");
    await expect(list.locator(".vlist-row", { hasText: "file-99999.bin" })).toBeVisible();
    expect(await list.locator(".vlist-row").count()).toBeLessThan(100);
    const pages = await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "getManifestPage").length);
    expect(pages).toBeLessThan(10);
  });
});

test.describe("U06 keyboard and mode switching", () => {
  test("keyboard alone reaches Create and starts a job", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#add-folder").focus();
    await page.keyboard.press("Enter");
    await expect(page.locator("#btn-create")).toBeEnabled();
    for (let i = 0; i < 40; i++) {
      if (await page.evaluate(() => document.activeElement?.id === "btn-create")) break;
      await page.keyboard.press("Tab");
    }
    await expect(page.locator("#btn-create")).toBeFocused();
    await page.keyboard.press("Enter");
    await expect(page.locator("#job-list")).toContainText("Holiday photos");
  });

  test("switching modes keeps the values typed in either mode", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#add-folder").click();
    await page.locator("#draft-name").fill("My release");
    await page.locator('#mode-switch [data-mode="advanced"]').click();
    await page.locator("#tab-general").click();
    await expect(page.locator("#draft-name")).toHaveValue("My release");
    await page.locator("#tab-metadata").click();
    await page.locator("#draft-comment").fill("Shared with friends");
    await page.locator('#mode-switch [data-mode="simple"]').click();
    await expect(page.locator("#draft-name")).toHaveValue("My release");
    await page.locator('#mode-switch [data-mode="advanced"]').click();
    await page.locator("#tab-metadata").click();
    await expect(page.locator("#draft-comment")).toHaveValue("Shared with friends");
  });

  test("tabs move with the arrow keys", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator('#mode-switch [data-mode="advanced"]').click();
    await page.locator("#tab-files").focus();
    await page.keyboard.press("ArrowRight");
    await expect(page.locator("#tab-general")).toHaveAttribute("aria-selected", "true");
    await expect(page.locator("#tab-general")).toBeFocused();
  });

  test("a profile with several changes asks before applying", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator("#draft-profile").selectOption("private");
    await expect(page.locator("#profile-changes")).toBeVisible();
    await page.locator("#profile-apply").click();
    await expect(page.locator("#active-settings")).toContainText("Profile: private");
  });
});

// A 1100×760 window at 100/150/200% display scaling leaves this much CSS space.
for (const scale of [1, 1.5, 2]) {
  test.describe(`U06 display scaling ${scale * 100}%`, () => {
    test.use({ viewport: { width: Math.floor(1100 / scale), height: Math.floor(760 / scale) }, deviceScaleFactor: scale });

    test("primary actions stay reachable without horizontal scrolling", async ({ page }) => {
      await page.goto("/index.html");
      await page.locator("#add-folder").click();
      await expect(page.locator("#btn-create")).toBeEnabled();
      for (const id of ["#add-files", "#add-folder", "#draft-name", "#draft-profile", "#choose-output", "#btn-create", "#btn-batch"]) {
        const el = page.locator(id);
        await el.scrollIntoViewIfNeeded();
        await expect(el).toBeInViewport();
      }
      await page.locator("#btn-create").click();
      await expect(page.locator("#job-list")).toBeAttached();
      const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
      expect(overflow).toBeLessThanOrEqual(0);
    });
  });
}

test.describe("M2 profile and clipboard dialogs", () => {
  test.use({ mockConfig: 'window.prompt = () => { throw new Error("Script prompts are disabled by the host"); };' });

  async function openProfile(page) {
    await page.goto("/index.html");
    await page.locator('#mode-switch [data-mode="advanced"]').click();
    await page.locator("#tab-general").click();
    await page.locator("#save-profile").click();
    await expect(page.locator("#profile-name")).toBeFocused();
  }

  test("keyboard saves a Unicode profile and flushes pending draft edits", async ({ page }) => {
    await page.goto("/index.html");
    await page.locator('#mode-switch [data-mode="advanced"]').click();
    await page.locator("#tab-general").click();
    await page.locator("#draft-name").fill("Pending title");
    await page.locator("#save-profile").focus();
    await page.keyboard.press("Enter");
    await expect(page.locator("#profile-name")).toBeFocused();
    await page.keyboard.type("  Мой профиль  ");
    await page.keyboard.press("Enter");
    await expect(page.locator("#dialog")).not.toHaveAttribute("open", "");
    await expect(page.locator("#draft-profile option:checked")).toHaveText("Мой профиль");
    const calls = await page.evaluate(() => window.__mock.requests);
    const index = calls.findIndex((r) => r.operation === "saveProfile");
    expect(calls[index].payload.name).toBe("Мой профиль");
    expect(calls.slice(0, index).some((r) => r.operation === "updateDraft" && r.payload.patch.name === "Pending title")).toBe(true);
  });

  test("Escape cancels without saving and the dialog can be reopened", async ({ page }) => {
    await openProfile(page);
    await page.locator("#profile-name").fill("Cancelled");
    await page.keyboard.press("Escape");
    await expect(page.locator("#dialog")).not.toHaveAttribute("open", "");
    expect(await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "saveProfile"))).toHaveLength(0);
    await page.locator("#save-profile").click();
    await expect(page.locator("#profile-name")).toBeFocused();
    await page.locator("#profile-name-cancel").click();
  });

  test("blank and oversized UTF-8 names cannot be submitted", async ({ page }) => {
    await openProfile(page);
    for (const name of ["   ", "Я".repeat(101)]) {
      await page.locator("#profile-name").fill(name);
      await page.locator("#profile-name-save").click();
      expect(await page.locator("#profile-name").evaluate((input) => input.validity.valid)).toBe(false);
      expect(await page.evaluate(() => window.__mock.requests.filter((r) => r.operation === "saveProfile"))).toHaveLength(0);
    }
    await page.locator("#profile-name").fill("Valid");
    await page.keyboard.press("Enter");
    await expect(page.locator("#draft-profile option:checked")).toHaveText("Valid");
  });

  test("native errors keep the name and allow retry", async ({ page }) => {
    await openProfile(page);
    await page.evaluate(() => {
      const save = window.__mock.ops.saveProfile;
      let first = true;
      window.__mock.ops.saveProfile = (p) => {
        if (first) { first = false; throw new Error("Cannot save profile"); }
        return save(p);
      };
    });
    await page.locator("#profile-name").fill("Retry profile");
    await page.keyboard.press("Enter");
    await expect(page.locator("#profile-save-error")).toContainText("Cannot save profile");
    await expect(page.locator("#profile-name")).toHaveValue("Retry profile");
    await expect(page.locator("#profile-name")).toBeFocused();
    await page.keyboard.press("Enter");
    await expect(page.locator("#draft-profile option:checked")).toHaveText("Retry profile");
  });

  test("clipboard denial opens a selected read-only magnet without a script prompt", async ({ page }) => {
    await page.addInitScript(() => Object.defineProperty(navigator, "clipboard", {
      value: { writeText: async () => { throw new Error("Permission denied"); } }, configurable: true,
    }));
    await page.goto("/index.html");
    await page.locator("#add-folder").click();
    await page.locator("#btn-create").click();
    await page.evaluate(() => window.__mock.emit("job", { job: {
      id: "job-1", state: "Succeeded", version: "2", name: "Holiday photos", kind: "create",
      result: { name: "Holiday photos", output: "C:\\out.torrent", format: "v1", payloadBytes: "1",
        realFiles: 1, pieceLength: 16384, numPieces: 1, infohashV1: "0123456789012345678901234567890123456789", layout: [] },
    } }));
    await page.locator("#result-copy-magnet").click();
    const text = page.locator("#magnet-copy-text");
    await expect(text).toBeFocused();
    await expect(text).toHaveAttribute("readonly", "");
    await expect(text).toHaveValue(/magnet:\?xt=urn:btih:/);
    expect(await text.evaluate((input) => input.value.slice(input.selectionStart, input.selectionEnd))).toBe(await text.inputValue());
    await page.keyboard.press("Escape");
    await expect(page.locator("#dialog")).not.toHaveAttribute("open", "");
  });
});

test.describe("self-test persistence failure", () => {
  test.use({ mockConfig: "window.__mockConfig = { selfTestPersistence: false };" });
  test("reports failure when native profile persistence is not confirmed", async ({ page }) => {
    await page.goto("/index.html?selfTest=1");
    await expect.poll(() => page.evaluate(() => window.__mock.requests.find((r) => r.operation === "reportSelfTest")?.payload))
      .toMatchObject({ ok: false });
  });
});

test.describe("native self-test recovery checkpoint", () => {
  test.use({ mockConfig: `window.__mockConfig = {
    settings: { language: "ru", theme: "dark", mode: "advanced" },
    nativeCheckpoint: { revision: "1", jobs: [], evidence: { nativeDialogs: true } },
    rendererRecoveries: 1,
  };` });

  test("restores the checkpoint without replaying creation or dialogs", async ({ page }) => {
    await page.goto("/index.html?selfTest=1&nativeFlow=1");
    await expect.poll(() => page.evaluate(() => window.__mock.requests.find((r) => r.operation === "reportSelfTest")?.payload))
      .toMatchObject({ ok: true, rendererRecovery: true, recoveredJobs: 0 });
    expect(await page.evaluate(() => window.__mock.requests.filter((r) => ["startCreate", "selfTestStep", "saveProfile"].includes(r.operation))))
      .toHaveLength(0);
  });
});

test.describe("native self-test recovery failure", () => {
  test.use({ mockConfig: `window.__mockConfig = {
    settings: { language: "ru", theme: "dark", mode: "advanced" },
    nativeCheckpoint: { revision: "1", jobs: [], evidence: {} }, rendererRecoveries: 0,
  };` });

  test("a page reload alone cannot pass the renderer crash check", async ({ page }) => {
    await page.goto("/index.html?selfTest=1&nativeFlow=1");
    await expect.poll(() => page.evaluate(() => window.__mock.requests.find((r) => r.operation === "reportSelfTest")?.payload))
      .toMatchObject({ ok: false, error: expect.stringContaining("renderer failure callback"),
        message: expect.stringContaining("renderer failure callback") });
  });
});
