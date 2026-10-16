// What a visitor does on the whycant website, checked in a real browser.
// No test touches the internet: GitHub is replaced by fixed data and every
// other outside request is blocked, so the results never depend on the network.
const { test, expect } = require("@playwright/test");
const fs = require("fs");

const AXE = fs.readFileSync(require.resolve("axe-core/axe.min.js"), "utf8");

const RELEASES = [
  { tag_name: "v9.9", published_at: "2026-10-16T16:40:00Z", html_url: "https://github.com/Hennashakil/whycant/releases/tag/v9.9",
    draft: false, assets: [{ download_count: 4 }, { download_count: 3 }] },
];
const REPO = { stargazers_count: 12 };
const COMMITS = [{ html_url: "https://github.com/Hennashakil/whycant/commit/abc",
  commit: { message: "Test commit\n\nbody", author: { date: "2026-10-16T16:00:00Z" } } }];

// Every test starts here: fixed GitHub answers, everything else outside is blocked.
test.beforeEach(async ({ page }, info) => {
  page.errors = [];
  page.outside = [];
  page.on("pageerror", (e) => page.errors.push(e.message));
  page.on("console", (m) => { if (m.type() === "error" && !/Failed to load resource/.test(m.text())) page.errors.push(m.text()); });
  await page.route(/^https?:\/\/(?!127\.0\.0\.1)/, (route) => {
    const url = new URL(route.request().url());
    page.outside.push(url.href);
    if (url.hostname === "api.github.com" && !info.title.includes("offline")) {
      const body = url.pathname.endsWith("/releases") ? RELEASES : url.pathname.endsWith("/commits") ? COMMITS : REPO;
      return route.fulfill({ status: 200, contentType: "application/json", headers: { "Access-Control-Allow-Origin": "*" }, body: JSON.stringify(body) });
    }
    return route.abort();
  });
});

test.afterEach(async ({ page }) => {
  expect(page.errors, "no JavaScript errors").toEqual([]);
});

async function axeCheck(page, label) {
  await page.addScriptTag({ content: AXE });
  const result = await page.evaluate(() => window.axe.run(document, {
    runOnly: ["wcag2a", "wcag2aa", "wcag21a", "wcag21aa", "wcag22aa", "best-practice"],
  }));
  const found = result.violations.map((v) => `${v.id}: ${v.help} (${v.nodes.map((n) => n.target.join(" ")).slice(0, 3).join(", ")})`);
  expect(found, `accessibility problems ${label}`).toEqual([]);
}

test("the page loads with its title and no horizontal scrolling", async ({ page }) => {
  await page.goto("./");
  await expect(page).toHaveTitle(/whycant/);
  await expect(page.locator("h1")).toContainText("Permission denied");
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  expect(overflow).toBeLessThanOrEqual(0);
});

test("no accessibility problems, before and after using the page", async ({ page }) => {
  await page.goto("./");
  await page.waitForTimeout(2000); // let the walk animation finish
  await axeCheck(page, "on load");

  await page.locator(".sw-special summary").click();
  await page.locator("#real summary").click();
  await page.locator('[data-preset="docker"]').click();
  await page.locator("#quiz-box [data-a='no']").click();
  await page.waitForTimeout(1500);
  await axeCheck(page, "after opening panels and answering a question");
});

test("switches and the number stay in step", async ({ page }) => {
  await page.goto("./");
  const num = page.locator("#sw-num");
  await num.fill("644");
  await expect(page.locator("#sw-sym")).toHaveText("rw-r--r--");
  await expect(page.getByLabel("Owner read", { exact: true })).toBeChecked();
  await expect(page.getByLabel("Owner run or enter", { exact: true })).not.toBeChecked();

  await page.getByLabel("Everyone else change", { exact: true }).click();
  await expect(num).toHaveValue("646");
  await expect(page.locator("#sw-flags")).toContainText("Everyone can change this");
  await expect(page.locator("#sw-c1")).toHaveText("chmod 646 FILE");
});

test("the demo walk stops each person at the right door", async ({ page }) => {
  await page.goto("./");
  const verdict = page.locator("#demo-verdict");

  await expect(verdict).toContainText("bob cannot get past /srv/demo/team");
  await expect(verdict).toContainText("usermod -aG devs bob");
  await expect(page.locator("#demo-path .step.deny")).toContainText("team/");
  await expect(page.locator("#demo-path .step.unreached")).toContainText("report.txt");

  await page.locator("label", { hasText: "carol" }).click();
  await expect(verdict).toContainText("Allowed");

  await page.locator("label", { hasText: "alice" }).click();
  await expect(verdict).toContainText("alice cannot read /srv/demo/team/report.txt");
  await expect(verdict).toContainText("chmod u+r");

  await page.locator("label", { hasText: "root" }).click();
  await expect(verdict).toContainText("Allowed");
});

