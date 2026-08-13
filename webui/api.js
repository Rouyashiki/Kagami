import { exec, hasKernelSU } from "./assets/kernelsu.js";

export const PATHS = {
  BINARY: "/data/adb/modules/kagami/kagamid",
  CONFIG: "/data/adb/kagami/config.json",
  MODE_CONFIG: "/data/adb/kagami/module_mode.json",
  RULES_CONFIG: "/data/adb/kagami/module_rules.json",
  DEFAULT_LOG: "/data/adb/kagami/daemon.log",
};

export const KASUMI_FEATURE_CONFIG_VERSION = 2;

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
  mirror_dir: "/dev/kagami_mirror",
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
  cmdline_value: "",
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
  version: "v2.2.5",
  author: "Anatdx",
};

const urlParams = new URLSearchParams(globalThis.location?.search || "");
const forceMock = urlParams.get("mock") === "1";
const runtimeMode = forceMock || !hasKernelSU() ? "mock" : "live";

export function getRuntimeMode() {
  return runtimeMode;
}

function apiOutput(result) {
  return String((result && (result.stdout || result.stderr)) || "").trim();
}

function shellEscape(value) {
  return String(value ?? "").replace(/'/g, "'\\''");
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function hasOwn(value, key) {
  return Object.prototype.hasOwnProperty.call(value, key);
}

function normalizeConfig(rawConfig) {
  const raw = rawConfig && typeof rawConfig === "object" ? clone(rawConfig) : {};
  const splitKeys = [
    "enable_overlay_xattr_hide",
    "enable_mount_hide",
    "enable_maps_spoof",
    "enable_statfs_spoof",
  ];
  const hasSplitFeatures =
    Number(raw.kasumi_feature_config_version || 0) >= KASUMI_FEATURE_CONFIG_VERSION ||
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

async function execJson(command, fallback = {}) {
  const result = await exec(command);
  const out = apiOutput(result);
  if (result.errno === 0 && out) {
    return JSON.parse(out);
  }
  return clone(fallback);
}

async function writeJsonFile(path, value) {
  const escapedPath = shellEscape(path);
  const payload = shellEscape(JSON.stringify(value, null, 2));
  const command = `mkdir -p "$(dirname '${escapedPath}')" && printf '%s\\n' '${payload}' > '${escapedPath}'`;
  const result = await exec(command);
  if (result.errno !== 0) {
    throw new Error(apiOutput(result) || `Failed to write ${path}`);
  }
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
    cmdline_value: "androidboot.verifiedbootstate=green androidboot.vbmeta.device_state=locked",
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
    mountBase: "/dev/kagami_mirror",
    kasumiAvailable: true,
    kasumiStatus: 0,
    kasumiModules: ["playintegrityfix"],
    kasumiMismatch: false,
    hooks:
      "GET_FD: tracepoint(sys_enter/sys_exit)\npath: tracepoint(sys_enter)\nvfs_getattr,d_path,iterate_dir,vfs_getxattr: ftrace+kretprobe\ncmdline: tracepoint(sys_enter/sys_exit)",
    features: {
      bitmask: 0x1e5,
      names: [
        "mount_hide",
        "maps_spoof",
        "statfs_spoof",
        "cmdline_spoof",
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
    autoload: true,
    kmi_override: "",
  },
  logs: {
    system: `[INFO] Kagami daemon initialized\n[INFO] tmpfs backend selected\n[DEBUG] active modules: zygisk_next, playintegrityfix\n[INFO] applied cmdline spoof`,
    kernel: `[KAGAMIFS] protocol=14 feature_mask=0x1e5\n[KAGAMIFS] tracepoint hooks ready\n[KAGAMIFS] add hide rule /data/adb/magisk\n[KAGAMIFS] cmdline spoof enabled`,
  },
};

const mockApi = {
  async loadConfig() {
    return clone(mockState.config);
  },

  async saveConfig(config) {
    mockState.config = { ...clone(DEFAULT_CONFIG), ...normalizeConfig(config) };
  },

  async scanModules() {
    return clone(mockState.modules);
  },

  async saveModules(modules) {
    mockState.modules = clone(modules);
  },

  async saveRules(modules) {
    mockState.modules = clone(modules);
  },

  async checkConflicts() {
    return [];
  },

  async syncPartitions() {
    return "sync completed";
  },

  async scanPartitionsFromModules() {
    return ["product", "system_ext", "vendor_dlkm"];
  },

  async readLogs(logPath) {
    return clone(logPath === "kernel" ? mockState.logs.kernel : mockState.logs.system);
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

  async getUserHideRules() {
    return clone(mockState.userHideRules);
  },

  async getAllRules() {
    return clone(mockState.allRules);
  },

  async addUserHideRule(path) {
    if (!mockState.userHideRules.includes(path)) {
      mockState.userHideRules.unshift(path);
      mockState.allRules.unshift({ type: "HIDE", path, isUserDefined: true });
    }
  },

  async removeUserHideRule(path) {
    mockState.userHideRules = mockState.userHideRules.filter((item) => item !== path);
    mockState.allRules = mockState.allRules.filter((item) => !(item.type === "HIDE" && item.path === path && item.isUserDefined));
  },

  async getLkmStatus() {
    return clone(mockState.lkmStatus);
  },

  async lkmSetKmi(kmi) {
    mockState.lkmStatus.kmi_override = kmi;
  },

  async lkmClearKmi() {
    mockState.lkmStatus.kmi_override = "";
  },

  async lkmLoad() {
    mockState.lkmStatus.loaded = true;
  },

  async lkmUnload() {
    mockState.lkmStatus.loaded = false;
  },

  async lkmSetAutoload(on) {
    mockState.lkmStatus.autoload = Boolean(on);
  },
};

const realApi = {
  async loadConfig() {
    const result = await exec(`${PATHS.BINARY} config show`);
    const out = apiOutput(result);
    if (result.errno === 0 && out) {
      const normalized = normalizeConfig(JSON.parse(out));
      return { ...clone(DEFAULT_CONFIG), ...normalized };
    }
    return clone(DEFAULT_CONFIG);
  },

  async saveConfig(config) {
    const configToSave = {
      moduledir: config.moduledir,
      tempdir: config.tempdir,
      mountsource: config.mountsource,
      debug: config.debug,
      verbose: config.verbose,
      lkm_autoload: config.lkm_autoload === true,
      fs_type: config.fs_type,
      work_dir: config.work_dir,
      mirror_dir: config.mirror_dir,
      mirror_img: config.mirror_img,
      mirror_img_size_mb: config.mirror_img_size_mb,
      overlay_writable: config.overlay_writable,
      disable_umount: config.disable_umount,
      enable_nuke: config.enable_nuke,
      enable_kernel_debug: config.enable_kernel_debug,
      enable_stealth: config.enable_stealth,
      kasumi_feature_config_version: KASUMI_FEATURE_CONFIG_VERSION,
      enable_overlay_xattr_hide: config.enable_overlay_xattr_hide === true,
      enable_mount_hide: config.enable_mount_hide === true,
      enable_maps_spoof: config.enable_maps_spoof === true,
      enable_statfs_spoof: config.enable_statfs_spoof === true,
      enable_selinux_fix: config.enable_selinux_fix === true,
      kasumi_enabled: config.kasumi_enabled,
      overlayfs_enabled: config.overlayfs_enabled,
      magic_mount_enabled: config.magic_mount_enabled,
      mount_backend: config.mount_backend,
      policy: clone(config.policy || DEFAULT_CONFIG.policy),
      cmdline_value: config.cmdline_value,
      partitions: config.partitions,
    };

    await writeJsonFile(PATHS.CONFIG, configToSave);

    if (config.kasumi_available) {
      const result = await exec(`${PATHS.BINARY} config apply`);
      if (result.errno !== 0) {
        await exec(`${PATHS.BINARY} kasumi disable`);
        throw new Error(apiOutput(result) || "Failed to apply Kasumi configuration");
      }
    }

    if (config.cmdline_value) {
      await exec(`${PATHS.BINARY} debug set-cmdline '${shellEscape(config.cmdline_value)}'`);
    } else {
      await exec(`${PATHS.BINARY} debug clear-cmdline`);
    }
  },

  async scanModules() {
    const data = await execJson(`${PATHS.BINARY} module list`, []);
    const modules = Array.isArray(data.modules) ? data.modules : Array.isArray(data) ? data : [];
    return modules.map((item) => ({
      id: item.id,
      name: item.name || item.id,
      version: item.version || "",
      author: item.author || "",
      description: item.description || "",
      mode: item.mode || "auto",
      strategy: item.strategy || "overlay",
      path: item.path || "",
      rules: Array.isArray(item.rules) ? item.rules : [],
    }));
  },

  async saveModules(modules) {
    const modes = {};
    modules.forEach((module) => {
      if (module.mode !== "auto" && /^[a-zA-Z0-9_.-]+$/.test(module.id)) {
        modes[module.id] = module.mode;
      }
    });
    await writeJsonFile(PATHS.MODE_CONFIG, modes);
  },

  async saveRules(modules) {
    const rules = {};
    modules.forEach((module) => {
      if (Array.isArray(module.rules) && module.rules.length > 0) {
        rules[module.id] = module.rules.map((rule) => ({ path: rule.path, mode: rule.mode }));
      }
    });
    await writeJsonFile(PATHS.RULES_CONFIG, rules);
  },

  async checkConflicts() {
    const result = await exec(`${PATHS.BINARY} module check-conflicts`);
    const out = apiOutput(result);
    if (result.errno === 0 && out) {
      return JSON.parse(out);
    }
    return [];
  },

  async syncPartitions() {
    const result = await exec(`${PATHS.BINARY} config sync-partitions`);
    if (result.errno === 0) {
      return result.stdout || result.stderr || "";
    }
    throw new Error(apiOutput(result) || "sync failed");
  },

  async scanPartitionsFromModules() {
    const result = await exec(`${PATHS.BINARY} config sync-partitions 2>&1`);
    const partitions = new Set();
    String(result.stdout || "")
      .split("\n")
      .forEach((line) => {
        const match = line.match(/Added partition:\s*(\S+)/);
        if (match) {
          partitions.add(match[1]);
        }
      });
    return [...partitions];
  },

  async readLogs(logPath, lines = 1000) {
    if (logPath === "kernel") {
      const result = await exec(`dmesg | grep -i kasumi | tail -n ${lines}`);
      return result.stdout || "";
    }

    const target = !logPath || logPath === "system" ? PATHS.DEFAULT_LOG : logPath;
    const result = await exec(`[ -f '${shellEscape(target)}' ] && tail -n ${lines} '${shellEscape(target)}' || echo ""`);
    if (result.errno === 0) {
      return result.stdout || "";
    }
    throw new Error(apiOutput(result) || "failed to read logs");
  },

  async clearLogs() {
    const result = await exec(`echo -n > '${shellEscape(PATHS.DEFAULT_LOG)}'`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "failed to clear logs");
    }
  },

  async getStorageUsage() {
    const result = await exec(`${PATHS.BINARY} api storage`);
    const out = apiOutput(result);
    if (result.errno === 0 && out) {
      const data = JSON.parse(out);
      if (!data.error) {
        return {
          size: data.size || "-",
          used: data.used || "-",
          avail: data.avail || "-",
          percent: typeof data.percent === "number" ? data.percent : 0,
          mode: data.mode || null,
        };
      }
    }
    return { size: "-", used: "-", avail: "-", percent: 0, mode: null };
  },

  async getSystemInfo() {
    let kernel = "Unknown";

    try {
      const versionResult = await exec("cat /proc/version");
      const stdout = String(versionResult.stdout || "");
      const releaseMatch = stdout.match(/Linux version ([^\s]+)/);
      if (releaseMatch) {
        kernel = releaseMatch[1];
      } else if (stdout.trim()) {
        kernel = stdout.trim();
      }
    } catch (_error) {
      // ignore
    }

    let selinux = "Unknown";
    try {
      const result = await exec("getenforce");
      if (result.stdout) {
        selinux = result.stdout.trim();
      }
    } catch (_error) {
      // ignore
    }

    let systemData = {};
    try {
      systemData = await execJson(`${PATHS.BINARY} api system`, {});
    } catch (_error) {
      systemData = {};
    }

    let mountData = {};
    try {
      mountData = await execJson(`${PATHS.BINARY} kasumi version`, {});
    } catch (_error) {
      mountData = {};
    }

    return {
      kernel,
      selinux,
      mountBase: systemData.mount_base || mountData.mount_base || "/dev/kagami_mirror",
      kasumiAvailable: systemData.kasumi_available,
      kasumiStatus: systemData.kasumi_status,
      kasumiModules: mountData.active_modules || [],
      kasumiMismatch: mountData.protocol_mismatch || false,
      mismatchMessage: mountData.mismatch_message,
      hooks: systemData.hooks || mountData.hooks || "",
      features: systemData.features,
      mountStats: systemData.mountStats,
      detectedPartitions: systemData.detectedPartitions,
    };
  },

  async hotMount(moduleId) {
    const result = await exec(`${PATHS.BINARY} module hot-mount '${shellEscape(moduleId)}'`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "hot mount failed");
    }
  },

  async hotUnmount(moduleId) {
    const result = await exec(`${PATHS.BINARY} module hot-unmount '${shellEscape(moduleId)}'`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "hot unmount failed");
    }
  },

  async getUserHideRules() {
    const result = await exec(`${PATHS.BINARY} hide list`);
    const out = apiOutput(result);
    if (result.errno === 0 && out) {
      try {
        const parsed = JSON.parse(out);
        if (Array.isArray(parsed)) {
          return parsed;
        }
      } catch (_error) {
        return out
          .split("\n")
          .map((line) => line.trim())
          .filter((line) => line.startsWith("/"));
      }
    }
    return [];
  },

  async getAllRules() {
    const [userRules, output] = await Promise.all([this.getUserHideRules(), exec(`${PATHS.BINARY} kasumi list`)]);
    const userSet = new Set(userRules);
    const rules = [];
    const out = apiOutput(output);

    if (output.errno === 0 && out) {
      let parsed = false;
      try {
        const data = JSON.parse(out);
        if (Array.isArray(data)) {
          parsed = true;
          data.forEach((rule) => {
            if (rule.type === "INJECT") {
              return;
            }
            const path = rule.path ?? rule.target ?? "";
            rules.push({
              type: rule.type || "UNKNOWN",
              path,
              target: rule.target,
              source: rule.source,
              isUserDefined: rule.type === "HIDE" && path && userSet.has(path),
            });
          });
        }
      } catch (_error) {
        parsed = false;
      }

      if (!parsed) {
        out.split("\n").forEach((line) => {
          const trimmed = line.trim();
          if (!trimmed) {
            return;
          }
          if (trimmed.startsWith("hide ")) {
            const path = trimmed.slice(5).trim();
            if (path) {
              rules.push({ type: "HIDE", path, isUserDefined: userSet.has(path) });
            }
            return;
          }

          const parts = trimmed.split(/\s+/);
          const type = String(parts[0] || "").toUpperCase();
          if (type === "INJECT") {
            return;
          }
          if (type === "MERGE" || type === "ADD") {
            rules.push({ type, path: parts[1] || "", source: parts[2], isUserDefined: false });
          } else {
            rules.push({ type, path: parts.slice(1).join(" "), isUserDefined: false });
          }
        });
      }
    }

    return rules;
  },

  async addUserHideRule(path) {
    const result = await exec(`${PATHS.BINARY} hide add '${shellEscape(path)}'`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "failed to add hide rule");
    }
  },

  async removeUserHideRule(path) {
    const result = await exec(`${PATHS.BINARY} hide remove '${shellEscape(path)}'`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "failed to remove hide rule");
    }
  },

  async getLkmStatus() {
    const data = await execJson(`${PATHS.BINARY} api lkm`, { loaded: false, autoload: true, kmi_override: "" });
    return {
      loaded: data.loaded === true,
      autoload: data.autoload !== false,
      kmi_override: data.kmi_override || "",
    };
  },

  async lkmSetKmi(kmi) {
    const result = await exec(`${PATHS.BINARY} lkm set-kmi '${shellEscape(kmi)}'`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "failed to set KMI override");
    }
  },

  async lkmClearKmi() {
    const result = await exec(`${PATHS.BINARY} lkm clear-kmi`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "failed to clear KMI override");
    }
  },

  async lkmLoad() {
    const result = await exec(`${PATHS.BINARY} lkm load`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "failed to load LKM");
    }
  },

  async lkmUnload() {
    const result = await exec(`${PATHS.BINARY} lkm unload`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "failed to unload LKM");
    }
  },

  async lkmSetAutoload(on) {
    const result = await exec(`${PATHS.BINARY} lkm set-autoload ${on ? "on" : "off"}`);
    if (result.errno !== 0) {
      throw new Error(apiOutput(result) || "failed to set autoload");
    }
  },
};

export const api = runtimeMode === "mock" ? mockApi : realApi;
