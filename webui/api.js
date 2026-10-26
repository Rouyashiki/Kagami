import { exec, hasKernelSU } from "./assets/kernelsu.js";

export const PATHS = {
  BINARY: "/data/adb/modules/kagami/kagamid",
  CONFIG: "/data/adb/kagami/config.json",
  MODE_CONFIG: "/data/adb/kagami/module_mode.json",
  RULES_CONFIG: "/data/adb/kagami/module_rules.json",
  DEFAULT_LOG: "/data/adb/kagami/daemon.log",
};

export const KASUMI_FEATURE_CONFIG_VERSION = 3;

export const DEFAULT_CONFIG = {
  moduledir: "/data/adb/modules",
  tempdir: "",
  mountsource: "KSU",
  logfile: PATHS.DEFAULT_LOG,
  debug: false,
  verbose: false,
  lkm_autoload: false,
  fs_type: "auto",
  work_dir: "/dev/kagami",
  mirror_dir: "",
  mirror_img: "/data/adb/kagami/mirror.img",
  mirror_img_size_mb: 2048,
  overlay_writable: false,
  disable_umount: false,
  enable_nuke: true,
  enable_kernel_debug: false,
  enable_stealth: true,
  kasumi_feature_config_version: KASUMI_FEATURE_CONFIG_VERSION,
  enable_overlay_xattr_hide: false,
  enable_mount_hide: false,
  enable_maps_spoof: false,
  enable_statfs_spoof: false,
  enable_selinux_fix: false,
  kasumi_enabled: true,
  overlayfs_enabled: true,
  magic_mount_enabled: true,
  mount_backend: "auto",
  policy: {
    owner: "auto",
    use_allow_uids: false,
    use_deny_uids: false,
    include_isolated_uids: false,
    allow_uids: [],
    deny_uids: [],
  },
  partitions: [],
  kasumi_available: false,
  tmpfs_xattr_supported: false,
};

export const BUILTIN_PARTITIONS = [
  "system",
  "vendor",
  "product",
  "system_ext",
  "odm",
  "oem",
];

export const MODULE_META = {
  name: "Kagami",
  version: "",
  author: "Anatdx",
};

const urlParams = new URLSearchParams(globalThis.location?.search || "");
const forceMock = urlParams.get("mock") === "1";
const runtimeMode = forceMock || !hasKernelSU() ? "mock" : "live";

export function getRuntimeMode() {
  return runtimeMode;
}

function apiOutput(result) {
  return String((result && (result.stderr || result.stdout)) || "").trim();
}

