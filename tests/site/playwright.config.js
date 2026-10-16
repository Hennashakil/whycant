// Browser tests for the whycant website in docs/.
// Run:  cd tests/site && npm ci && npx playwright install chromium && npx playwright test
const { defineConfig, devices } = require("@playwright/test");

const PORT = 8123;

module.exports = defineConfig({
  testDir: ".",
  timeout: 30000,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  reporter: process.env.CI ? [["list"], ["github"]] : "list",
  use: {
    baseURL: `http://127.0.0.1:${PORT}/`,
    trace: "retain-on-failure",
  },
  projects: [
    { name: "desktop", use: { ...devices["Desktop Chrome"], viewport: { width: 1280, height: 900 } } },
    { name: "mobile", use: { ...devices["Desktop Chrome"], viewport: { width: 390, height: 844 }, isMobile: false, hasTouch: true } },
    { name: "dark", use: { ...devices["Desktop Chrome"], viewport: { width: 1280, height: 900 }, colorScheme: "dark" } },
  ],
  webServer: {
    command: `python3 -m http.server ${PORT} --bind 127.0.0.1 --directory ../../docs`,
    url: `http://127.0.0.1:${PORT}/`,
    reuseExistingServer: !process.env.CI,
    stdout: "ignore",
    stderr: "ignore",
  },
});
