import assert from "node:assert/strict";
import { afterEach, test } from "node:test";

const apiUrl = new URL("../../../webui/api.js", import.meta.url);
const bridgeUrl = new URL("../../../webui/assets/kernelsu.js", import.meta.url);
let importCounter = 0;

afterEach(() => {
  delete globalThis.ksu;
  delete globalThis.location;
});

// Parse the POSIX quoting used by the actual bridge commands. In particular,
// adjacent quoted/unquoted segments must remain one argument for apostrophes.
function shellWords(command) {
  const words = [];
  let word = "", quote = "", started = false;
  for (let i = 0; i < command.length; i += 1) {
    const ch = command[i];
    if (quote) {
      if (ch === quote) quote = "";
      else word += ch;
      started = true;
    } else if (ch === "'" || ch === '"') {
      quote = ch;
      started = true;
    } else if (ch === "\\") {
      word += command[++i];
      started = true;
    } else if (/\s/.test(ch)) {
      if (started) words.push(word);
      word = "";
      started = false;
    } else {
      word += ch;
      started = true;
    }
  }
  assert.equal(quote, "", "balanced shell quoting");
  if (started) words.push(word);
  return words;
}

function fixture() {
  const data = {
    config: { moduledir: "/data/adb/modules", debug: false, enable_hidexattr: true, custom: { keep: 1 } },
    modes: { visible: "auto", disabled: "magic" },
    rules: { visible: [{ path: "/system/etc/old", mode: "overlay" }], disabled: [{ path: "/system/hidden", mode: "none" }] },
    saved: ["/system/saved"],
    active: [
      { type: "KASUMI", args: "Protocol: 17" },
      { type: "KASUMI", args: "Enabled: 1" },
      { type: "HIDE", path: "/system/active" },
      { type: "STEALTH", args: "enabled" },
    ],
    runtime: { state: "mounted", backend: "kasumi", reason: null, error: null },
    system: { kernel: "6.1-test", selinux: "Enforcing", kasumi_available: true, kasumi_status: 0, backends: [], mountStats: { total_mounts: 2, overlayfs_mounts: 1 } },
    lkm: { loaded: true, builtin: false, autoload: false, kmi_override: "", detected_kmi: "android14-6.1", asset: "/asset.ko", last_error: "", unload: { attempted: true, quiesce_supported: true, busy_mask: 4 } },
    commands: [],
    failures: new Map(),
  };
  data.run = (command) => {
    data.commands.push(command);
    const args = shellWords(command);
    const action = args[0].endsWith("/kagamid") ? args.slice(1, 3).join(" ") : args[0];
    if (data.failures.has(action)) return data.failures.get(action);
    let value;
    switch (action) {
      case "config show": value = data.config; break;
      case "config merge-json": Object.assign(data.config, JSON.parse(args[3])); return { stdout: "" };
      case "config apply": return { stdout: "{}" };
      case "kasumi disable": return { stdout: "" };
      case "api meta": value = { module_id: "kagami", version: "v9.9-test", log_file: "/actual.log" }; break;
      case "api system": value = data.system; break;
      case "api lkm": value = data.lkm; break;
      case "api storage": value = { size: "2G", used: "1G", avail: "1G", percent: 50, mode: "tmpfs" }; break;
      case "kasumi version": value = { protocol_mismatch: false, active_modules: ["visible"] }; break;
      case "module list": value = { modules: data.modules || [{ id: "visible", mode: data.modes.visible, strategy: "kasumi", runtime: data.runtime, rules: data.rules.visible }] }; break;
      case "module set-mode": data.modes[args[3]] = args[4]; return { stdout: "" };
      case "module add-rule": {
        const rules = data.rules[args[3]] ||= [];
        const existing = rules.find((rule) => rule.path === args[4]);
        if (existing) existing.mode = args[5];
        else rules.push({ path: args[4], mode: args[5] });
        return { stdout: "" };
      }
      case "module remove-rule": data.rules[args[3]] = (data.rules[args[3]] || []).filter((rule) => rule.path !== args[4]); return { stdout: "" };
      case "module check-conflicts": value = [{ file: "/system/etc/a", modules: ["visible", "disabled"] }]; break;
      case "hide list": value = data.saved; break;
      case "hide add": data.saved.push(args[3]); return { stdout: "" };
      case "hide remove": data.saved = data.saved.filter((path) => path !== args[3]); return { stdout: "" };
      case "kasumi list": value = data.active; break;
      case "config sync-partitions": return { stdout: args.includes("--json")
        ? JSON.stringify({ partitions: ["vendor_dlkm"], invalid: ["zygisk", "lkm"] })
        : "Added partition: vendor_dlkm\n" };
      case "tail": return { stdout: "a\nb\n" };
      case "dmesg": return { stdout: "other kernel line\nKasumi first\nKAGAMIFS second\n" };
      default: throw new Error(`Unexpected command: ${command}`);
    }
    return { stdout: JSON.stringify(value) };
  };
  return data;
}

