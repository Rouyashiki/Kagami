#include "mount/backend.hpp"

#include "core/json.hpp"
#include "core/json_value.hpp"
#include "core/lkm.hpp"
#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/kasumi.hpp"
#include "mount/magic_mount.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"
#include "mount/storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

namespace kagami::mount {

namespace fs = std::filesystem;

std::string backend_kind_name(BackendKind kind) {
    switch (kind) {
    case BackendKind::Kasumi:
        return "kasumi";
    case BackendKind::Overlayfs:
        return "overlayfs";
    case BackendKind::MagicMount:
        return "magic_mount";
    }
    return "unknown";
}

static bool proc_filesystems_has(const std::string& name) {
    std::ifstream in("/proc/filesystems");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream parts(line);
        std::string first;
        std::string second;
        parts >> first >> second;
        if (first == name || second == name) {
            return true;
        }
    }
    return false;
}

static int count_committed_mounts() {
    std::ifstream in((runtime_data_dir() / "run" / "magic_mounts.list").string());
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ++n;
        }
    }
    return n;
}

// Policy is API17 kernel state, not a static LKM setting. Reapply it whenever
// Kagami prepares rules so AUTO remains tied to KernelSU's live denylist after
// every module load and hot refresh.
static bool apply_kasumi_policy(const Config& config) {
    if (!config.kasumi_enabled || !::kagami::kasumi::is_available()) {
        return true;
    }
    std::string error;
    if (kasumi::apply_policy_config(config.policy, error)) {
        return true;
    }
    fsutil::mlog("kasumi: policy apply failed: " + error);
    return false;
}

// --- bootloop protection state ---
constexpr int kMaxBootAttempts = 3;

static fs::path recovery_attempts_file() { return runtime_data_dir() / "run" / "boot_attempts"; }
static fs::path recovery_disabled_file() { return runtime_data_dir() / "run" / "mount_disabled"; }

static int read_boot_attempts() {
    std::ifstream in(recovery_attempts_file());
    int n = 0;
    in >> n;
    return n > 0 ? n : 0;
}

static void write_boot_attempts(int n) {
    std::error_code ec;
    fs::create_directories(recovery_attempts_file().parent_path(), ec);
    std::ofstream out(recovery_attempts_file(), std::ios::trunc);
    out << n << "\n";
}

// After a successful mount, daemonize a watcher that clears the bootloop counter
// once sys.boot_completed=1 (a fallback alongside boot-completed.sh). Double-fork
// + setsid so it outlives the short-lived metamount.sh process that invoked us.
static void spawn_boot_completed_watcher() {
    const pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return;
    }
    setsid();
    if (fork() != 0) {
        _exit(0); // intermediate child exits; grandchild reparents to init
    }
    // The daemon may already own its Kasumi capability FD, listening socket,
    // and lifetime lock. This long-lived watcher must not retain any of them.
    std::vector<int> inherited_fds;
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) {
        _exit(0); // boot-completed.sh remains the non-watcher fallback
    }
    const int scan_fd = dirfd(dir);
    while (dirent* entry = readdir(dir)) {
        char* end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' && fd > STDERR_FILENO &&
            fd != scan_fd) {
            inherited_fds.push_back(static_cast<int>(fd));
        }
    }
    closedir(dir);
    for (const int fd : inherited_fds) {
        close(fd);
    }
    for (int i = 0; i < 150; ++i) { // ~5 min ceiling
        char buf[PROP_VALUE_MAX] = {};
        if (__system_property_get("sys.boot_completed", buf) > 0 && buf[0] == '1') {
            break;
        }
        sleep(2);
    }
    recovery_boot_completed();
    _exit(0);
}

std::vector<BackendStatus> backend_statuses() {
    const auto version = ::kagami::kasumi::version_info();
    const bool kasumi_available = version.status == ::kagami::kasumi::Status::Available;
    std::ostringstream kasumi_detail;
    kasumi_detail << "protocol expected=" << version.expected_protocol
                  << " kernel=" << version.kernel_protocol
                  << " uid=" << version.process_uid
                  << " euid=" << version.process_euid
                  << " status=" << static_cast<int>(version.status);

    const bool overlayfs_available = proc_filesystems_has("overlay");

    const int magic_mounts = count_committed_mounts();
    std::ostringstream magic_detail;
    magic_detail << "Magisk-style systemless mount; live mounts=" << magic_mounts;

    return {
        {BackendKind::Kasumi, "Kasumi", kasumi_available, true, kasumi_detail.str()},
        {BackendKind::Overlayfs, "OverlayFS", overlayfs_available, false,
         overlayfs_available ? "overlay filesystem registered" : "overlay filesystem is not registered"},
        {BackendKind::MagicMount, "Magic Mount", true, false, magic_detail.str()},
    };
}

