const { test, expect } = require("./fixture");

async function openBridgePage(page) {
  await page.route("**/bridge-test.html", (route) => route.fulfill({
    contentType: "text/html", body: "<!doctype html><title>Bridge regression</title>",
  }));
  await page.goto("/bridge-test.html");
}

for (const withFiles of [false, true]) {
  test(`failed ${withFiles ? "attached-file" : "ordinary"} sends ignore late errors and allow recovery`, async ({ page }) => {
    await openBridgePage(page);
    const result = await page.evaluate(async (withFiles) => {
      let listener, failedId, attached = false;
      const fail = (text, files) => {
        failedId = JSON.parse(text).requestId;
        attached = Boolean(files);
        throw new Error("Native send failed");
      };
      window.chrome.webview = {
        addEventListener: (_, callback) => { listener = callback; },
        postMessage: fail,
        postMessageWithAdditionalObjects: fail,
      };
      const bridge = await import("/bridge.js");
      let failure;
      try {
        if (withFiles) await bridge.requestWithFiles("drop", [new File(["payload"], "file.bin")]);
        else await bridge.request("getEngineInfo");
      } catch (error) { failure = error.message; }

      // A failed send is no longer outstanding. A late native error must be
      // ignored, including constructing an Error for a caller already rejected.
      let lateErrors = 0;
      const OriginalError = window.Error;
      window.Error = new Proxy(OriginalError, {
        construct(target, args) {
          if (args[0] === "Late failed request") lateErrors += 1;
          return Reflect.construct(target, args);
        },
      });
      try {
        listener({ data: JSON.stringify({ protocolVersion: 1, requestId: failedId,
          ok: false, error: { message: "Late failed request" } }) });
      } finally { window.Error = OriginalError; }

      window.chrome.webview.postMessage = (text) => {
        const message = JSON.parse(text);
        listener({ data: JSON.stringify({ protocolVersion: 1, requestId: message.requestId,
          ok: true, result: { recovered: true } }) });
      };
      return { failure, attached, lateErrors, next: await bridge.request("getEngineInfo") };
    }, withFiles);
    expect(result).toEqual({ failure: "Native send failed", attached: withFiles,
      lateErrors: 0, next: { recovered: true } });
  });
}

test("serialization failures reject without sending and the next request succeeds", async ({ page }) => {
  await openBridgePage(page);
  const result = await page.evaluate(async () => {
    let listener, sends = 0;
    window.chrome.webview = {
      addEventListener: (_, callback) => { listener = callback; },
      postMessage: (text) => {
        sends += 1;
        const message = JSON.parse(text);
        listener({ data: JSON.stringify({ protocolVersion: 1, requestId: message.requestId,
          ok: true, result: { recovered: true } }) });
      },
    };
    const { request } = await import("/bridge.js");
    const circular = {}; circular.self = circular;
    let rejected = 0;
    for (const payload of [circular, { integer: 1n }]) {
      try { await request("echo", payload); }
      catch (error) { if (error instanceof TypeError) rejected += 1; }
    }
    const sendsAfterFailure = sends;
    return { rejected, sendsAfterFailure, next: await request("echo"), sends };
  });
  expect(result).toEqual({ rejected: 2, sendsAfterFailure: 0, next: { recovered: true }, sends: 1 });
});