test("the quiz scores five right answers as 5 / 5", async ({ page }) => {
  await page.goto("./");
  for (const answer of ["no", "yes", "no", "no", "no"]) {
    await page.locator(`#quiz-box [data-a='${answer}']`).click();
    await expect(page.locator("#q-result .tag")).toContainText("Right!");
    await page.locator("#q-next").click();
  }
  await expect(page.locator(".q-score")).toHaveText("5 / 5");
});

test("playground presets give a fix, or a stop for dangerous files", async ({ page }) => {
  await page.goto("./");
  const verdict = page.locator("#pg-verdict");

  await page.locator('[data-preset="owner"]').click();
  await expect(verdict).toContainText("henna cannot read /home/henna/notes.txt");
  await expect(verdict).toContainText("chmod u+r /home/henna/notes.txt");

  for (const preset of ["docker", "shadow"]) {
    await page.locator(`[data-preset="${preset}"]`).click();
    await expect(verdict.locator(".v-stop")).toHaveText("Stop");
    await expect(verdict).not.toContainText("usermod");
  }
});

test("a shared link opens the same example, and a bad link is ignored", async ({ page }) => {
  const state = { u: "alice", g: "alice", a: "w", rows: [["", "755", "root", "root"], ["notes", "644", "alice", "alice"]] };
  await page.goto("./#try=" + encodeURIComponent(JSON.stringify(state)));
  await expect(page.locator("#pg-user")).toHaveValue("alice");
  await expect(page.locator("#pg-verdict")).toContainText("alice can write to /notes");

  const evil = { u: "<img src=x onerror=alert(1)>", g: "x", a: "r", rows: "nope" };
  await page.goto("./#try=" + encodeURIComponent(JSON.stringify(evil)));
  await page.reload();
  await expect(page.locator("#pg-user")).toHaveValue("www-data");
  await expect(page.locator("#pg-verdict img")).toHaveCount(0);
});

test("pasted namei and id output is checked in the browser", async ({ page }) => {
  await page.goto("./");
  await page.locator("#real summary").click();
  await page.locator("#real-namei").fill([
    "f: /srv/demo/team/report.txt",
    " drwxr-xr-x root  root /",
    " drwxr-xr-x root  root srv",
    " drwxr-xr-x root  root demo",
    " drwxr-x--- root  devs team",
    " -rw-r----- alice devs report.txt",
  ].join("\n"));
  await page.locator("#real-id").fill("uid=1001(prince) gid=1001(prince) groups=1001(prince),27(sudo)");
  await page.locator("#real-go").click();
  await expect(page.locator("#pg-user")).toHaveValue("prince");
  await expect(page.locator("#pg-groups")).toHaveValue("prince, sudo");
  await expect(page.locator("#pg-verdict")).toContainText("prince cannot get past /srv/demo/team");

  await page.locator("#real-id").fill("not an id");
  await page.locator("#real-go").click();
  await expect(page.locator("#real-err")).toContainText("Couldn’t read the id output");
});

test("copy buttons put the command on the clipboard", async ({ page, context }) => {
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.goto("./");
  const box = page.locator("#install .codebox").nth(1);
  await box.locator(".copy").click();
  await expect(box.locator(".copy")).toHaveText("Copied");
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe("sha256sum -c --ignore-missing SHA256SUMS");

  await page.locator("#demo-verdict .mini-copy").first().click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe("setfacl -m u:bob:x /srv/demo/team");
});

test("project status comes live from GitHub", async ({ page }) => {
  await page.goto("./");
  await expect(page.locator("#st-version")).toHaveText("v9.9");
  await expect(page.locator("#st-downloads")).toHaveText("7");
  await expect(page.locator("#st-stars")).toHaveText("12");
  await expect(page.locator("#dl-btn")).toHaveText("Download v9.9");
  await expect(page.locator("#st-last")).toContainText("Test commit");
});

test("when GitHub is offline the page says so and keeps working", async ({ page }) => {
  await page.goto("./");
  await expect(page.locator("#live-text")).toContainText("Couldn’t reach GitHub");
  await expect(page.locator("#st-version")).toHaveText(/^v\d/);
  await expect(page.locator("#demo-verdict")).toContainText("Blocked");
});

test("comments load only when the visitor scrolls to them", async ({ page }) => {
  await page.goto("./");
  await expect(page.locator("#comments")).toBeVisible();
  await page.waitForTimeout(500);
  expect(page.outside.some((u) => u.includes("giscus.app"))).toBe(false);
  await page.locator("#comments").scrollIntoViewIfNeeded();
  await expect.poll(() => page.outside.some((u) => u.includes("giscus.app"))).toBe(true);
});

test("keyboard users can reach the code boxes and the main controls", async ({ page }) => {
  await page.goto("./");
  await expect(page.locator("#install .codebox pre").first()).toHaveAttribute("tabindex", "0");
  await page.keyboard.press("Tab");
  await expect(page.locator(".skip")).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(page).toHaveURL(/#main$/);
  // The skip link jumps past the header: the next Tab lands in the page itself
  await page.keyboard.press("Tab");
  await expect(page.locator("main .cta a").first()).toBeFocused();
});
