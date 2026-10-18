/* SPDX-License-Identifier: Apache-2.0 */

import assert from "node:assert/strict";
import { createServer } from "node:http";
import { mkdir, readFile, readdir, stat, unlink, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { installHostFixture } from "./browser.fixture.mjs";

const root = fileURLToPath(new URL("../../../", import.meta.url));
const output = path.join(root, "build/webui-tests");
const requested = process.argv.slice(2);
const projects = requested.length ? requested : ["source", "bundle"];
for (const project of projects) assert.ok(["source", "bundle"].includes(project), `Unknown project ${project}`);
await mkdir(output, { recursive: true });

const mime = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".json": "application/json", ".svg": "image/svg+xml", ".woff2": "font/woff2", ".ttf": "font/ttf" };
const server = createServer(async (request, response) => {
  try {
    const url = new URL(request.url, "http://localhost");
    const [, project, ...segments] = decodeURIComponent(url.pathname).split("/");
    assert.ok(["source", "bundle"].includes(project));
    const directory = path.join(root, project === "source" ? "webui" : "build/webui");
    const filename = path.resolve(directory, segments.join("/") || "index.html");
    assert.ok(filename.startsWith(directory + path.sep));
    assert.ok((await stat(filename)).isFile());
    response.writeHead(200, { "Content-Type": mime[path.extname(filename)] || "application/octet-stream", "Cache-Control": "no-store" });
    response.end(await readFile(filename));
  } catch {
    response.writeHead(404);
    response.end("Not found");
  }
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
let browser;
const results = [];

async function loadPlaywright() {
  if (process.env.KAGAMI_PLAYWRIGHT_MODULE) {
    let filename = path.resolve(process.env.KAGAMI_PLAYWRIGHT_MODULE);
    if ((await stat(filename)).isDirectory()) filename = path.join(filename, "index.mjs");
    return import(pathToFileURL(filename).href);
  }
  return import("playwright-core");
}

const selector = (action) => `[data-action="${action}"]`;
const runtimeModules = ["overlay", "magic", "kasumi"].flatMap((backend) => ["mounted", "unmounted", "partial"].map((state) => ({
  id: `${backend}_${state}`, name: `${backend} ${state}`, mode: "none", strategy: "none", rules: [],
  runtime: { state, backend, reason: null, error: null },
})));
runtimeModules.push(
  { id: "disabled", name: "Disabled module", mode: "kasumi", strategy: "kasumi", inactive: true, rules: [], runtime: { state: "unmounted", backend: "none", reason: "disabled", error: null } },
  { id: "missing_snapshot", name: "No snapshot", mode: "magic", strategy: "magic", rules: [], runtime: { state: "unknown", backend: null, reason: "missing_snapshot", error: null } },
  { id: "read_error", name: "Runtime read error", mode: "overlay", strategy: "overlay", rules: [], runtime: { state: "mounted", backend: "overlay", reason: null, error: { code: "READ_FAILED", message: "Cannot read mountinfo" } } },
  // kasumi version lists beta_audio as active: legacy metadata still cannot
  // establish either a backend or a current mounted/unmounted state.
  { id: "beta_audio", name: "Legacy controller module", mode: "kasumi", strategy: "kasumi", rules: [] },
);
async function navigate(page, tab) {
  await page.locator(`[data-tab="${tab}"]:visible`).click();
  await page.waitForFunction((tab) => new URL(location.href).searchParams.get("tab") === tab, tab);
}
async function openView(page, view) {
  await page.locator(`${selector("open-view")}[data-view="${view}"]`).click();
  await page.waitForFunction((view) => new URL(location.href).searchParams.get("view") === view, view);
}
async function back(page) {
  await page.locator(selector("back")).click();
  await page.waitForFunction(() => !new URL(location.href).searchParams.has("view"));
}
async function assertStableInput(page, field, text) {
  const input = page.locator(`[data-field="${field}"]`);
  await input.fill("");
  await input.focus();
  await input.pressSequentially(text, { delay: 30 });
  assert.equal(await input.inputValue(), text);
  assert.equal(await input.evaluate((element) => document.activeElement === element), true, `${field} lost keyboard focus`);
}
async function waitForCommand(page, needle) {
  await page.waitForFunction((needle) => window.__kagamiFixture.commands.some((command) => command.includes(needle)), needle);
}
async function assertNoOverflow(page) {
  const size = await page.evaluate(() => ({ width: innerWidth, document: document.documentElement.scrollWidth, body: document.body.scrollWidth }));
  assert.ok(size.document <= size.width + 1 && size.body <= size.width + 1, `Horizontal overflow: ${JSON.stringify(size)}`);
}
async function selectPreference(page, key, value) {
  await page.locator(`${selector("preference")}[data-key="${key}"]`).click();
  await page.locator(`#editor input[name="value"][value="${value}"]`).check();
  await page.locator(selector("dialog-save")).click();
  await page.locator("#editor").waitFor({ state: "hidden" });
}

async function runCase(project, name, fn, options = {}) {
  if (process.env.KAGAMI_BROWSER_TEST_FILTER && !new RegExp(process.env.KAGAMI_BROWSER_TEST_FILTER).test(name)) return;
  const context = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 1, colorScheme: "light", locale: "en-US", reducedMotion: "reduce", ...(options.touch ? { hasTouch: true, isMobile: true } : {}) });
  context.setDefaultTimeout(8000);
  const page = await context.newPage();
  if (options.clock) await page.clock.install();
  const errors = [];
  const external = [];
  const failedAssets = [];
  let fileUrl;
  if (options.offlineFile) {
    const directory = path.join(output, "standalone");
    await mkdir(directory, { recursive: true });
    await writeFile(path.join(directory, "index.html"), await readFile(path.join(root, "build/webui/index.html")));
    assert.deepEqual(await readdir(directory), ["index.html"], "Offline fixture must contain only the HTML artifact");
    const url = pathToFileURL(path.join(directory, "index.html"));
    url.search = "mock=1&tab=status";
    fileUrl = url.href;
    await context.setOffline(true);
  }
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("response", (response) => { if (response.status() >= 400) failedAssets.push(`${response.status()} ${response.url()}`); });
  await context.route("**/*", (route) => {
    if (fileUrl && route.request().url().split("?")[0] === fileUrl.split("?")[0]) return route.continue();
    // WebUI X intercepts these virtual host resources; never send them online.
    if (/^https:\/\/mui\.kernelsu\.org\/internal\/(colors|insets)\.css$/.test(route.request().url())) {
      if (!options.hostStyles) return route.abort();
      const css = route.request().url().endsWith("colors.css")
        ? ":root { --primary: #7654ab; --onPrimary: #ffffff; --surface: #faf6ff; }"
        : ":root { --window-inset-top: 24px; --window-inset-bottom: 20px; }";
      return route.fulfill({ status: 200, contentType: "text/css", body: css });
    }
    if (new URL(route.request().url()).origin !== origin) {
      external.push(route.request().url());
      return route.abort();
    }
    return route.continue();
  });
  if (options.fixture !== false) await context.addInitScript(installHostFixture, options.fixture || {});
  const start = Date.now();
  try {
    await page.goto(fileUrl || `${origin}/${project}/${options.query || "?tab=status"}`, { waitUntil: "networkidle" });
    await page.locator("main#page-content").waitFor();
    await fn(page);
    if (options.fixture !== false) assert.deepEqual(await page.evaluate(() => window.__kagamiFixture.unhandled), [], "The host fixture did not handle a CLI request");
    assert.deepEqual(errors, [], "Uncaught browser errors");
    assert.deepEqual(external, [], "The WebUI must load without external requests");
    assert.deepEqual(failedAssets, [], "Missing local assets");
    await unlink(path.join(output, `${project}-${name}-failed.png`)).catch((error) => { if (error.code !== "ENOENT") throw error; });
    await page.screenshot({ path: path.join(output, `${project}-${name}.png`), fullPage: true });
    results.push({ project, name, status: "passed", durationMs: Date.now() - start });
    console.log(`PASS ${project}: ${name}`);
  } catch (error) {
    await page.screenshot({ path: path.join(output, `${project}-${name}-failed.png`), fullPage: true }).catch(() => {});
    const diagnostics = await page.evaluate(() => ({ text: document.body.innerText.slice(0, 6000), commands: window.__kagamiFixture?.commands.slice(-40) || [] })).catch(() => ({}));
    results.push({ project, name, status: "failed", error: error.stack || String(error), browserErrors: errors, external, failedAssets, ...diagnostics, durationMs: Date.now() - start });
    console.error(`FAIL ${project}: ${name}\n${error.stack || error}`);
  } finally {
    await context.close();
  }
}

