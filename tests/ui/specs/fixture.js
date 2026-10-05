// Serves frontend/ at the same virtual host the app maps, and installs the
// mocked native bridge before any page script runs.
const path = require("node:path");
const fs = require("node:fs");
const base = require("@playwright/test");

const FRONTEND = path.resolve(__dirname, "../../../frontend");
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css" };

exports.test = base.test.extend({
  // Script source run before the mock installs; it may set window.__mockConfig.
  mockConfig: ["", { option: true }],
  page: async ({ page, mockConfig }, use) => {
    const errors = [];
    page.on("pageerror", (err) => errors.push(err));
    await page.route("https://torrentcontrol.example/**", (route) => {
      const url = new URL(route.request().url());
      const file = path.join(FRONTEND, decodeURIComponent(url.pathname === "/" ? "/index.html" : url.pathname));
      if (!file.startsWith(FRONTEND) || !fs.existsSync(file)) return route.fulfill({ status: 404, body: "" });
      return route.fulfill({ status: 200, contentType: TYPES[path.extname(file)] ?? "application/octet-stream", body: fs.readFileSync(file) });
    });
    // Playwright does not guarantee the order of separate init scripts.
    await page.addInitScript({ content: `${mockConfig}\n${fs.readFileSync(path.join(__dirname, "../mock-native.js"), "utf8")}` });
    await use(page);
    base.expect(errors, "uncaught page errors").toEqual([]);
  },
});
exports.expect = base.expect;
