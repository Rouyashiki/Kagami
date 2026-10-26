/* SPDX-License-Identifier: Apache-2.0 */

// Runs in the browser before the app. Only the public WebUI X bridge is mocked.
// Commands that the fixture does not implement fail instead of silently passing.
export function installHostFixture(options = {}) {
  const config = {
    moduledir: "/data/adb/modules", tempdir: "", mountsource: "KSU", fs_type: "auto",
    debug: false, verbose: false, lkm_autoload: false, kasumi_enabled: true,
    disable_umount: false, enable_nuke: true, enable_kernel_debug: false,
    enable_stealth: true, enable_overlay_xattr_hide: false, enable_mount_hide: false,
    enable_maps_spoof: false, enable_statfs_spoof: false,
    enable_selinux_fix: false, kasumi_feature_config_version: 3,
    overlayfs_enabled: true, magic_mount_enabled: true, mount_backend: "auto",
    work_dir: "/dev/kagami", mirror_dir: "", mirror_img: "/data/adb/kagami/mirror.img",
    mirror_img_size_mb: 2048, overlay_writable: false, partitions: ["vendor_dlkm"],
    logfile: "/data/adb/kagami/daemon.log", tmpfs_xattr_supported: true,
    kasumi_available: true, policy: { owner: "manual", allow_uids: [10234], deny_uids: [] },
    future_setting: { keep: true },
  };
  if (Array.isArray(options.partitions)) config.partitions = [...options.partitions];
  const modules = [
    { id: "alpha_fonts", name: "Alpha Fonts", version: "1.2", author: "Fixture", description: "System font replacement", mode: "auto", strategy: "overlay", runtime: { state: "mounted", backend: "overlay", reason: null, error: null }, path: "/data/adb/modules/alpha_fonts", rules: [{ path: "/system/fonts", mode: "overlay" }] },
    { id: "beta_audio", name: "Beta Audio", version: "2.0", author: "Fixture", description: "Audio configuration", mode: "kasumi", strategy: "kasumi", runtime: { state: "mounted", backend: "kasumi", reason: null, error: null }, path: "/data/adb/modules/beta_audio", rules: [] },
    { id: "long_module", name: "Long name with <script> markup and a considerably longer description", version: "3.0", author: "Fixture", description: "Long module description to verify wrapping on a narrow screen, with <img src=x onerror=alert(1)> treated as text.", mode: "magic", strategy: "magic", runtime: { state: "mounted", backend: "magic", reason: null, error: null }, path: "/data/adb/modules/long_module", rules: [] },
  ];
  if (Array.isArray(options.modules)) modules.splice(0, modules.length, ...options.modules);
  const fixture = {
    commands: [], unhandled: [], failures: { ...(options.failures || {}) }, config, modules,
    held: [], pending: [], execCalls: 0, spawnCalls: 0,
    hides: ["/data/local/tmp/fixture-hidden"], toasts: [],
    kasumiConflicts: options.kasumiConflicts || [],
    logs: "[INFO] Kagami fixture ready\n[WARN] Fixture warning\n[ERROR] Fixture failure detail\n",
    lkm: { loaded: true, builtin: false, autoload: false, kmi_override: "", detected_kmi: "android14-6.1", asset: "kasumi-android14-6.1.ko", last_error: "", unload: { allowed: true, can_unload: true, reason: "" } },
    system: {
      kernel: "6.1.99-android14-fixture", selinux: "Enforcing", mount_base: "/dev/kagami/mirror",
      kasumi_available: true, kasumi_status: 0, hooks: "vfs_getattr: ready\niterate_dir: ready",
      features: { names: ["STEALTH", "MOUNT_HIDE", "MAPS_SPOOF", ...(options.noBypass ? [] : ["rule_bypass"])], mask: 7 },
      mountStats: { total_mounts: 12, successful_mounts: 12, failed_mounts: 0, files_mounted: 0, dirs_mounted: 0, symlinks_created: 0, overlayfs_mounts: 8, success_rate: 100 },
      detectedPartitions: [{ name: "system", mount_point: "/system", fs_type: "ext4", is_read_only: true }, { name: "vendor_dlkm", mount_point: "/vendor_dlkm", fs_type: "erofs", is_read_only: true }],
      backends: { overlay: { available: true }, magic: { available: true }, kasumi: { available: true } },
    },
  };
  if (options.builtin) Object.assign(fixture.lkm, { loaded: false, builtin: true });
  if (options.unavailable) fixture.system.kasumi_available = false;
  if (options.mismatch) fixture.system.kasumi_available = false;
  if (options.empty) fixture.modules.length = 0;
  globalThis.__kagamiFixture = fixture;
  if (!localStorage.getItem("kagami_static_language")) localStorage.setItem("kagami_static_language", options.language || "en");
  if (!localStorage.getItem("kagami_static_theme")) localStorage.setItem("kagami_static_theme", options.theme || "system");
  if (options.acknowledged !== false) localStorage.setItem("kagami_experimental_ack", "true");

  // The CLI arguments are single quoted by the adapter, using the standard
  // shell '\'' escape for embedded apostrophes. No shell or eval is involved.
  function shellWords(source) {
    return (source.match(/'(?:[^']|'\\'')*'|[^\s]+/g) || []).map((word) =>
      word.startsWith("'") ? word.slice(1, -1).replaceAll("'\\''", "'") : word);
  }
  const json = (value) => ({ errno: 0, stdout: JSON.stringify(value), stderr: "" });
  const ok = (text = "") => ({ errno: 0, stdout: text, stderr: "" });
  function execute(command) {
    const cli = command.replace(/^.*?\/kagamid\s+/, "");
    const fail = Object.entries(fixture.failures).find(([needle, count]) => count && cli.includes(needle));
    if (fail) {
      if (typeof fail[1] === "number") fixture.failures[fail[0]] -= 1;
      return { errno: 1, stdout: "", stderr: `Fixture failure: ${fail[0]}` };
    }
    const args = shellWords(cli);
    const verb = args.slice(0, 2).join(" ");
    if (verb === "config show") return json(config);
    if (verb === "kasumi conflicts") return json({ events: fixture.kasumiConflicts });
    if (verb === "kasumi ack-conflicts") {
      fixture.kasumiConflicts = fixture.kasumiConflicts.filter(event => BigInt(event.sequence) > BigInt(args[2]));
      return ok();
    }
    if (verb === "config merge-json") { Object.assign(config, JSON.parse(args[2])); return ok(); }
    if (verb === "config apply") return ok();
    if (verb === "config sync-partitions") return args.includes("--json")
      ? json({ partitions: ["vendor_dlkm", "odm_dlkm"], invalid: options.invalidPartitions || [] })
      : ok("Added partition: vendor_dlkm\nAdded partition: odm_dlkm\n");
    if (verb === "api meta") return json({ module_id: "kagami", metamodule: true, version: "v9.8.7-fixture", data_dir: "/data/adb/kagami", modules_dir: config.moduledir, config_file: "/data/adb/kagami/config.json", log_file: config.logfile });
    if (verb === "api system") return json(fixture.system);
    if (verb === "api storage") return json({ size: "2.0G", used: "256M", avail: "1.7G", percent: 13, mode: "ext4" });
    if (verb === "api lkm") return json(fixture.lkm);
    if (verb === "kasumi version") return json({ kasumi_available: fixture.system.kasumi_available, protocol_version: 18, kernel_version: options.mismatch ? 17 : 18, protocol_mismatch: Boolean(options.mismatch), mismatch_message: options.mismatch ? "Kasumi protocol mismatch" : "", active_modules: options.mismatch ? [] : ["beta_audio"], mount_base: fixture.system.mount_base });
    if (verb === "kasumi list") return json([
      { type: "KASUMI", args: "Protocol: 18" },
      { type: "KASUMI", args: `Enabled: ${options.kasumiDisabled ? 0 : 1}` },
      ...(options.hidesActive === false ? [] : fixture.hides.map((path) => ({ type: "HIDE", path }))),
      { type: "MERGE", target: "/system/etc/audio", source: "/data/adb/modules/beta_audio/system/etc/audio" },
      { type: "STEALTH", args: "enabled" },
      { type: "MOUNT_HIDE", args: "enabled" },
    ]);
    if (verb === "kasumi disable") return ok();
    if (verb === "module list") return json({ modules: args.includes("--include-inactive") ? modules : modules.filter((item) => !item.inactive) });
    if (verb === "module check-conflicts") return json([{ file: "/system/etc/shared.xml", modules: ["alpha_fonts", "beta_audio"] }]);
    if (verb === "module set-bypass") { modules.find(item => item.id === args[2]).bypass = args[3] === "on"; return ok(); }
    if (verb === "module set-mode") { modules.find((item) => item.id === args[2]).mode = args[3]; return ok(); }
    if (verb === "module add-rule") {
      const module = modules.find((item) => item.id === args[2]);
      const old = module.rules.find((rule) => rule.path === args[3]);
      if (old) { old.mode = args[4]; old.bypass = args.includes("--bypass"); } else module.rules.push({ path: args[3], mode: args[4], bypass: args.includes("--bypass") });
      return ok();
    }
    if (verb === "module remove-rule") { const module = modules.find((item) => item.id === args[2]); module.rules = module.rules.filter((rule) => rule.path !== args[3]); return ok(); }
    if (verb === "hide list") return json(args.includes("--details") ? fixture.hides.map(path => ({ path, bypass: fixture.hideBypasses?.[path] === true })) : fixture.hides);
    if (verb === "hide add") { fixture.hideBypasses ??= {}; fixture.hideBypasses[args[2]] = args.includes("--bypass"); if (!fixture.hides.includes(args[2])) fixture.hides.push(args[2]); return ok(); }
    if (verb === "hide remove") { fixture.hides = fixture.hides.filter((path) => path !== args[2]); return ok(); }
    if (verb === "lkm set-autoload") { fixture.lkm.autoload = args[2] === "on"; config.lkm_autoload = fixture.lkm.autoload; return ok(); }
    if (verb === "lkm set-kmi") { fixture.lkm.kmi_override = args[2]; return ok(); }
    if (verb === "lkm clear-kmi") { fixture.lkm.kmi_override = ""; return ok(); }
    if (verb === "lkm load") { fixture.lkm.loaded = true; return ok(); }
    if (verb === "lkm unload") { fixture.lkm.loaded = false; return ok(); }
    if (command.includes("dmesg")) return ok("[0.1] other kernel message\n[1.0] kasumi: fixture initialized\n");
    if (command.includes("tail -n")) return ok(fixture.logs);
    if (command.includes("echo -n") || command.includes("truncate") || command.startsWith(": >") || command.startsWith("printf '' >")) { fixture.logs = ""; return ok(); }
    fixture.unhandled.push(command);
    return { errno: 127, stdout: "", stderr: `Unhandled fixture command: ${command}` };
  }
  globalThis.ksu = {
    exec(command, _options, callbackName) {
      fixture.execCalls++;
      if (!options.legacyExec) throw new Error("KernelSU exec blocks the WebView; use spawn");
      fixture.commands.push(command);
      setTimeout(() => {
        let result;
        try { result = execute(command); }
        catch (error) { result = { errno: 1, stdout: "", stderr: `Fixture command error: ${error.message}` }; }
        globalThis[callbackName]?.(result.errno, result.stdout, result.stderr);
      }, options.delayMs || 8);
    },
    toast(message) { fixture.toasts.push(String(message)); },
    enableEdgeToEdge() {}, enableInsets() {}, fullScreen() {},
  };
  if (!options.legacyExec) globalThis.ksu.spawn = (command, args, _options, reference) => {
    fixture.spawnCalls++;
    if (JSON.stringify(JSON.parse(args)) !== "[]") throw new Error("The bridge must preserve the complete quoted shell command");
    fixture.commands.push(command);
    // The host evaluates this app-generated callback reference for every event.
    // execute() above interprets fixture commands without executing shell input.
    const callback = () => Function(`return ${reference}`)();
    const respond = () => {
      let result;
      try { result = execute(command); }
      catch (error) { result = { errno: 1, stdout: "", stderr: `Fixture command error: ${error.message}` }; }
      for (const stream of ["stdout", "stderr"]) {
        if (result[stream]) for (const line of result[stream].split("\n")) callback()[stream].emit("data", line);
      }
      callback().emit("exit", result.errno);
      if (result.errno) callback().emit("error", { exitCode: result.errno, message: result.stderr });
    };
    if (fixture.held.some(needle => command.includes(needle))) fixture.pending.push(respond);
    else setTimeout(respond, options.delayMs || 8);
  };
  fixture.release = () => { fixture.held.length = 0; fixture.pending.splice(0).forEach(respond => respond()); };
}