try {
  const { chromium } = await loadPlaywright();
  browser = await chromium.launch({ headless: true, ...(process.env.KAGAMI_BROWSER_EXECUTABLE_PATH ? { executablePath: process.env.KAGAMI_BROWSER_EXECUTABLE_PATH } : {}) });
  for (const project of projects) {
    if (project === "bundle") await runCase(project, "standalone-file-offline", async (page) => {
      await navigate(page, "modules");
      await page.locator(selector("module-detail")).first().waitFor();
      await navigate(page, "settings");
      await openView(page, "about");
      assert.equal(new URL(page.url()).protocol, "file:");
      await back(page);
      await selectPreference(page, "theme", "dark");
      assert.equal(await page.locator("html").getAttribute("data-theme"), "dark");
      await assertNoOverflow(page);
    }, { fixture: false, offlineFile: true });

    await runCase(project, "first-use-confirmation", async (page) => {
      const confirm = page.locator(selector("dialog-save"));
      await page.locator("#editor[open]").waitFor();
      assert.equal(await confirm.isEnabled(), true, "First-use acknowledgement must not impose a countdown");
      assert.equal((await confirm.innerText()).trim(), "I understand");
      await page.goBack();
      await page.locator("#editor[open]").waitFor();
      assert.notEqual(await page.evaluate(() => localStorage.getItem("kagami_experimental_ack")), "true", "Browser back must not acknowledge the first-use notice");
      await confirm.click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      assert.equal(await page.evaluate(() => localStorage.getItem("kagami_experimental_ack")), "true");
      await page.reload({ waitUntil: "networkidle" });
      assert.equal(await page.locator("#editor").isVisible(), false);
    }, { fixture: { acknowledged: false } });

    await runCase(project, "browser-preview", async (page) => {
      await navigate(page, "modules");
      await page.locator(selector("module-detail")).first().waitFor();
      await navigate(page, "settings");
      await openView(page, "about");
      await assertNoOverflow(page);
    }, { fixture: false, query: "?mock=1&tab=status" });

    await runCase(project, "slow-poll-keeps-touch-controls-responsive", async (page) => {
      await page.locator(selector("module-detail")).first().waitFor();
      await page.evaluate(() => { window.__kagamiFixture.held = ["module list"]; });
      await page.clock.fastForward(16000);
      await page.waitForFunction(() => window.__kagamiFixture.pending.length > 0);
      await page.locator(selector("module-filter")).tap();
      await page.locator('#editor input[value="all"]').check();
      await page.locator(selector("dialog-save")).tap();
      await page.locator("#editor").waitFor({ state: "hidden" });
      await page.locator(selector("module-detail")).first().tap();
      await page.locator(selector("rule-add")).tap();
      await page.locator(selector("dialog-cancel")).tap();
      await page.locator("#editor").waitFor({ state: "hidden" });
      await back(page);
      await assertStableInput(page, "module-search", "Alpha");
      await navigate(page, "settings");
      await selectPreference(page, "theme", "dark");
      assert.equal(await page.evaluate(() => window.__kagamiFixture.execCalls), 0);
      await page.evaluate(() => window.__kagamiFixture.release());
      await navigate(page, "modules");
      await page.waitForFunction(() => !document.querySelector('[data-action="refresh"]').disabled);
      assert.equal(await page.locator('[data-field="module-search"]').inputValue(), "Alpha");
      await page.locator(selector("module-detail")).first().tap();
      await back(page);
    }, { query: "?tab=modules", clock: true, touch: true });

    await runCase(project, "legacy-host-does-not-poll-synchronous-exec", async (page) => {
      await page.waitForFunction(() => !document.querySelector('[data-action="refresh"]').disabled);
      const count = await page.evaluate(() => window.__kagamiFixture.commands.length);
      await page.clock.fastForward(31000);
      assert.equal(await page.evaluate(() => window.__kagamiFixture.commands.length), count);
      await page.locator(selector("refresh")).click();
      await page.waitForFunction(count => window.__kagamiFixture.commands.length > count, count);
      await page.waitForFunction(() => !document.querySelector('[data-action="refresh"]').disabled);
      await page.locator(selector("module-detail")).first().click();
      await back(page);
    }, { query: "?tab=modules", fixture: { legacyExec: true }, clock: true });

    await runCase(project, "navigation-search-detail", async (page) => {
      for (const tab of ["status", "modules", "kasumi", "settings"]) assert.equal(await page.locator(`[data-tab="${tab}"]:visible`).count(), 1);
      await navigate(page, "modules");
      await assertStableInput(page, "module-search", "Alpha");
      await page.locator(`${selector("module-detail")}[data-id="alpha_fonts"]`).click();
      await page.waitForFunction(() => new URL(location.href).searchParams.get("id") === "alpha_fonts");
      assert.ok((await page.locator("#page-content").innerText()).includes("Alpha Fonts"));
      await back(page);
      assert.equal(await page.locator('[data-field="module-search"]').inputValue(), "Alpha");
      await page.locator('[data-field="module-search"]').fill("");
      assert.equal(await page.locator('img[src="x"]').count(), 0, "Module text must not become executable HTML");
      await page.waitForFunction(() => [...document.querySelectorAll('[data-action="module-detail"] svg')].every((svg) => svg.getBBox().width > 0), undefined, { timeout: 3000 });
      await page.screenshot({ path: path.join(output, `${project}-modules-list.png`), fullPage: true });
      await openView(page, "conflicts");
      await waitForCommand(page, "module check-conflicts");
      await page.getByText("/system/etc/shared.xml", { exact: true }).waitFor();
      await back(page);
      for (const tab of ["kasumi", "settings", "status"]) {
        await navigate(page, tab);
        await page.screenshot({ path: path.join(output, `${project}-${tab}.png`), fullPage: true });
      }
      await assertNoOverflow(page);
    });

    await runCase(project, "logs-focus-and-back", async (page) => {
      await openView(page, "logs");
      await waitForCommand(page, "tail -n");
      await assertStableInput(page, "log-search", "Fixture warning");
      assert.ok((await page.locator("#page-content").innerText()).includes("Fixture warning"));
      await back(page);
      assert.equal(new URL(page.url()).searchParams.get("tab"), "status");
      await openView(page, "logs");
      await page.goBack();
      await page.waitForFunction(() => !new URL(location.href).searchParams.has("view"));
    });

    await runCase(project, "preferences-rtl-narrow", async (page) => {
      await navigate(page, "settings");
      await page.emulateMedia({ colorScheme: "dark" });
      await selectPreference(page, "theme", "light");
      const forcedLight = await page.evaluate(() => ({ mode: document.documentElement.dataset.theme, background: getComputedStyle(document.body).backgroundColor }));
      assert.equal(forcedLight.mode, "light");
      await page.screenshot({ path: path.join(output, `${project}-forced-light.png`), fullPage: true });
      await selectPreference(page, "theme", "dark");
      const forcedDark = await page.evaluate(() => ({ mode: document.documentElement.dataset.theme, background: getComputedStyle(document.body).backgroundColor }));
      assert.equal(forcedDark.mode, "dark");
      assert.notEqual(forcedDark.background, forcedLight.background, "Forced light must override the system dark palette");
      await page.screenshot({ path: path.join(output, `${project}-forced-dark.png`), fullPage: true });
      await selectPreference(page, "language", "ar");
      assert.equal(await page.locator("html").getAttribute("dir"), "rtl");
      await page.setViewportSize({ width: 320, height: 740 });
      for (const tab of ["status", "modules", "kasumi", "settings"]) {
        await navigate(page, tab);
        await assertNoOverflow(page);
        await page.screenshot({ path: path.join(output, `${project}-rtl-320-${tab}.png`), fullPage: true });
      }
      await page.reload({ waitUntil: "networkidle" });
      assert.equal(await page.locator("html").getAttribute("dir"), "rtl");
      await assertNoOverflow(page);
    });

    await runCase(project, "host-colors-and-insets", async (page) => {
      await page.waitForFunction(() => getComputedStyle(document.documentElement).getPropertyValue("--md-sys-color-primary").trim().toLowerCase() === "#7654ab");
      assert.equal(await page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue("--window-inset-top").trim()), "24px");
      await assertNoOverflow(page);
      await navigate(page, "settings");
      await selectPreference(page, "theme", "light");
      assert.notEqual(await page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue("--md-sys-color-primary").trim().toLowerCase()), "#7654ab", "An explicit palette must override host dynamic colors");
    }, { hostStyles: true });

    await runCase(project, "direct-module-link", async (page) => {
      await page.getByText("Beta Audio", { exact: true }).first().waitFor();
      assert.equal(new URL(page.url()).searchParams.get("view"), "module");
      await back(page);
      assert.equal(new URL(page.url()).searchParams.get("tab"), "modules");
    }, { query: "?tab=modules&view=module&id=beta_audio" });

    await runCase(project, "module-dialog-save-cancel", async (page) => {
      const opener = page.locator(`${selector("module-mode")}[data-id="alpha_fonts"]`);
      await opener.click();
      await page.locator('#editor input[name="value"][value="kasumi"]').check();
      await page.locator(selector("dialog-cancel")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      assert.equal(await page.evaluate(() => window.__kagamiFixture.modules.find((item) => item.id === "alpha_fonts").mode), "auto");
      await opener.click();
      await page.keyboard.press("Escape");
      await page.locator("#editor").waitFor({ state: "hidden" });
      await opener.click();
      await page.locator('#editor input[name="value"][value="kasumi"]').check();
      await page.locator(selector("dialog-save")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      await page.waitForFunction(() => window.__kagamiFixture.modules.find((item) => item.id === "alpha_fonts").mode === "kasumi");
      assert.ok((await page.locator('[data-key="module-mode"]').innerText()).includes("OverlayFS"), "Persisting a strategy must not claim the running backend changed");
      assert.equal(await page.locator('[data-key="runtime-status"]').innerText(), "OverlayFS Mounted");
      assert.equal(await opener.locator("small").innerText(), "Kasumi");
      await back(page);
      assert.equal(await page.locator(`${selector("module-detail")}[data-id="alpha_fonts"] [data-runtime-state]`).innerText(), "OverlayFS Mounted");
    }, { query: "?tab=modules&view=module&id=alpha_fonts" });

    await runCase(project, "module-runtime-backends-and-inactive", async (page) => {
      assert.equal(await page.locator('[data-key="mounted-count"] strong').innerText(), "6", "Only mounted and partial runtime evidence contributes to the count");
      assert.equal(await page.locator('[data-key="module-count"] strong').innerText(), String(runtimeModules.length));
      await navigate(page, "modules");
      await page.locator(`${selector("module-detail")}[data-id="disabled"]`).waitFor();
      assert.equal(await page.locator(selector("module-detail")).count(), runtimeModules.length);
      for (const module of runtimeModules) {
        const expectedState = module.runtime?.error ? "unknown" : module.runtime?.state || "unknown";
        const backend = { overlay: "OverlayFS", magic: "Magic Mount", kasumi: "Kasumi", none: "None" }[module.runtime?.backend];
        const expectedLabel = module.runtime?.error ? "Could not read mount status" : expectedState === "unknown" ? "Mount status unavailable"
          : `${module.runtime.backend === "none" ? "" : `${backend} `}${{ mounted: "Mounted", unmounted: "Not mounted", partial: "Partially mounted" }[expectedState]}`;
        const item = page.locator(`${selector("module-detail")}[data-id="${module.id}"]`);
        assert.equal(await item.locator("[data-runtime-state]").getAttribute("data-runtime-state"), expectedState);
        assert.equal(await item.locator("[data-runtime-state]").innerText(), expectedLabel);
        await item.click();
        assert.equal(await page.locator('[data-key="runtime-status"]').innerText(), expectedLabel);
        const actualBackend = page.locator('[data-key="module-mode"] dl div').filter({ has: page.locator("dt", { hasText: "Running backend" }) }).locator("dd");
        assert.equal(await actualBackend.innerText(), module.runtime?.error ? "Could not read mount status" : backend || "Unknown");
        if (module.runtime?.error) await page.getByText("Cannot read mountinfo", { exact: true }).waitFor();
        await back(page);
      }
      assert.ok(await page.evaluate(() => window.__kagamiFixture.commands.filter((command) => command.includes("module list")).every((command) => command.endsWith("module list --include-inactive"))));
      assert.doesNotMatch(await page.locator("#page-content").innerText(), /observed in Kasumi|Reported strategy|Active in Kasumi/);
      await assertNoOverflow(page);
    }, { fixture: { modules: runtimeModules } });

    await runCase(project, "module-mounted-filter-and-search-focus", async (page) => {
      await page.locator(selector("module-filter")).click();
      await page.locator('#editor input[name="value"][value="active"]').check();
      await page.locator(selector("dialog-save")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      assert.equal(await page.locator(selector("module-detail")).count(), 6);
      assert.deepEqual((await page.locator(`${selector("module-detail")} [data-runtime-state]`).evaluateAll((items) => items.map((item) => item.dataset.runtimeState))).sort(), ["mounted", "mounted", "mounted", "partial", "partial", "partial"]);
      await assertStableInput(page, "module-search", "magic");
      assert.equal(await page.locator(selector("module-detail")).count(), 2);
      await page.locator(`${selector("module-detail")}[data-id="magic_partial"]`).click();
      await back(page);
      assert.equal(await page.locator('[data-field="module-search"]').inputValue(), "magic");
      await assertStableInput(page, "module-search", "kasumi");
      assert.equal(await page.locator(selector("module-detail")).count(), 2, "Legacy Kasumi membership must not enter the mounted filter");
      await page.locator(selector("module-filter")).click();
      await page.locator('#editor input[name="value"][value="kasumi"]').check();
      await page.locator(selector("dialog-save")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      await page.locator('[data-field="module-search"]').fill("");
      assert.deepEqual((await page.locator(selector("module-detail")).evaluateAll((items) => items.map((item) => item.dataset.id))).sort(), ["beta_audio", "disabled"], "Configured-mode filtering remains independent of running backend");
    }, { query: "?tab=modules", fixture: { modules: runtimeModules, failures: { "api system": true } } });

    await runCase(project, "module-runtime-read-failure-is-not-unmounted", async (page) => {
      await page.locator(`${selector("module-detail")}[data-id="alpha_fonts"]`).waitFor();
      await page.evaluate(() => { window.__kagamiFixture.failures["module list"] = true; });
      await page.locator(selector("refresh")).click();
      await page.getByText(/Fixture failure: module list/).first().waitFor();
      assert.equal(await page.getByText("Could not read mount status", { exact: true }).count(), 3);
      assert.equal(await page.locator('[data-runtime-state="mounted"]').count(), 0);
      await page.locator(`${selector("module-detail")}[data-id="alpha_fonts"]`).click();
      assert.equal(await page.locator('[data-key="runtime-status"]').innerText(), "Could not read mount status");
      assert.equal(await page.locator(selector("module-mode")).isEnabled(), false);
      await back(page);
      await page.locator(selector("module-filter")).click();
      await page.locator('#editor input[name="value"][value="active"]').check();
      await page.locator(selector("dialog-save")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      assert.equal(await page.locator(selector("module-detail")).count(), 0, "Stale mounted badges must not survive a failed read");
      await navigate(page, "status");
      assert.equal(await page.locator('[data-key="mounted-count"] strong').innerText(), "—");
    }, { query: "?tab=modules" });

    await runCase(project, "rule-rename-partial-failure", async (page) => {
      const renamed = "/system/fonts/fixture's theme";
      await page.locator(`${selector("rule-edit")}[data-path="/system/fonts"]`).click();
      await page.locator('#editor input[name="path"]').fill(renamed);
      await page.locator('#editor input[name="mode"][value="hide"]').check();
      await page.locator(selector("dialog-save")).click();
      await page.locator("#dialog-error").waitFor({ state: "visible" });
      assert.match(await page.locator("#dialog-error").innerText(), /old rule could not be removed/);
      assert.equal(await page.locator('#editor input[name="path"]').inputValue(), renamed, "Failed saves must retain the draft");
      const added = await page.evaluate(() => window.__kagamiFixture.commands.filter((command) => command.includes("module add-rule")).length);
      assert.equal(added, 1);
      await page.locator(selector("dialog-save")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      const state = await page.evaluate(() => ({ rules: window.__kagamiFixture.modules.find((item) => item.id === "alpha_fonts").rules, additions: window.__kagamiFixture.commands.filter((command) => command.includes("module add-rule")).length }));
      assert.deepEqual(state.rules, [{ path: renamed, mode: "hide" }]);
      assert.equal(state.additions, added, "Partial rename retry must not add the same rule again");
      await page.locator(selector("rule-remove")).click();
      await page.locator(selector("dialog-save")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      assert.deepEqual(await page.evaluate(() => window.__kagamiFixture.modules.find((item) => item.id === "alpha_fonts").rules), []);
    }, { query: "?tab=modules&view=module&id=alpha_fonts", fixture: { failures: { "module remove-rule": 1 } } });

    await runCase(project, "partition-scan-preserves-defaults", async (page) => {
      await page.locator(selector("partition-scan")).click();
      await page.waitForFunction(() => window.__kagamiFixture.config.partitions.includes("odm_dlkm"));
      const partitions = await page.evaluate(() => window.__kagamiFixture.config.partitions);
      for (const name of ["system", "vendor", "product", "system_ext", "odm", "oem", "vendor_dlkm", "odm_dlkm"]) assert.ok(partitions.includes(name), `Missing managed partition ${name}`);
      assert.equal(new Set(partitions).size, partitions.length);
      await page.locator('[data-key="save-config:partitions"]').getByText("Saved changes are used at the next boot.", { exact: true }).waitFor();
    }, { query: "?tab=settings&view=partitions", fixture: { partitions: [] } });

    await runCase(project, "config-write-failure-retry", async (page) => {
      const toggle = page.locator('md-switch[data-field="debug"]');
      await toggle.click();
      await page.getByText(/Fixture failure: config merge-json/).first().waitFor();
      assert.equal(await page.evaluate(() => window.__kagamiFixture.config.debug), false);
      await page.locator(`${selector("retry-mutation")}[data-key="config:debug"]`).click();
      await page.waitForFunction(() => window.__kagamiFixture.config.debug === true);
      await page.locator('[data-key="save-config:debug"]').getByText("Saved and applied", { exact: true }).waitFor();
      await waitForCommand(page, "config apply");
      assert.deepEqual(await page.evaluate(() => window.__kagamiFixture.config.future_setting), { keep: true });
      assert.equal(await page.evaluate(() => window.__kagamiFixture.config.policy.owner), "manual");
    }, { query: "?tab=settings", fixture: { failures: { "config merge-json": 1 } } });

    await runCase(project, "config-apply-failure-retry", async (page) => {
      await page.locator('md-switch[data-field="debug"]').click();
      await page.getByText(/Saved, but runtime application failed/).first().waitFor();
      assert.equal(await page.evaluate(() => window.__kagamiFixture.config.debug), true);
      await waitForCommand(page, "kasumi disable");
      await page.locator(`${selector("retry-mutation")}[data-key="config:debug"]`).click();
      await page.locator('[data-key="save-config:debug"]').getByText("Saved and applied", { exact: true }).waitFor();
      assert.equal(await page.evaluate(() => window.__kagamiFixture.commands.filter((command) => command.includes("config apply")).length), 2, "Retry must attempt runtime application again");
    }, { query: "?tab=settings", fixture: { failures: { "config apply": 1 } } });

    await runCase(project, "saved-readback-retry", async (page) => {
      await page.locator('md-switch[data-field="debug"]').waitFor();
      await page.waitForFunction(() => !document.querySelector('md-switch[data-field="debug"]').disabled);
      await page.evaluate(() => { window.__kagamiFixture.failures["config show"] = 1; });
      await page.locator('md-switch[data-field="debug"]').click();
      await page.getByText(/Saved · status not confirmed/).first().waitFor();
      const writes = await page.evaluate(() => window.__kagamiFixture.commands.filter((command) => command.includes("config merge-json")).length);
      await page.locator(`${selector("retry-mutation")}[data-key="config:debug"]`).click();
      await page.locator('[data-key="save-config:debug"]').getByText("Saved and applied", { exact: true }).waitFor();
      assert.equal(await page.evaluate(() => window.__kagamiFixture.commands.filter((command) => command.includes("config merge-json")).length), writes, "Readback retry must not repeat a successful mutation");
    }, { query: "?tab=settings" });

    await runCase(project, "slow-save-navigation-refresh", async (page) => {
      await page.locator('md-switch[data-field="debug"]').click();
      await navigate(page, "modules");
      await page.locator(selector("refresh")).click();
      await navigate(page, "settings");
      await page.locator('[data-key="save-config:debug"]').getByText("Saved and applied", { exact: true }).waitFor();
      assert.equal(await page.locator('md-switch[data-field="debug"]').evaluate((element) => element.selected), true);
      assert.equal(await page.evaluate(() => window.__kagamiFixture.config.debug), true);
      assert.equal(await page.evaluate(() => window.__kagamiFixture.commands.filter((command) => command.includes("config merge-json")).length), 1, "Navigation and refresh must not repeat a pending write");
      await openView(page, "storage");
      await page.locator(`${selector("config-edit")}[data-field="moduledir"]`).click();
      await page.locator('#editor input[name="value"]').fill("/data/adb/slow-write");
      await page.locator(selector("dialog-save")).click();
      await page.locator('#editor[aria-busy="true"]').waitFor();
      await page.goBack();
      assert.equal(await page.locator("#editor").isVisible(), true, "Back must not dismiss an operation whose result is still pending");
      await page.locator("#editor").waitFor({ state: "hidden" });
      assert.equal(await page.evaluate(() => window.__kagamiFixture.config.moduledir), "/data/adb/slow-write");
      assert.equal(new URL(page.url()).searchParams.get("view"), "storage");
      assert.equal(await page.evaluate(() => window.__kagamiFixture.commands.filter((command) => command.includes("config merge-json")).length), 2, "A locked back action must not duplicate the write");
    }, { query: "?tab=settings", fixture: { delayMs: 200 } });

    await runCase(project, "chinese-breakpoints-keyboard", async (page) => {
      assert.equal(await page.locator("html").getAttribute("lang"), "zh-CN");
      for (const width of [390, 480, 840, 1280]) {
        await page.setViewportSize({ width, height: 844 });
        for (const tab of ["status", "modules", "kasumi", "settings"]) {
          await navigate(page, tab);
          await assertNoOverflow(page);
          assert.equal(await page.locator(`[data-tab="${tab}"]:visible`).count(), 1);
        }
        assert.equal(await page.locator(".side-nav").isVisible(), width >= 840);
        await page.screenshot({ path: path.join(output, `${project}-zh-CN-${width}.png`), fullPage: true });
      }
      await page.setViewportSize({ width: 390, height: 844 });
      await openView(page, "storage");
      await page.locator(`${selector("config-edit")}[data-field="moduledir"]`).click();
      const field = page.locator('#editor input[name="value"]');
      await field.fill("/data/adb/keyboard-draft");
      await page.setViewportSize({ width: 390, height: 390 });
      await page.waitForFunction(() => document.documentElement.style.getPropertyValue("--viewport-height") === "390px");
      assert.equal(await field.inputValue(), "/data/adb/keyboard-draft");
      await page.locator(selector("dialog-save")).scrollIntoViewIfNeeded();
      const box = await page.locator(selector("dialog-save")).boundingBox();
      assert.ok(box.y >= 0 && box.y + box.height <= 390, "The keyboard-sized viewport must keep dialog actions reachable");
      await page.screenshot({ path: path.join(output, `${project}-keyboard-dialog.png`), fullPage: true });
      await page.locator(selector("dialog-cancel")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      assert.equal(await page.evaluate(() => window.__kagamiFixture.config.moduledir), "/data/adb/modules");
    }, { fixture: { language: "zh-CN" } });

    await runCase(project, "runtime-read-failure", async (page) => {
      await waitForCommand(page, "api system");
      await page.getByText(/Fixture failure: api system/).first().waitFor();
      await navigate(page, "modules");
      await page.getByText("Alpha Fonts", { exact: true }).first().waitFor();
      await assertNoOverflow(page);
    }, { fixture: { failures: { "api system": true } } });

    await runCase(project, "hide-rules-active-and-builtin-lkm", async (page) => {
      await openView(page, "hide");
      await page.getByText("Confirmed active", { exact: true }).waitFor();
      assert.equal(await page.locator('code[dir="ltr"]').first().getAttribute("dir"), "ltr");
      await back(page);
      await openView(page, "rules");
      await page.locator(".rule-details details").first().waitFor();
      assert.equal(await page.locator(".rule-details details").count(), 2);
      await page.locator(".rule-details summary").filter({ hasText: "/system/etc/audio" }).click();
      await page.getByText("/data/adb/modules/beta_audio/system/etc/audio", { exact: true }).waitFor();
      assert.equal(await page.locator('#page-content [role="alert"]').count(), 0);
      await back(page);
      await openView(page, "lkm");
      await page.getByText("Kasumi is provided by the kernel. External module controls do not apply.", { exact: true }).waitFor();
      assert.equal(await page.locator(`${selector("lkm-load")}, ${selector("lkm-unload")}`).count(), 0);
      assert.equal(await page.locator('md-switch[data-field="lkm_autoload"]').count(), 0);
    }, { query: "?tab=kasumi", fixture: { builtin: true } });

    await runCase(project, "lkm-unload-failure", async (page) => {
      await page.locator(selector("lkm-unload")).click();
      await page.locator(selector("dialog-save")).click();
      await page.locator("#dialog-error").waitFor({ state: "visible" });
      assert.match(await page.locator("#dialog-error").innerText(), /Fixture failure: lkm unload/);
      assert.equal(await page.evaluate(() => window.__kagamiFixture.lkm.loaded), true);
      await page.locator(selector("dialog-cancel")).click();
      await page.locator("#editor").waitFor({ state: "hidden" });
      assert.equal(await page.locator(selector("lkm-unload")).count(), 1);
      assert.equal(await page.locator(selector("lkm-load")).count(), 0);
    }, { query: "?tab=kasumi&view=lkm", fixture: { failures: { "lkm unload": 1 } } });

    await runCase(project, "protocol-mismatch", async (page) => {
      await page.locator("#page-content").getByText(/Kasumi Protocol Mismatch/i).first().waitFor();
      assert.equal(await page.getByText("Controller connected", { exact: true }).count(), 0);
      await navigate(page, "kasumi");
      await page.locator('[role="alert"]').getByText(/Kasumi protocol mismatch/i).first().waitFor();
    }, { fixture: { mismatch: true } });

    await runCase(project, "saved-hide-rule-not-active", async (page) => {
      await page.getByText("Saved · not observed as active", { exact: true }).waitFor();
      assert.equal(await page.getByText("Confirmed active", { exact: true }).count(), 0);
      assert.equal(await page.locator(selector("hide-remove")).count(), 1, "A saved rule remains editable when it is not observed active");
    }, { query: "?tab=kasumi&view=hide", fixture: { hidesActive: false } });

    await runCase(project, "disabled-kasumi-retains-inactive-rules", async (page) => {
      await page.getByText("Saved · not observed as active", { exact: true }).waitFor();
      assert.equal(await page.getByText("Confirmed active", { exact: true }).count(), 0);
      await back(page);
      await openView(page, "rules");
      await page.getByText("No active rules reported", { exact: true }).waitFor();
      assert.equal(await page.locator(".rule-details details").count(), 0);
      assert.equal(await page.locator('#page-content [role="alert"]').count(), 0);
    }, { query: "?tab=kasumi&view=hide", fixture: { kasumiDisabled: true } });

    await runCase(project, "empty-modules-wide", async (page) => {
      await page.setViewportSize({ width: 1280, height: 900 });
      await navigate(page, "modules");
      assert.equal(await page.locator(selector("module-detail")).count(), 0);
      assert.ok((await page.locator("#page-content").innerText()).trim().length > 0);
      await assertNoOverflow(page);
    }, { fixture: { empty: true } });
  }
} finally {
  await browser?.close();
  await new Promise((resolve) => server.close(resolve));
  await writeFile(path.join(output, "results.json"), JSON.stringify(results, null, 2) + "\n");
  for (const project of projects) await writeFile(path.join(output, `results-${project}.json`), JSON.stringify(results.filter((result) => result.project === project), null, 2) + "\n");
}

const failed = results.filter((result) => result.status !== "passed");
console.log(`${results.length - failed.length}/${results.length} browser scenarios passed; artifacts: ${output}`);
if (failed.length) process.exitCode = 1;