std::vector<ModuleEntry> enumerate_mountable_modules(const Config& config) {
    std::vector<ModuleEntry> out;
    const fs::path root = config.module_dir.empty() ? runtime_modules_dir() : fs::path(config.module_dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return out;
    }
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        const fs::path p = e.path();
        if (!fs::exists(p / "module.prop", ec)) {
            continue;
        }
        if (fs::exists(p / "disable", ec) || fs::exists(p / "remove", ec) ||
            fs::exists(p / "skip_mount", ec) ||
            fs::exists(runtime_data_dir() / "run" / "hot_unmounted" / p.filename(), ec)) {
            continue;
        }
        out.push_back({p.filename().string(), p});
    }
    std::sort(out.begin(), out.end(),
              [](const ModuleEntry& a, const ModuleEntry& b) { return a.id < b.id; });
    return out;
}

// Per-module control-plane state is separate from module files so the manager
// can select backends without modifying another module's contents.
ModuleModeMap load_module_modes() {
    ModuleModeMap out;
    std::ifstream in((runtime_data_dir() / "module_mode.json").string());
    if (!in) {
        return out;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    JsonValue root;
    std::string error;
    if (parse_json(buf.str(), root, error) && root.is_object()) {
        for (const auto& [id, value] : root.object_value) {
            if (value.is_string()) {
                out[id] = value.string_value;
            }
        }
    }
    return out;
}

bool save_module_modes(const ModuleModeMap& modes) {
    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    if (ec) {
        return false;
    }
    std::ofstream out(runtime_data_dir() / "module_mode.json", std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "{\n";
    for (auto it = modes.begin(); it != modes.end(); ++it) {
        out << "  " << json_quote(it->first) << ": " << json_quote(it->second);
        if (std::next(it) != modes.end()) {
            out << ",";
        }
        out << "\n";
    }
    out << "}\n";
    return out.good();
}

ModuleRuleMap load_module_rules() {
    ModuleRuleMap out;
    std::ifstream in(runtime_data_dir() / "module_rules.json");
    if (!in) {
        return out;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    JsonValue root;
    std::string error;
    if (!parse_json(buf.str(), root, error) || !root.is_object()) {
        return out;
    }
    for (const auto& [id, entries] : root.object_value) {
        if (!entries.is_array()) {
            continue;
        }
        for (const auto& entry : entries.array_value) {
            if (!entry.is_object()) {
                continue;
            }
            const JsonValue* path = entry.find("path");
            const JsonValue* mode = entry.find("mode");
            if (path && mode && path->is_string() && mode->is_string()) {
                out[id].push_back({path->string_value, mode->string_value});
            }
        }
    }
    return out;
}

bool save_module_rules(const ModuleRuleMap& rules) {
    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    if (ec) {
        return false;
    }
    std::ofstream out(runtime_data_dir() / "module_rules.json", std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "{\n";
    for (auto module = rules.begin(); module != rules.end(); ++module) {
        out << "  " << json_quote(module->first) << ": [";
        for (std::size_t i = 0; i < module->second.size(); ++i) {
            const auto& rule = module->second[i];
            if (i > 0) {
                out << ",";
            }
            out << "\n    {\"path\": " << json_quote(rule.path) << ", \"mode\": "
                << json_quote(rule.mode) << "}";
        }
        if (!module->second.empty()) {
            out << "\n  ";
        }
        out << "]";
        if (std::next(module) != rules.end()) {
            out << ",";
        }
        out << "\n";
    }
    out << "}\n";
    return out.good();
}

static bool dir_has_direct_files(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec)) {
            return true; // a non-dir entry sits directly at this level
        }
    }
    return false;
}

// A module needs magic mount when it places files directly at a partition root
// (e.g. system/build.prop): overlay only stacks on leaf subdirs, never on a
// partition root. Everything else can be overlaid.
static std::vector<std::string> managed_partitions(const Config& config) {
    std::vector<std::string> parts = fsutil::kManagedPartitions;
    for (const auto& part : config.partitions) {
        if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end()) {
            parts.push_back(part);
        }
    }
    return parts;
}

static bool module_needs_magic(const ModuleEntry& m, const Config& config) {
    if (dir_has_direct_files(m.path / "system")) {
        return true;
    }
    for (const auto& part : managed_partitions(config)) {
        if (part == "system") {
            continue;
        }
        if (dir_has_direct_files(m.path / part) ||
            dir_has_direct_files(m.path / "system" / part)) {
            return true;
        }
    }
    return false;
}