async function loadApi(handler, search = "") {
  globalThis.location = { search };
  if (handler) {
    globalThis.ksu = { exec(command, options, callback) {
      assert.deepEqual(JSON.parse(options), {});
      Promise.resolve().then(() => handler(command)).then((result) => {
        globalThis[callback]?.(result.errno ?? 0, result.stdout ?? "", result.stderr ?? "");
      }, (error) => globalThis[callback]?.(1, "", error.message));
    } };
  } else delete globalThis.ksu;
  return import(`${apiUrl.href}?case=${++importCounter}`);
}

test("configuration reads migrate legacy features without replacing unrelated saved keys", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  const config = await api.loadConfig();
  assert.equal(config.enable_mount_hide, true);
  assert.equal(config.enable_maps_spoof, true);
  assert.equal(config.kasumi_feature_config_version, 3);
  assert.equal(config.enable_hidexattr, undefined);
  assert.deepEqual(config.custom, { keep: 1 });
  assert.equal(config.__lkmAutoloadPresent, false);
  await api.saveConfig({ debug: true, lkm_autoload: true, kasumi_available: false });
  const merge = data.commands.find((command) => command.includes("config merge-json"));
  assert.deepEqual(JSON.parse(shellWords(merge)[3]), { debug: true });
  assert.deepEqual(data.config.custom, { keep: 1 });
});

test("configuration persistence and runtime apply failure remain distinct", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  data.failures.set("config apply", { errno: 1, stderr: "kernel rejected apply" });
  await assert.rejects(api.saveConfig({ enable_mount_hide: true }), (error) => error.configPersisted === true && /kernel rejected/.test(error.message));
  assert.equal(data.config.enable_mount_hide, true);
  assert.equal(data.config.kasumi_feature_config_version, 3);
  assert.match(data.commands.at(-1), /kasumi disable$/);
  data.failures.set("config merge-json", { errno: 1, stderr: "read-only filesystem" });
  await assert.rejects(api.saveConfig({ debug: true }), (error) => !error.configPersisted && /read-only/.test(error.message));
});

test("invalid runtime read after a successful save reports persisted state", async () => {
  const data = fixture();
  data.failures.set("api system", { stdout: "{}" });
  const { api } = await loadApi(data.run);
  await assert.rejects(api.saveConfig({ debug: true }), (error) => error.configPersisted === true);
});

test("read failures and malformed data never become default configuration, empty lists, or unloaded status", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  for (const [action, method] of [["config show", "loadConfig"], ["module list", "scanModules"], ["module check-conflicts", "checkConflicts"], ["api lkm", "getLkmStatus"], ["hide list", "getUserHideRules"], ["api system", "getSystemInfo"]]) {
    data.failures.set(action, { errno: 13, stderr: "permission denied" });
    await assert.rejects(api[method](), /permission denied/, method);
    data.failures.set(action, { stdout: "not-json" });
    await assert.rejects(api[method](), /invalid/i, method);
    data.failures.delete(action);
  }
  data.failures.set("module list", { stdout: "{}" });
  await assert.rejects(api.scanModules(), /invalid/i);
});

test("incremental module commands retain invisible modules and preserve quoted path arguments", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  const path = "/system/etc/a'b $(touch /bad); `other`";
  assert.equal((await api.scanModules()).length, 1);
  await api.setModuleMode("visible", "magic");
  await api.addModuleRule("visible", path, "hide");
  await api.addModuleRule("visible", path, "none");
  assert.equal(data.rules.visible.filter((rule) => rule.path === path).length, 1);
  assert.equal(data.rules.visible.at(-1).mode, "none");
  await api.removeModuleRule("visible", path);
  assert.deepEqual(data.rules.disabled, [{ path: "/system/hidden", mode: "none" }]);
  assert.equal(data.modes.disabled, "magic");
  assert.ok(data.commands.every((command) => !command.includes(" > ")));
  const add = shellWords(data.commands.find((command) => command.includes("module add-rule")));
  assert.equal(add.length, 6);
  assert.equal(add[4], path);
});

