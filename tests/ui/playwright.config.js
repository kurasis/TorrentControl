// UI tests run the real frontend in Chromium against a mocked native bridge.
const { defineConfig } = require("@playwright/test");

module.exports = defineConfig({
  testDir: "./specs",
  timeout: 30_000,
  fullyParallel: true,
  reporter: process.env.CI ? [["list"], ["github"]] : "list",
  use: {
    baseURL: "https://torrentcontrol.example/",
    viewport: { width: 1200, height: 800 },
    launchOptions: process.env.TC_CHROMIUM ? { executablePath: process.env.TC_CHROMIUM } : {},
  },
});