// True if the module has any managed-partition tree (i.e. contributes mounts).
static bool module_has_content(const ModuleEntry& m, const Config& config) {
    for (const auto& part : managed_partitions(config)) {
        std::error_code ec;
        if (fs::is_directory(m.path / part, ec)) {
            return true;
        }
    }
    return false;
}

static bool kasumi_usable() {
    const auto version = ::kagami::kasumi::version_info();
    return version.status == ::kagami::kasumi::Status::Available;
}

static std::string fallback_backend(const ModuleEntry& m, const Config& config) {
    if (config.overlayfs_enabled && proc_filesystems_has("overlay") && !module_needs_magic(m, config)) {
        return "overlay";
    }
    return config.magic_mount_enabled ? "magic" : "none";
}

std::string resolve_module_backend(const ModuleEntry& m, const Config& config,
                                   const ModuleModeMap& modes) {
    if (!module_has_content(m, config)) {
        return "none"; // no managed-partition tree → contributes no mounts
    }
    std::string mode = "auto";
    const std::string& global = config.mount_backend;
    if (!global.empty() && global != "auto") {
        mode = global; // global override forces every module
    } else {
        const auto it = modes.find(m.id);
        if (it != modes.end() && !it->second.empty()) {
            mode = it->second;
        }
    }
    // Kasumi is deliberately opt-in. `auto` preserves Kagami's established
    // OverlayFS -> Magic Mount fallback path; only an explicit module/global
    // "kasumi" selection reaches the LKM backend.
    if (mode == "kasumi") {
        return config.kasumi_enabled && kasumi_usable() ? "kasumi"
                                                              : fallback_backend(m, config);
    }
    if (mode == "auto") {
        return fallback_backend(m, config);
    }
    if (mode == "overlay") {
        return config.overlayfs_enabled && proc_filesystems_has("overlay") ? "overlay"
                                                                              : fallback_backend(m, config);
    }
    if (mode == "magic") {
        return config.magic_mount_enabled ? "magic" : fallback_backend(m, config);
    }
    return mode == "none" ? "none" : fallback_backend(m, config);
}

// Rewrite Kagami's own module.prop description so the manager's module list shows
// the live mount status (like hymo). Keeps the file's inode/context (truncate).
static void update_self_status(bool ok, std::size_t overlay, std::size_t magic,
                              std::size_t kasumi) {
    const fs::path prop = runtime_modules_dir() / "kagami" / "module.prop";
    std::ifstream in(prop);
    if (!in) {
        return;
    }
    std::ostringstream desc;
    desc << (ok ? "😋" : "😭") << " Kagami · Overlay " << overlay << " / Magic " << magic
         << " / Kasumi " << kasumi;

    std::string content;
    std::string line;
    bool desc_done = false;
    while (std::getline(in, line)) {
        if (line.rfind("description=", 0) == 0) {
            content += "description=" + desc.str() + "\n";
            desc_done = true;
        } else {
            content += line + "\n";
        }
    }
    in.close();
    if (!desc_done) {
        content += "description=" + desc.str() + "\n";
    }
    std::ofstream out(prop, std::ios::trunc);
    out << content;
}