test("module runtime retains every backend and state independently of configured mode", async () => {
  const data = fixture();
  data.modules = ["overlay", "magic", "kasumi"].flatMap((backend) => ["mounted", "unmounted", "partial"].map((state) => ({
    id: `${backend}_${state}`, mode: "none", strategy: "none", runtime: { state, backend, reason: null, error: null },
  })));
  data.modules.push({ id: "disabled", mode: "kasumi", runtime: { state: "unmounted", backend: "none", reason: "disabled", error: null } });
  const { api } = await loadApi(data.run);
  const modules = await api.scanModules();
  assert.deepEqual(modules.map((item) => item.runtime), data.modules.map((item) => item.runtime));
  assert.equal(modules.at(-1).id, "disabled");
  assert.deepEqual(shellWords(data.commands.at(-1)).slice(1), ["module", "list", "--include-inactive"]);
  data.modules = undefined;
  await api.setModuleMode("visible", "magic");
  const changed = (await api.scanModules())[0];
  assert.equal(changed.mode, "magic");
  assert.deepEqual(changed.runtime, data.runtime, "A saved next-boot mode cannot replace current runtime evidence");
});

test("legacy module metadata and failed runtime reads cannot imply a mounted module", async () => {
  const data = fixture();
  const readError = { code: "READ_FAILED", message: "Cannot read mountinfo" };
  data.modules = [
    { id: "visible", mode: "kasumi", strategy: "kasumi" },
    { id: "missing", mode: "magic", runtime: { state: "unknown", backend: null, reason: "missing_snapshot", error: null } },
    { id: "failed", mode: "overlay", runtime: { state: "mounted", backend: "overlay", reason: null, error: readError } },
  ];
  const { api } = await loadApi(data.run);
  const [legacy, missing, failed] = await api.scanModules();
  assert.deepEqual(legacy.runtime, { state: "unknown", backend: null, reason: null, error: null });
  assert.equal(missing.runtime.state, "unknown");
  assert.equal(missing.runtime.error, null);
  assert.equal(failed.runtime.state, "unknown");
  assert.deepEqual(failed.runtime.error, readError);
  assert.equal(data.commands.some((command) => command.includes("kasumi version")), false, "Kasumi membership is not module runtime evidence");
  for (const runtime of [false, [], { state: "mounted", backend: "auto" }, { state: "yes", backend: "overlay" }, { state: "unknown", error: "bad" }]) {
    data.modules = [{ id: "invalid", runtime }];
    await assert.rejects(api.scanModules(), /invalid/i);
  }
});

test("module edits serialize across modules and a failure does not poison later edits", async () => {
  const data = fixture();
  let running = 0, maxRunning = 0;
  const { api } = await loadApi(async (command) => {
    running += 1;
    maxRunning = Math.max(maxRunning, running);
    await new Promise((resolve) => setTimeout(resolve, 5));
    running -= 1;
    return data.run(command);
  });
  data.failures.set("module set-mode", { errno: 1, stderr: "save failed" });
  const first = api.setModuleMode("visible", "magic");
  const second = api.addModuleRule("disabled", "/system/new", "none");
  await assert.rejects(first, /save failed/);
  await second;
  assert.equal(maxRunning, 1);
  assert.equal(data.rules.disabled.at(-1).path, "/system/new");
});

test("invalid module identifiers and rule input never reach the root bridge", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  await assert.rejects(api.setModuleMode("../escape", "magic"), /Invalid/);
  await assert.rejects(api.addModuleRule("visible", "relative", "hide"), /absolute/);
  await assert.rejects(api.addModuleRule("visible", "/system/file\nother", "hide"), /line breaks/);
  await assert.rejects(api.addModuleRule("visible", "/system/file", "auto"), /Invalid/);
  assert.equal(data.commands.length, 0);
});

test("saved hide rules and observed active rules remain separate", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  assert.deepEqual(await api.addUserHideRule("/system/pending"), { persisted: true });
  assert.ok((await api.loadUserHideRules()).includes("/system/pending"));
  const active = await api.loadAllRules();
  assert.equal(active.length, 1);
  assert.equal(active[0].path, "/system/active");
  assert.equal(active[0].active, true);
  assert.equal(active[0].isUserDefined, false);
  data.system.kasumi_available = false;
  await assert.rejects(api.getAllRules(), /unavailable/);
  assert.ok((await api.getUserHideRules()).includes("/system/pending"));
});