function shellEscape(value) {
  return String(value ?? "").replace(/'/g, "'\\''");
}

function configPersistedError(message) {
  const error = new Error(message);
  error.configPersisted = true;
  return error;
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function hasOwn(value, key) {
  return Object.prototype.hasOwnProperty.call(value, key);
}

function normalizeConfig(rawConfig) {
  const raw = rawConfig && typeof rawConfig === "object" ? clone(rawConfig) : {};
  delete raw.mount_hide_mode;
  const splitKeys = [
    "enable_overlay_xattr_hide",
    "enable_mount_hide",
    "enable_maps_spoof",
    "enable_statfs_spoof",
  ];
  const hasSplitFeatures =
    Number(raw.kasumi_feature_config_version || 0) >= 2 ||
    splitKeys.some((key) => hasOwn(raw, key));

  if (hasSplitFeatures) {
    raw.kasumi_feature_config_version = KASUMI_FEATURE_CONFIG_VERSION;
    delete raw.enable_hidexattr;
    return raw;
  }

  const legacy = raw.enable_hidexattr === true;
  const normalized = {
    ...raw,
    kasumi_feature_config_version: KASUMI_FEATURE_CONFIG_VERSION,
    enable_overlay_xattr_hide: legacy,
    enable_mount_hide: legacy,
    enable_maps_spoof: legacy,
    enable_statfs_spoof: legacy,
    enable_selinux_fix: legacy || raw.enable_selinux_fix === true,
    enable_stealth: Boolean(raw.enable_stealth ?? DEFAULT_CONFIG.enable_stealth) || legacy,
  };
  delete normalized.enable_hidexattr;
  return normalized;
}

async function checkedExec(command) {
  const result = await exec(command);
  if (result.errno !== 0) {
    const error = new Error(apiOutput(result) || `Command failed (${result.errno})`);
    error.errno = result.errno;
    throw error;
  }
  return result;
}

async function execJson(command) {
  const result = await checkedExec(command);
  try {
    return JSON.parse(result.stdout);
  } catch (_error) {
    throw new Error("The command returned an invalid JSON response");
  }
}

function objectResponse(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("The command returned an invalid object response");
  }
  return value;
}

function arrayResponse(value) {
  if (!Array.isArray(value)) throw new Error("The command returned an invalid list response");
  return value;
}

function conflictSequence(value) {
  if (typeof value !== "string" || !/^(0|[1-9][0-9]{0,19})$/.test(value) || BigInt(value) > 18446744073709551615n) {
    throw new Error("Invalid conflict sequence");
  }
  return value;
}

function validModuleId(id) {
  if (typeof id !== "string" || !/^[a-zA-Z0-9_.-]+$/.test(id) || id === "." || id === "..") {
    throw new Error("Invalid module ID");
  }
}

function validPath(path) {
  if (typeof path !== "string" || !path.startsWith("/") || /[\0\r\n]/.test(path)) {
    throw new Error("The path must be absolute and contain no line breaks");
  }
}

function validMode(mode, rule = false) {
  const modes = rule ? ["kasumi", "overlay", "magic", "none", "hide"] : ["auto", "kasumi", "overlay", "magic", "none"];
  if (!modes.includes(mode)) throw new Error("Invalid mount mode");
}

function moduleRuntime(value) {
  // Older controllers expose a configured strategy but no runtime evidence.
  if (value == null) return { state: "unknown", backend: null, reason: null, error: null };
  objectResponse(value);
  const { state, backend = null, reason = null, error = null } = value;
  if (!["mounted", "unmounted", "partial", "unknown"].includes(state)
      || !["overlay", "magic", "kasumi", "none", null].includes(backend)
      || (reason !== null && typeof reason !== "string")
      || (error !== null && (typeof error !== "object" || Array.isArray(error)
        || typeof error.code !== "string" || typeof error.message !== "string"))) {
    throw new Error("Invalid module runtime response");
  }
  return { state: error ? "unknown" : state, backend, reason, error };
}

function configPatch(updates) {
  const patch = clone(objectResponse(updates || {}));
  for (const key of ["lkm_autoload", "__lkmAutoloadPresent", "kasumi_available", "tmpfs_xattr_supported"]) delete patch[key];
  if (Object.keys(patch).some((key) => (key.startsWith("enable_") && key !== "enable_nuke"))) {
    patch.kasumi_feature_config_version = KASUMI_FEATURE_CONFIG_VERSION;
  }
  return patch;
}

// Native mode/rule commands read and rewrite shared maps. Serialize all edits,
// including edits to different modules, without blocking later retries on failure.
let moduleWriteQueue = Promise.resolve();
function queueModuleWrite(task) {
  const result = moduleWriteQueue.then(task);
  moduleWriteQueue = result.catch(() => undefined);
  return result;
}

const mockState = {
  config: {
    ...clone(DEFAULT_CONFIG),
    tempdir: "/data/adb/kagami/img_mnt",
    kasumi_available: true,
    tmpfs_xattr_supported: true,
    partitions: ["system", "vendor", "product"],
    enable_overlay_xattr_hide: true,
    enable_mount_hide: true,
    enable_maps_spoof: true,
    enable_statfs_spoof: true,
    enable_selinux_fix: true,
  },
  modules: [
    {
      id: "zygisk_next",
      name: "Zygisk Next",
      version: "1.2.6",
      author: "Dr-TSNG",
      description: "Inject zygisk into the current runtime.",
      mode: "overlay",
      strategy: "overlay",
      runtime: { state: "mounted", backend: "overlay", reason: null, error: null },
      path: "/data/adb/modules/zygisk_next",
      rules: [{ path: "/system/bin/app_process64", mode: "overlay" }],
    },
    {
      id: "playintegrityfix",
      name: "PlayIntegrityFix",
      version: "17.4",
      author: "chiteroman",
      description: "Spoof a safer device profile and properties.",
      mode: "kasumi",
      strategy: "kasumi",
      runtime: { state: "mounted", backend: "kasumi", reason: null, error: null },
      path: "/data/adb/modules/playintegrityfix",
      rules: [
        { path: "/system/etc/security", mode: "kasumi" },
        { path: "/system/bin/keystore2", mode: "kasumi" },
      ],
    },
  ],
  storage: {
    size: "768M",
    used: "183M",
    avail: "585M",
    percent: 23.8,
    mode: "tmpfs",
  },
  systemInfo: {
    kernel: "6.1.89-android14-gki",
    selinux: "Permissive",
    mountBase: "/mnt/ab12cd34ef567890",
    kasumiAvailable: true,
    kasumiStatus: 0,
    kasumiModules: ["playintegrityfix"],
    kasumiMismatch: false,
    hooks:
      "GET_FD: tracepoint(sys_enter/sys_exit)\npath: tracepoint(sys_enter)\nvfs_getattr,d_path,iterate_dir,vfs_getxattr: ftrace+kretprobe",
    features: {
      bitmask: 0x1e5,
      names: [
        "rule_bypass",
        "mount_hide",
        "maps_spoof",
        "statfs_spoof",
        "kstat_spoof",
        "merge_dir",
      ],
    },
    mountStats: {
      total_mounts: 58,
      successful_mounts: 57,
      failed_mounts: 1,
      tmpfs_created: 4,
      files_mounted: 29,
      dirs_mounted: 17,
      symlinks_created: 11,
      overlayfs_mounts: 5,
      success_rate: 98.3,
    },
    detectedPartitions: [
      { name: "system", mount_point: "/system", fs_type: "erofs", is_read_only: true, exists_as_symlink: false },
      { name: "vendor", mount_point: "/vendor", fs_type: "erofs", is_read_only: true, exists_as_symlink: true },
      { name: "product", mount_point: "/product", fs_type: "erofs", is_read_only: true, exists_as_symlink: true },
      { name: "system_ext", mount_point: "/system_ext", fs_type: "erofs", is_read_only: true, exists_as_symlink: false },
    ],
  },
  userHideRules: ["/data/adb/magisk", "/sdcard/Download/advanced", "/system/app/EdXposed"],
  allRules: [
    { type: "HIDE", path: "/data/adb/magisk", isUserDefined: true },
    { type: "HIDE", path: "/sdcard/Download/advanced", isUserDefined: true },
    { type: "MERGE", path: "/system/etc", source: "/data/adb/modules/playintegrityfix/system/etc", isUserDefined: false },
    { type: "ADD", path: "/system/bin/keystore2", source: "/data/adb/modules/playintegrityfix/system/bin/keystore2", isUserDefined: false },
  ],
  lkmStatus: {
    loaded: true,
    builtin: false,
    autoload: true,
    kmi_override: "",
    detected_kmi: "android14-6.1",
    asset: "/data/adb/modules/kagami/kasumi/android14-6.1_arm64_kasumi_lkm.ko",
    last_error: "",
    unload: { attempted: false, quiesce_supported: true },
  },
  logs: {
    system: `[INFO] Kagami daemon initialized\n[INFO] tmpfs backend selected\n[DEBUG] active modules: zygisk_next, playintegrityfix`,
    kernel: `[KAGAMIFS] protocol=14 feature_mask=0x1e5\n[KAGAMIFS] tracepoint hooks ready\n[KAGAMIFS] add hide rule /data/adb/magisk`,
  },
};

const mockApi = {
  async getKasumiConflicts() { return { events: [] }; },
  async ackKasumiConflicts(sequence) { conflictSequence(sequence); },
  async getMeta() {
    return { ...MODULE_META, version: "Preview", module_id: "kagami", config_file: PATHS.CONFIG, log_file: PATHS.DEFAULT_LOG };
  },

  async loadConfig() {
    return { ...clone(mockState.config), __lkmAutoloadPresent: true };
  },

  async saveConfig(updates) {
    mockState.config = { ...normalizeConfig(mockState.config), ...configPatch(updates) };
    return { persisted: true, runtimeApplied: mockState.systemInfo.kasumiAvailable };
  },

  async scanModules() {
    return clone(mockState.modules);
  },

  async setModuleMode(id, mode) {
    validModuleId(id);
    validMode(mode);
    const module = mockState.modules.find((item) => item.id === id);
    if (!module) throw new Error("Module not found");
    module.mode = mode;
  },

  async setModuleBypass(id, bypass) {
    validModuleId(id);
    const module = mockState.modules.find(item => item.id === id);
    if (!module) throw new Error("Module not found");
    module.bypass = bypass;
  },

  async addModuleRule(id, path, mode, bypass = false) {
    validModuleId(id);
    validPath(path);
    validMode(mode, true);
    const module = mockState.modules.find((item) => item.id === id);
    if (!module) throw new Error("Module not found");
    const rule = module.rules.find((item) => item.path === path);
    if (rule) { rule.mode = mode; rule.bypass = bypass; }
    else module.rules.push({ path, mode, bypass });
  },

  async removeModuleRule(id, path) {
    validModuleId(id);
    validPath(path);
    const module = mockState.modules.find((item) => item.id === id);
    if (module) module.rules = module.rules.filter((item) => item.path !== path);
  },

  async checkConflicts() {
    return [];
  },

  async applyModuleRules(id) {
    validModuleId(id);
    const module = mockState.modules.find(item => item.id === id);
    return { applied: mockState.config.kasumi_enabled && module?.runtime?.backend === "kasumi" };
  },

  async syncPartitions() {
    return "sync completed";
  },

  async scanPartitionsFromModules() {
    return ["product", "system_ext", "vendor_dlkm"];
  },

  async getPartitionScan() {
    return { partitions: await this.scanPartitionsFromModules(), invalid: [] };
  },

  async readLogs(logPath = "system", lines = 1000) {
    if (!Number.isInteger(lines) || lines < 1 || lines > 10000) throw new Error("Invalid log line count");
    return (logPath === "kernel" ? mockState.logs.kernel : mockState.logs.system).split("\n").slice(-lines).join("\n");
  },

  async clearLogs() {
    mockState.logs.system = "";
  },

  async getStorageUsage() {
    return clone(mockState.storage);
  },

  async getSystemInfo() {
    return clone(mockState.systemInfo);
  },

  async hotMount(moduleId) {
    if (!mockState.systemInfo.kasumiModules.includes(moduleId)) {
      mockState.systemInfo.kasumiModules.push(moduleId);
    }
  },

  async hotUnmount(moduleId) {
    mockState.systemInfo.kasumiModules = mockState.systemInfo.kasumiModules.filter((item) => item !== moduleId);
  },

  async getUserHideRules(details = false) {
    return details ? mockState.userHideRules.map(path => ({ path, bypass: mockState.hideBypasses?.[path] === true })) : clone(mockState.userHideRules);
  },

  async getAllRules() {
    if (!mockState.systemInfo.kasumiAvailable) throw new Error("Active rules are unavailable while Kasumi cannot be queried");
    const userSet = new Set(mockState.userHideRules);
    return clone(mockState.allRules).map((rule) => ({ ...rule, active: true, isUserDefined: rule.type === "HIDE" && userSet.has(rule.path) }));
  },

  async addUserHideRule(path, bypass = false) {
    validPath(path);
    mockState.hideBypasses ??= {};
    mockState.hideBypasses[path] = bypass;
    if (!mockState.userHideRules.includes(path)) {
      mockState.userHideRules.unshift(path);
      if (mockState.systemInfo.kasumiAvailable) mockState.allRules.unshift({ type: "HIDE", path, isUserDefined: true });
    }
    return { persisted: true };
  },

  async removeUserHideRule(path) {
    validPath(path);
    mockState.userHideRules = mockState.userHideRules.filter((item) => item !== path);
    if (mockState.systemInfo.kasumiAvailable) mockState.allRules = mockState.allRules.filter((item) => !(item.type === "HIDE" && item.path === path && item.isUserDefined));
    return { persisted: true };
  },

  async getLkmStatus() {
    return { valid: true, ...clone(mockState.lkmStatus) };
  },

  async lkmSetKmi(kmi) {
    if (typeof kmi !== "string" || !/^android\d+-\d+\.\d+$/.test(kmi)) throw new Error("Invalid KMI; expected android14-6.1 format");
    mockState.lkmStatus.kmi_override = kmi;
  },

  async lkmClearKmi() {
    mockState.lkmStatus.kmi_override = "";
  },

  async lkmLoad() {
    mockState.lkmStatus.loaded = true;
    mockState.systemInfo.kasumiAvailable = true;
    mockState.config.kasumi_available = true;
  },

  async lkmUnload() {
    mockState.lkmStatus.loaded = false;
    mockState.systemInfo.kasumiAvailable = false;
    mockState.config.kasumi_available = false;
    mockState.allRules = [];
  },

  async lkmSetAutoload(on) {
    if (typeof on !== "boolean") throw new Error("Autoload must be a boolean");
    mockState.lkmStatus.autoload = Boolean(on);
    mockState.config.lkm_autoload = on;
  },
};

const realApi = {
  async getKasumiConflicts() {
    const snapshot = objectResponse(await execJson(`${PATHS.BINARY} kasumi conflicts`));
    const events = arrayResponse(snapshot.events);
    if (events.length > 16) throw new Error("Invalid conflict snapshot");
    for (const event of events) {
      objectResponse(event);
      if (conflictSequence(event.sequence) === "0" || !Number.isInteger(event.kind) || typeof event.target !== "string") {
        throw new Error("Invalid conflict event");
      }
    }
    return { events };
  },
  async ackKasumiConflicts(sequence) {
    await checkedExec(`${PATHS.BINARY} kasumi ack-conflicts ${conflictSequence(sequence)}`);
  },
  async getMeta() {
    const data = objectResponse(await execJson(`${PATHS.BINARY} api meta`));
    if (typeof data.version !== "string" || !data.version) throw new Error("The module version is unavailable");
    return { ...MODULE_META, ...data };
  },

  async loadConfig() {
    const raw = objectResponse(await execJson(`${PATHS.BINARY} config show`));
    return {
      ...clone(DEFAULT_CONFIG),
      ...normalizeConfig(raw),
      __lkmAutoloadPresent: hasOwn(raw, "lkm_autoload"),
    };
  },

  async saveConfig(updates) {
    const patch = configPatch(updates);
    await checkedExec(`${PATHS.BINARY} config merge-json '${shellEscape(JSON.stringify(patch))}'`);
    try {
      const system = objectResponse(await execJson(`${PATHS.BINARY} api system`));
      if (typeof system.kasumi_available !== "boolean") throw new Error("The runtime availability response is invalid");
      if (system.kasumi_available) {
        try {
          await checkedExec(`${PATHS.BINARY} config apply`);
        } catch (error) {
          // Preserve the existing fail-closed control path, including when a
          // bridge timeout leaves the apply result unknown.
          try { await checkedExec(`${PATHS.BINARY} kasumi disable`); } catch (_cleanupError) { /* original error wins */ }
          throw error;
        }
      }
      return { persisted: true, runtimeApplied: system.kasumi_available };
    } catch (cause) {
      const error = configPersistedError(cause.message || "Configuration saved, but runtime application could not be confirmed");
      if (cause.outcomeUnknown) error.outcomeUnknown = true;
      throw error;
    }
  },

  async scanModules() {
    const data = await execJson(`${PATHS.BINARY} module list --include-inactive`);
    const modules = arrayResponse(Array.isArray(data) ? data : objectResponse(data).modules);
    return modules.map((item) => {
      objectResponse(item);
      validModuleId(item.id);
      const rules = arrayResponse(item.rules ?? []).map((rule) => {
        objectResponse(rule);
        if (typeof rule.path !== "string" || typeof rule.mode !== "string") throw new Error("Invalid module rule response");
        return { path: rule.path, mode: rule.mode, bypass: rule.bypass === true };
      });
      return {
        id: item.id,
        name: item.name || item.id,
        version: item.version || "",
        author: item.author || "",
        description: item.description || "",
        mode: item.mode || "auto",
        bypass: item.bypass === true,
        strategy: item.strategy || "unknown",
        runtime: moduleRuntime(item.runtime),
        path: item.path || "",
        rules,
      };
    });
  },

  async setModuleMode(id, mode) {
    validModuleId(id);
    validMode(mode);
    await queueModuleWrite(async () => {
      await checkedExec(`${PATHS.BINARY} module set-mode '${shellEscape(id)}' '${shellEscape(mode)}'`);
      await this.verifyModule(id, module => module.mode === mode);
    });
  },

  async setModuleBypass(id, bypass) {
    validModuleId(id);
    await queueModuleWrite(async () => {
      await checkedExec(`${PATHS.BINARY} module set-bypass '${shellEscape(id)}' ${bypass ? 'on' : 'off'}`);
      await this.verifyModule(id, module => module.bypass === bypass);
    });
  },

  async addModuleRule(id, path, mode, bypass = false) {
    validModuleId(id);
    validPath(path);
    validMode(mode, true);
    await queueModuleWrite(async () => {
      await checkedExec(`${PATHS.BINARY} module add-rule '${shellEscape(id)}' '${shellEscape(path)}' '${shellEscape(mode)}'${bypass ? ' --bypass' : ''}`);
      await this.verifyModule(id, module => module.rules.some(rule => rule.path === path && rule.mode === mode && rule.bypass === bypass));
    });
  },

  async removeModuleRule(id, path) {
    validModuleId(id);
    validPath(path);
    await queueModuleWrite(async () => {
      await checkedExec(`${PATHS.BINARY} module remove-rule '${shellEscape(id)}' '${shellEscape(path)}'`);
      await this.verifyModule(id, module => module.rules.every(rule => rule.path !== path));
    });
  },

  async verifyModule(id, matches) {
    const module = (await this.scanModules()).find(item => item.id === id);
    if (!module || !matches(module)) throw new Error("Saved module settings do not match the requested change");
  },

  async applyModuleRules(id) {
    validModuleId(id);
    return queueModuleWrite(async () => {
      try {
        const result = objectResponse(await execJson(`${PATHS.BINARY} module apply '${shellEscape(id)}'`));
        if (typeof result.applied !== "boolean") throw new Error("Invalid module apply response");
        return result;
      } catch (error) {
        if (error.outcomeUnknown) {
          error.configPersisted = true;
          throw error;
        }
        throw configPersistedError(error.message);
      }
    });
  },

  async checkConflicts() {
    return arrayResponse(await execJson(`${PATHS.BINARY} module check-conflicts`)).map((item) => {
      objectResponse(item);
      if (typeof item.file !== "string" || !Array.isArray(item.modules) || item.modules.some((id) => typeof id !== "string")) {
        throw new Error("Invalid conflict response");
      }
      return item;
    });
  },

  async syncPartitions() {
    return (await checkedExec(`${PATHS.BINARY} config sync-partitions`)).stdout;
  },

  async scanPartitionsFromModules() {
    const output = await this.syncPartitions();
    const partitions = new Set();
    for (const line of output.split("\n")) {
      const match = line.match(/^Added partition:\s*(\S+)\s*$/);
      if (match) partitions.add(match[1]);
    }
    return [...partitions];
  },

  async getPartitionScan() {
    const data = objectResponse(await execJson(`${PATHS.BINARY} config sync-partitions --json`));
    const partitions = arrayResponse(data.partitions), invalid = arrayResponse(data.invalid);
    if (partitions.some(name => typeof name !== "string" || !/^[A-Za-z0-9_.-]+$/.test(name) || name === "." || name === "..")
        || invalid.some(name => typeof name !== "string")) {
      throw new Error("Invalid partition scan response");
    }
    return { partitions: [...new Set(partitions)], invalid: [...new Set(invalid)] };
  },

  async readLogs(logPath = "system", lines = 1000) {
    if (!Number.isInteger(lines) || lines < 1 || lines > 10000) throw new Error("Invalid log line count");
    if (logPath === "kernel") {
      // Filtering on the JS side preserves dmesg's exit status; a shell
      // grep/tail pipeline used to turn permission failures into empty logs.
      const result = await checkedExec("dmesg");
      return result.stdout.split("\n").filter((line) => /kasumi|kagamifs/i.test(line)).slice(-lines).join("\n");
    }
    const target = !logPath || logPath === "system" ? PATHS.DEFAULT_LOG : logPath;
    validPath(target);
    return (await checkedExec(`tail -n ${lines} '${shellEscape(target)}'`)).stdout;
  },

  async clearLogs() {
    await checkedExec(`printf '' > '${shellEscape(PATHS.DEFAULT_LOG)}'`);
  },

  async getStorageUsage() {
    const data = objectResponse(await execJson(`${PATHS.BINARY} api storage`));
    if (data.error) throw new Error(String(data.error));
    return {
      ...data,
      size: data.size || "-",
      used: data.used || "-",
      avail: data.avail || "-",
      percent: typeof data.percent === "number" ? data.percent : null,
      mode: data.mode || null,
    };
  },

  async getSystemInfo() {
    const [systemData, mountData] = await Promise.all([
      execJson(`${PATHS.BINARY} api system`).then(objectResponse),
      execJson(`${PATHS.BINARY} kasumi version`).then(objectResponse),
    ]);
    if (typeof systemData.kasumi_available !== "boolean" || typeof mountData.protocol_mismatch !== "boolean") {
      throw new Error("Invalid Kasumi availability response");
    }
    const modules = arrayResponse(mountData.active_modules);
    if (modules.some((id) => typeof id !== "string")) throw new Error("Invalid active module response");
    return {
      kernel: systemData.kernel || "",
      selinux: systemData.selinux || "",
      mountBase: systemData.mount_base || mountData.mount_base || "",
      kasumiAvailable: systemData.kasumi_available,
      kasumiStatus: systemData.kasumi_status,
      kasumiModules: modules,
      kasumiMismatch: mountData.protocol_mismatch,
      mismatchMessage: mountData.mismatch_message || "",
      hooks: systemData.hooks || "",
      features: systemData.features,
      mountStats: systemData.mountStats,
      detectedPartitions: systemData.detectedPartitions,
      backends: systemData.backends,
    };
  },

  async hotMount(moduleId) {
    validModuleId(moduleId);
    await checkedExec(`${PATHS.BINARY} module hot-mount '${shellEscape(moduleId)}'`);
  },

  async hotUnmount(moduleId) {
    validModuleId(moduleId);
    await checkedExec(`${PATHS.BINARY} module hot-unmount '${shellEscape(moduleId)}'`);
  },

  async getUserHideRules(details = false) {
    if (details) {
      return arrayResponse(await execJson(`${PATHS.BINARY} hide list --details`)).map(value => {
        const rule = typeof value === "string" ? { path: value, bypass: false } : objectResponse(value);
        validPath(rule.path);
        if (typeof rule.bypass !== "boolean") throw new Error("Invalid hide bypass flag");
        return { path: rule.path, bypass: rule.bypass };
      });
    }
    const paths = arrayResponse(await execJson(`${PATHS.BINARY} hide list`));
    if (paths.some((path) => typeof path !== "string" || !path.startsWith("/"))) throw new Error("Invalid saved hide rule response");
    return paths;
  },

  async getAllRules() {
    const status = objectResponse(await execJson(`${PATHS.BINARY} api system`));
    if (status.kasumi_available !== true) throw new Error("Active rules are unavailable while Kasumi cannot be queried");
    const [userRules, data] = await Promise.all([
      this.getUserHideRules(),
      execJson(`${PATHS.BINARY} kasumi list`),
    ]);
    const userSet = new Set(userRules);
    const rules = [];
    let enabled = true;
    for (const value of arrayResponse(data)) {
      const rule = objectResponse(value);
      if (typeof rule.type !== "string") throw new Error("Invalid active rule response");
      // The native CLI also returns protocol, enablement and feature metadata.
      // A disabled kernel can retain rules without applying them.
      if (rule.type === "KASUMI") {
        const state = /^Enabled:\s*([01])$/.exec(rule.args);
        if (state) enabled = state[1] === "1";
        continue;
      }
      if (!["ADD", "MERGE", "HIDE"].includes(rule.type)) continue;
      const path = rule.path ?? rule.target;
      if (typeof path !== "string" || !path.startsWith("/") ||
          (rule.type !== "HIDE" && (typeof rule.source !== "string" || !rule.source.startsWith("/")))) {
        throw new Error("Invalid active rule response");
      }
      rules.push({ ...rule, path, isUserDefined: rule.type === "HIDE" && userSet.has(path), active: true });
    }
    return enabled ? rules : [];
  },

  async addUserHideRule(path, bypass = false) {
    validPath(path);
    await checkedExec(`${PATHS.BINARY} hide add '${shellEscape(path)}'${bypass ? ' --bypass' : ''}`);
    if (!(await this.getUserHideRules(true)).some(rule => rule.path === path && rule.bypass === bypass)) {
      throw new Error("Saved hide rule does not match the requested change");
    }
    return { persisted: true };
  },

  async removeUserHideRule(path) {
    validPath(path);
    await checkedExec(`${PATHS.BINARY} hide remove '${shellEscape(path)}'`);
    if ((await this.getUserHideRules()).includes(path)) {
      throw new Error("Hide rule is still saved after removal");
    }
    return { persisted: true };
  },

  async getLkmStatus() {
    const data = objectResponse(await execJson(`${PATHS.BINARY} api lkm`));
    if (typeof data.loaded !== "boolean" && data.builtin !== true) throw new Error("Invalid LKM status response");
    return { ...data, valid: true };
  },

  async lkmSetKmi(kmi) {
    if (typeof kmi !== "string" || !/^android\d+-\d+\.\d+$/.test(kmi)) throw new Error("Invalid KMI; expected android14-6.1 format");
    await checkedExec(`${PATHS.BINARY} lkm set-kmi '${shellEscape(kmi)}'`);
  },

  async lkmClearKmi() {
    await checkedExec(`${PATHS.BINARY} lkm clear-kmi`);
  },

  async lkmLoad() {
    await checkedExec(`${PATHS.BINARY} lkm load`);
  },

  async lkmUnload() {
    await checkedExec(`${PATHS.BINARY} lkm unload`);
  },

  async lkmSetAutoload(on) {
    if (typeof on !== "boolean") throw new Error("Autoload must be a boolean");
    await checkedExec(`${PATHS.BINARY} lkm set-autoload ${on ? "on" : "off"}`);
  },
};

for (const implementation of [mockApi, realApi]) {
  implementation.loadUserHideRules = implementation.getUserHideRules;
  implementation.loadAllRules = implementation.getAllRules;
}

export const api = runtimeMode === "mock" ? mockApi : realApi;