MountReport mount_all_enabled(const Config& config) {
    MountReport report;

    // An LKM is an explicit boot-time choice. A packaged .ko must not load just
    // because Kagami itself runs, and this is the only automatic-load call site.
    if (config.kasumi_enabled && config.lkm_autoload && !lkm::autoload()) {
        fsutil::mlog("Kasumi LKM autoload failed: " + lkm::last_error());
    }

    const bool policy_ok = apply_kasumi_policy(config);
    const auto modules = enumerate_mountable_modules(config);
    report.modules = static_cast<int>(modules.size());

    // Bootloop protection: refuse to mount if previous boots never confirmed
    // completion, so a bad mount cannot brick boot. The counter is bumped here
    // and cleared by recovery_boot_completed() once boot_completed is reached.
    std::error_code ec;
    fs::create_directories(recovery_attempts_file().parent_path(), ec);
    if (fs::exists(recovery_disabled_file(), ec)) {
        report.ok = true;
        report.detail = "mounting disabled by bootloop protection; run 'kagamid recovery reset'";
        return report;
    }
    const int attempts = read_boot_attempts();
    if (attempts >= kMaxBootAttempts) {
        std::ofstream(recovery_disabled_file().string(), std::ios::trunc).put('\n');
        report.ok = true;
        report.detail = "bootloop protection tripped after " + std::to_string(attempts) +
                        " unconfirmed boots; mounting disabled (run 'kagamid recovery reset')";
        return report;
    }
    write_boot_attempts(attempts + 1);

    // Orchestrate per module: a global override (config.mount_backend != "auto")
    // forces every module; otherwise each module's mode (module_mode.json, default
    // "auto") decides, with auto following OverlayFS -> Magic Mount -> none.
    // Kasumi is selected only by an explicit module/global setting.
    const auto modes = load_module_modes();
    const auto rules = load_module_rules();
    std::vector<ModuleEntry> overlay_set;
    std::vector<ModuleEntry> magic_set;
    std::vector<ModuleEntry> kasumi_set;
    for (const auto& m : modules) {
        const std::string mode = resolve_module_backend(m, config, modes);
        if (mode == "overlay") {
            overlay_set.push_back(m);
        } else if (mode == "magic") {
            magic_set.push_back(m);
        } else if (mode == "kasumi") {
            kasumi_set.push_back(m);
        }
        // "none" → skip
    }

    fsutil::mlog("orchestrator: overlay=" + std::to_string(overlay_set.size()) +
                 " magic=" + std::to_string(magic_set.size()) +
                 " kasumi=" + std::to_string(kasumi_set.size()));

    bool ok = policy_ok;
    if (!overlay_set.empty() || !magic_set.empty() || !kasumi_set.empty()) {
        ok = fsutil::run_in_init_mount_ns([&]() {
            bool r = true;
            // Magic first: magic::mount_modules clears stale KSU mounts at start,
            // which would otherwise tear down overlay's freshly-mounted KSU leaves
            // (both backends use the same mount source).
            if (!magic_set.empty()) {
                r = magic::mount_modules(magic_set, config) && r;
            }
            if (!overlay_set.empty()) {
                r = overlay::mount_modules(overlay_set, config) && r;
            }
            if (!kasumi_set.empty()) {
                r = kasumi::mount_modules(kasumi_set, config, rules) && r;
            }
            return r;
        });
    }

    report.backend = "hybrid(overlay=" + std::to_string(overlay_set.size()) +
                     ",magic=" + std::to_string(magic_set.size()) +
                     ",kasumi=" + std::to_string(kasumi_set.size()) + ")";
    report.ok = ok;
    report.mounts = count_committed_mounts();
    report.detail = ok ? "ok" : "some backends reported errors (see daemon.log)";
    update_self_status(ok, overlay_set.size(), magic_set.size(), kasumi_set.size());
    if (ok) {
        spawn_boot_completed_watcher(); // clears the bootloop counter once boot completes
    }
    return report;
}

bool unmount_all(const Config& config) {
    // Hybrid: both backends may have live mounts; tear down both (each is a
    // source-gated no-op when it owns nothing).
    return fsutil::run_in_init_mount_ns([&]() {
        bool r = true;
        r = kasumi::unmount_all(config) && r;
        r = overlay::unmount_all(config) && r;
        r = magic::unmount_all(config) && r;
        storage::teardown_shared(config);
        return r;
    });
}

bool refresh_kasumi_modules(const Config& config) {
    if (!config.kasumi_enabled || !kasumi_usable()) {
        return false;
    }
    if (!apply_kasumi_policy(config)) {
        return false;
    }
    const auto all = enumerate_mountable_modules(config);
    const auto modes = load_module_modes();
    std::vector<ModuleEntry> selected;
    for (const auto& module : all) {
        if (resolve_module_backend(module, config, modes) == "kasumi") {
            selected.push_back(module);
        }
    }
    const auto rules = load_module_rules();
    return fsutil::run_in_init_mount_ns(
        [&]() { return kasumi::mount_modules(selected, config, rules); });
}

void recovery_boot_completed() {
    std::error_code ec;
    fs::remove(recovery_attempts_file(), ec); // boot confirmed: clear the counter
}

void recovery_reset() {
    std::error_code ec;
    fs::remove(recovery_attempts_file(), ec);
    fs::remove(recovery_disabled_file(), ec);
}

std::string recovery_status_json() {
    std::error_code ec;
    const bool disabled = fs::exists(recovery_disabled_file(), ec);
    std::ostringstream out;
    out << "{\"boot_attempts\":" << read_boot_attempts()
        << ",\"max_attempts\":" << kMaxBootAttempts
        << ",\"mounting_disabled\":" << (disabled ? "true" : "false") << "}";
    return out.str();
}

} // namespace kagami::mount