test("native rule responses separate path rules from protocol and feature metadata", async () => {
  const data = fixture();
  data.active.push(
    { type: "ADD", target: "/system/etc/config", source: "/custom/modules/example/system/etc/config" },
    { type: "MERGE", target: "/system/fonts", source: "/custom/modules/example/system/fonts" },
    { type: "HIDE", path: "/system/saved" },
    { type: "INJECT", args: "/custom/modules/example" },
    { type: "MOUNT_HIDE", args: "enabled mode=normal" },
    { type: "MAPS_SPOOF", args: "enabled" },
    { type: "STATFS_SPOOF", args: "enabled" },
    { type: "SELINUX_FIX", args: "enabled" },
    { type: "HIDE_XATTR_SB", args: "000000001234abcd" },
  );
  const { api } = await loadApi(data.run);
  const active = await api.getAllRules();
  assert.deepEqual(active.map(({ type, path, isUserDefined }) => ({ type, path, isUserDefined })), [
    { type: "HIDE", path: "/system/active", isUserDefined: false },
    { type: "ADD", path: "/system/etc/config", isUserDefined: false },
    { type: "MERGE", path: "/system/fonts", isUserDefined: false },
    { type: "HIDE", path: "/system/saved", isUserDefined: true },
  ]);
  assert.ok(active.every((rule) => rule.active));
  assert.equal(active[1].source, "/custom/modules/example/system/etc/config");
  data.active = data.active.filter(({ type }) => !["HIDE", "ADD", "MERGE"].includes(type));
  assert.deepEqual(await api.getAllRules(), [], "Metadata alone is a successful empty rule list");
});

test("disabled Kasumi rules are not active and headerless responses remain compatible", async () => {
  const data = fixture();
  data.active[1].args = "Enabled: 0";
  const { api } = await loadApi(data.run);
  assert.deepEqual(await api.getAllRules(), [], "Disabled kernels may retain their rule table");
  data.active = [{ type: "HIDE", path: "/system/active" }];
  assert.equal((await api.getAllRules())[0].active, true);
});

test("malformed path rules remain read failures instead of successful empty results", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  for (const rule of [
    { type: "HIDE" },
    { type: "HIDE", path: "relative" },
    { type: "ADD", target: "/system/file" },
    { type: "MERGE", target: 42, source: "/custom/modules/example" },
    { path: "/system/file" },
    null,
  ]) {
    data.active = [{ type: "KASUMI", args: "Protocol: 17" }, rule];
    await assert.rejects(api.getAllRules(), /invalid/i);
  }
});

test("LKM native fields and installed metadata are retained", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  assert.deepEqual(await api.getLkmStatus(), { ...data.lkm, valid: true });
  assert.equal((await api.getMeta()).version, "v9.9-test");
  data.lkm = { builtin: true, available: true, external_management: false };
  const builtin = await api.getLkmStatus();
  assert.equal(builtin.external_management, false);
  assert.equal(builtin.loaded, undefined);
  const system = await api.getSystemInfo();
  assert.equal(system.kernel, "6.1-test");
  assert.equal(system.selinux, "Enforcing");
});

test("log and partition failures propagate while successful empty results stay empty", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  assert.equal(await api.readLogs("kernel", 1), "KAGAMIFS second");
  for (const [action, read] of [["dmesg", () => api.readLogs("kernel")], ["tail", () => api.readLogs("system")], ["config sync-partitions", () => api.scanPartitionsFromModules()]]) {
    data.failures.set(action, { errno: 1, stderr: "read failed" });
    await assert.rejects(read(), /read failed/);
    data.failures.set(action, { stdout: "" });
    assert.deepEqual(await read(), action === "config sync-partitions" ? [] : "");
  }
  await assert.rejects(api.readLogs("system", "1; id"), /Invalid/);
});

test("partition cleanup requires a complete validated scan response", async () => {
  const data = fixture();
  const { api } = await loadApi(data.run);
  assert.deepEqual(await api.getPartitionScan(), { partitions: ["vendor_dlkm"], invalid: ["zygisk", "lkm"] });
  for (const response of [{}, { partitions: [], invalid: null }, { partitions: ["../data"], invalid: [] }, { partitions: [], invalid: [7] }]) {
    data.failures.set("config sync-partitions", { stdout: JSON.stringify(response) });
    await assert.rejects(api.getPartitionScan(), /invalid/i);
  }
  data.failures.set("config sync-partitions", { stdout: JSON.stringify({ partitions: [], invalid: [] }) });
  assert.deepEqual(await api.getPartitionScan(), { partitions: [], invalid: [] });
  data.failures.set("config sync-partitions", { errno: 1, stderr: "mount table unreadable" });
  await assert.rejects(api.getPartitionScan(), /mount table unreadable/);
});

test("bridge failure, invalid status and timeouts reject and clean up callbacks", async () => {
  const { exec } = await import(bridgeUrl.href);
  delete globalThis.ksu;
  await assert.rejects(exec("command"), /unavailable/);
  let callbackName;
  for (const status of ["bad", "", null, undefined]) {
    globalThis.ksu = { exec(_command, _options, callback) { callbackName = callback; globalThis[callback](status, "", ""); } };
    await assert.rejects(exec("command"), /invalid exit status/);
    assert.equal(globalThis[callbackName], undefined);
  }
  globalThis.ksu = { exec(_command, _options, callback) { callbackName = callback; } };
  await assert.rejects(exec("command", {}, 5), (error) => error.code === "BRIDGE_TIMEOUT" && error.outcomeUnknown === true);
  assert.equal(globalThis[callbackName], undefined);
});

// KernelSU's three-argument exec still runs Shell.Job.exec synchronously.
// Only spawn enqueues the job before returning to the WebView's JS thread.
test("bridge uses asynchronous spawn and preserves command, lines and exit status", async () => {
  const { exec } = await import(bridgeUrl.href);
  let callback;
  globalThis.ksu = {
    exec() { throw new Error("synchronous exec must not run"); },
    spawn(command, args, options, reference) {
      assert.equal(command, "printf '%s' 'a; b' && false");
      assert.deepEqual(JSON.parse(args), []);
      assert.deepEqual(JSON.parse(options), { cwd: "/data/local/tmp" });
      callback = () => Function(`return ${reference}`)();
    },
  };
  const pending = exec("printf '%s' 'a; b' && false", { cwd: "/data/local/tmp" });
  assert.equal(typeof callback, "function");
  callback().stdout.emit("data", "line one");
  callback().stdout.emit("data", "");
  callback().stdout.emit("data", "line three");
  callback().stderr.emit("data", "failure");
  callback().emit("exit", 7);
  assert.deepEqual(await pending, { errno: 7, stdout: "line one\n\nline three", stderr: "failure" });
  // Official hosts emit error after a nonzero exit, in a later JS dispatch.
  assert.doesNotThrow(() => callback().emit("error", { exitCode: 7, message: "failure" }));
});

test("spawn timeout releases the UI and safely ignores late host events", async () => {
  const { exec } = await import(bridgeUrl.href);
  let callback;
  const before = Object.keys(globalThis).filter(key => key.startsWith("exec_callback_"));
  globalThis.ksu = { spawn(_command, _args, _options, reference) { callback = () => Function(`return ${reference}`)(); } };
  await assert.rejects(exec("slow query", {}, 5), error => error.code === "BRIDGE_TIMEOUT" && error.outcomeUnknown);
  assert.doesNotThrow(() => { callback().stdout.emit("data", "late"); callback().emit("exit", 0); });
  assert.deepEqual(Object.keys(globalThis).filter(key => key.startsWith("exec_callback_")), before);
});

test("mock implements incremental editing, metadata and persisted versus active hide state", async () => {
  const { api, getRuntimeMode } = await loadApi(undefined, "?mock=1");
  assert.equal(getRuntimeMode(), "mock");
  const modules = await api.scanModules();
  assert.deepEqual(modules[0].runtime, { state: "mounted", backend: "overlay", reason: null, error: null });
  await api.setModuleMode(modules[0].id, "magic");
  assert.deepEqual((await api.scanModules())[0].runtime, modules[0].runtime);
  await api.addModuleRule(modules[0].id, "/system/new", "hide");
  await api.addModuleRule(modules[0].id, "/system/new", "none");
  assert.equal((await api.scanModules())[0].rules.filter((rule) => rule.path === "/system/new").length, 1);
  assert.equal((await api.getMeta()).version, "Preview");
  await api.lkmUnload();
  assert.deepEqual(await api.addUserHideRule("/system/pending"), { persisted: true });
  assert.ok((await api.getUserHideRules()).includes("/system/pending"));
  await assert.rejects(api.getAllRules(), /unavailable/);
});
