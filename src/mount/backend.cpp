#include "mount/backend.hpp"
#include <iterator>

#include <charconv>
#include "core/directory.hpp"
#include "core/json.hpp"
#include "core/lkm.hpp"
#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/kasumi.hpp"
#include "mount/magic_mount.hpp"
#include "mount/module_runtime.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"
#include "mount/storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
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

namespace {
bool proc_filesystems_has(const std::string& name) {
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

int count_committed_mounts() {
    return static_cast<int>(magic::active_mounts(Config{}).size());
}

bool apply_kasumi_policy(const Config& config) {
    if (!config.kasumi_enabled || !::kagami::kasumi::is_available())
        return true;
    std::string error;
    if (kasumi::apply_policy_config(config.policy, error))
        return true;
    fsutil::mlog("kasumi: policy apply failed: " + error, logging::Level::Error);
    return false;
}

bool apply_kasumi_features(const Config& config) {
    if (!config.kasumi_enabled || !::kagami::kasumi::is_available()) {
        return true;
    }
    std::string error;
    if (kasumi::apply_feature_config(config, error)) {
        return true;
    }
    fsutil::mlog("kasumi: feature apply failed: " + error, logging::Level::Error);
    return false;
}
}  // namespace

// --- bootloop protection state ---
constexpr int kMaxBootAttempts = 3;

namespace {
fs::path recovery_attempts_file() {
    return runtime_data_dir() / "run" / "boot_attempts";
}
fs::path recovery_disabled_file() {
    return runtime_data_dir() / "run" / "mount_disabled";
}
fs::path mount_orchestrator_file() {
    return runtime_data_dir() / "run" / "mount_orchestrator_boot";
}

std::string current_boot_id() {
    std::ifstream in("/proc/sys/kernel/random/boot_id");
    std::string value;
    std::getline(in, value);
    return value;
}

bool claim_mount_orchestrator() {
    const std::string boot_id = current_boot_id();
    if (boot_id.empty()) {
        return false;
    }
    {
        std::ifstream in(mount_orchestrator_file());
        std::string recorded;
        if (std::getline(in, recorded) && recorded == boot_id) {
            return false;
        }
    }
    std::error_code ec;
    fs::create_directories(mount_orchestrator_file().parent_path(), ec);
    if (ec) {
        return false;
    }
    fs::path temporary = mount_orchestrator_file();
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    out << boot_id << "\n";
    out.flush();
    const bool write_ok = out.good();
    out.close();
    if (!write_ok || out.fail()) {
        fs::remove(temporary, ec);
        return false;
    }
    fs::rename(temporary, mount_orchestrator_file(), ec);
    if (ec) {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

int read_boot_attempts() {
    std::ifstream in(recovery_attempts_file());
    int n = 0;
    in >> n;
    return n > 0 ? n : 0;
}

void write_boot_attempts(int n) {
    std::error_code ec;
    fs::create_directories(recovery_attempts_file().parent_path(), ec);
    std::ofstream out(recovery_attempts_file(), std::ios::trunc);
    out << n << "\n";
}

// After a successful mount, daemonize a watcher that clears the bootloop counter
// once sys.boot_completed=1 (a fallback alongside boot-completed.sh). Double-fork
// + setsid so it outlives the short-lived metamount.sh process that invoked us.
void spawn_boot_completed_watcher() {
#if defined(__ANDROID__)
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
        _exit(0);  // intermediate child exits; grandchild reparents to init
    }
    // The daemon may already own its Kasumi capability FD, listening socket,
    // and lifetime lock. This long-lived watcher must not retain any of them.
    {
        const posix::Fd scan_fd(open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (scan_fd.get() < 0)
            _exit(0);  // boot-completed.sh remains the non-watcher fallback
        char scan_bytes[4096];
        posix::Directory entries(scan_fd.get(), scan_bytes, sizeof(scan_bytes));
        posix::DirectoryEntry entry;
        int scan_error = 0;
        while (entries.next(entry, scan_error)) {
            int fd;
            const auto parsed =
                std::from_chars(entry.name.data(), entry.name.data() + entry.name.size(), fd);
            if (parsed.ec == std::errc{} && parsed.ptr == entry.name.data() + entry.name.size() &&
                fd > STDERR_FILENO && fd != scan_fd.get())
                close(fd);
        }
    }
    for (int i = 0; i < 150; ++i) {  // ~5 min ceiling
        char buf[PROP_VALUE_MAX] = {};
        if (__system_property_get("sys.boot_completed", buf) > 0 && buf[0] == '1') {
            break;
        }
        sleep(2);
    }
    recovery_boot_completed();
    _exit(0);
#endif
}
}  // namespace

std::vector<BackendStatus> backend_statuses() {
    return backend_statuses(::kagami::kasumi::version_info(), count_committed_mounts());
}

std::vector<BackendStatus> backend_statuses(const ::kagami::kasumi::VersionInfo& version,
                                            int magic_mounts) {
    const bool kasumi_available = version.status == ::kagami::kasumi::Status::Available;
    std::ostringstream kasumi_detail;
    kasumi_detail << "protocol expected=" << version.expected_protocol
                  << " kernel=" << version.kernel_protocol << " uid=" << version.process_uid
                  << " euid=" << version.process_euid
                  << " status=" << static_cast<int>(version.status);

    const bool overlayfs_available = proc_filesystems_has("overlay");

    std::ostringstream magic_detail;
    magic_detail << "Magisk-style systemless mount; live mounts=" << magic_mounts;

    return {
        {BackendKind::Kasumi, "Kasumi", kasumi_available, true, kasumi_detail.str()},
        {BackendKind::Overlayfs, "OverlayFS", overlayfs_available, false,
         overlayfs_available ? "overlay filesystem registered"
                             : "overlay filesystem is not registered"},
        {BackendKind::MagicMount, "Magic Mount", true, false, magic_detail.str()},
    };
}

std::vector<ModuleEntry> enumerate_mountable_modules(const Config& config, int& error) {
    error = 0;
    std::vector<ModuleEntry> out;
    const fs::path root = config.module_dir.empty() ? runtime_modules_dir()
                                                    : fs::path(std::string_view(config.module_dir));
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return out;
    }
    fs::directory_iterator entry(root, ec);
    const fs::directory_iterator end;
    if (ec) {
        error = ec.value();
        return out;
    }
    for (; entry != end; entry.increment(ec)) {
        if (ec)
            break;
        const auto& e = *entry;
        std::error_code metadata_error;
        if (!e.is_directory(metadata_error)) {
            continue;
        }
        const fs::path& p = e.path();
        if (!fs::exists(p / "module.prop", metadata_error)) {
            continue;
        }
        if (fs::exists(p / "disable", metadata_error) || fs::exists(p / "remove", metadata_error) ||
            fs::exists(p / "skip_mount", metadata_error) ||
            fs::exists(runtime_data_dir() / "run" / "hot_unmounted" / p.filename(),
                       metadata_error)) {
            continue;
        }
        out.push_back({p.filename().string(), p});
    }
    if (ec) {
        error = ec.value();
        return out;
    }
    std::sort(out.begin(), out.end(),
              [](const ModuleEntry& a, const ModuleEntry& b) { return a.id < b.id; });
    return out;
}

// Per-module control-plane state is separate from module files so the manager
// can select backends without modifying another module's contents.
ModuleModes load_module_modes(int& error) {
    ModuleModes modes;
    (void)modes.prepare_modes((runtime_data_dir() / "module_mode.json").c_str(), error);
    return modes;
}
ModuleRules load_module_rules(int& error) {
    ModuleRules rules;
    (void)rules.prepare_rules((runtime_data_dir() / "module_rules.json").c_str(), error);
    return rules;
}
namespace {
template <typename Serialize>
bool save_settings(const char* name, const Serialize& serialize, int& error) {
    bounded::Storage bytes;
    if (!bytes.prepare_storage(PreparedInput::maximum, PreparedInput::maximum)) {
        error = ENOMEM;
        return false;
    }
    bounded::Writer output(bytes.data(), bytes.size());
    if (!serialize(output)) {
        error = EOVERFLOW;
        return false;
    }
    std::string message;
    if (!write_file_atomic((runtime_data_dir() / name).string(), output.view(), message)) {
        error = errno ? errno : EIO;
        return false;
    }
    return true;
}
}  // namespace
bool update_module_mode(std::string_view id, std::string_view mode, int& error) {
    const auto modes = load_module_modes(error);
    return !error && save_settings(
                         "module_mode.json",
                         [&](bounded::Writer& output) noexcept {
                             return modes.write_update(output, id, mode);
                         },
                         error);
}
bool update_module_rule(std::string_view id, std::string_view path, std::string_view mode,
                        bool remove, int& error) {
    const auto rules = load_module_rules(error);
    if (error || (remove && rules.find(id).empty()))
        return !error;
    return save_settings(
        "module_rules.json",
        [&](bounded::Writer& output) noexcept {
            return rules.write_update(output, id, path, mode, remove);
        },
        error);
}

namespace {
bool kasumi_usable() {
    const auto version = ::kagami::kasumi::version_info();
    return version.status == ::kagami::kasumi::Status::Available;
}

bool overlay_usable(BackendAvailability& availability) {
    if (!availability.overlay.has_value())
        availability.overlay = proc_filesystems_has("overlay");
    return *availability.overlay;
}

}  // namespace

BackendPlan prepare_backend_plan(const Config& config, std::string_view mode,
                                 BackendAvailability& observed, bool allow_kasumi) {
    if (!config.mount_backend.empty() && config.mount_backend != "auto")
        mode = config.mount_backend;
    if (mode == "none")
        return {"none", "none", false};
    if (mode == "kasumi" && allow_kasumi && config.kasumi_enabled) {
        if (!observed.kasumi.has_value())
            observed.kasumi = kasumi_usable();
        if (*observed.kasumi)
            return {"kasumi", "kasumi", false};
    }
    if (mode == "overlay" && config.overlayfs_enabled && overlay_usable(observed))
        return {"overlay", "overlay", false};
    if (mode == "magic" && config.magic_mount_enabled)
        return {"magic", "magic", false};
    const std::string_view fallback = config.magic_mount_enabled ? "magic" : "none";
    if (config.overlayfs_enabled && overlay_usable(observed))
        return {"overlay", fallback, true};
    return {fallback, fallback, false};
}

// Rewrite Kagami's own module.prop description so the manager's module list shows
// the live mount status (like hymo). Keeps the file's inode/context (truncate).
namespace {
void update_self_status(bool ok, std::size_t overlay, std::size_t magic, std::size_t kasumi) {
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
}  // namespace

namespace {
MountReport mount_all_enabled_impl(const Config& config) {
    MountReport report;
    fsutil::mlog("mount plan begin config=" + runtime_config_file().string());

    if (!claim_mount_orchestrator()) {
        report.detail = "module mount-all already ran this boot or its state could not be recorded";
        fsutil::mlog(report.detail, logging::Level::Warning);
        return report;
    }
    kasumi::invalidate_active_state();
    kasumi::clear_replayable_mappings();

    // An LKM is an explicit boot-time choice. lkm::autoload() owns the persisted
    // setting (including the legacy-file migration) and is a no-op when disabled.
#if !defined(KAGAMI_EMBEDDED)
    if (!lkm::autoload()) {
        fsutil::mlog("Kasumi LKM autoload failed: " + lkm::last_error(), logging::Level::Warning);
    }
#endif

    const bool kasumi_present =
        ::kagami::kasumi::module_loaded() || ::kagami::kasumi::is_available();
    bool initial_kasumi_ok = true;
    if (!config.kasumi_enabled && kasumi_present) {
        std::string error;
        if (!kasumi::deactivate(error)) {
            fsutil::mlog("kasumi: disable failed: " + error, logging::Level::Error);
            initial_kasumi_ok = false;
        }
    }
    int scan_error = 0;
    const auto modules = enumerate_mountable_modules(config, scan_error);
    if (scan_error) {
        report.detail = "module enumeration failed: " + std::string(std::strerror(scan_error));
        return report;
    }
    report.modules = static_cast<int>(modules.size());
    // A claimed boot starts a fresh inventory. Backend-specific evidence is
    // published by the child that performs the actual attachment.
    std::vector<module_runtime::Assignment> assignments;
    assignments.reserve(modules.size());
    for (const auto& module : modules) {
        assignments.push_back({module.id, "none"});
    }
    (void)module_runtime::assign_modules(assignments);

    // Bootloop protection: refuse to mount if previous boots never confirmed
    // completion, so a bad mount cannot brick boot. The counter is bumped here
    // and cleared by recovery_boot_completed() once boot_completed is reached.
    std::error_code ec;
    fs::create_directories(recovery_attempts_file().parent_path(), ec);
    if (fs::exists(recovery_disabled_file(), ec)) {
        bool deactivate_ok = true;
        if (kasumi_present) {
            std::string error;
            deactivate_ok = kasumi::deactivate(error);
            if (!deactivate_ok) {
                fsutil::mlog("kasumi: bootloop disable failed: " + error, logging::Level::Error);
            }
        }
        report.ok = deactivate_ok;
        report.detail =
            deactivate_ok ? "mounting disabled by bootloop protection; run 'kagamid recovery reset'"
                          : "mounting disabled, but Kasumi cleanup failed (see controller log)";
        return report;
    }
    const int attempts = read_boot_attempts();
    if (attempts >= kMaxBootAttempts) {
        bool deactivate_ok = true;
        if (kasumi_present) {
            std::string error;
            deactivate_ok = kasumi::deactivate(error);
            if (!deactivate_ok) {
                fsutil::mlog("kasumi: bootloop disable failed: " + error, logging::Level::Error);
            }
        }
        std::ofstream(recovery_disabled_file().string(), std::ios::trunc).put('\n');
        report.ok = deactivate_ok;
        report.detail =
            deactivate_ok
                ? "bootloop protection tripped after " + std::to_string(attempts) +
                      " unconfirmed boots; mounting disabled (run 'kagamid recovery reset')"
                : "bootloop protection tripped, but Kasumi cleanup failed (see controller log)";
        return report;
    }
    write_boot_attempts(attempts + 1);

    const bool manage_kasumi = config.kasumi_enabled && ::kagami::kasumi::is_available();
    bool kasumi_prepare_ok = true;
    if (manage_kasumi) {
        std::string error;
        kasumi_prepare_ok = kasumi::deactivate(error);
        if (!kasumi_prepare_ok) {
            fsutil::mlog("kasumi: reset before rebuild failed: " + error, logging::Level::Error);
        }
    }

    // Orchestrate per module: a global override (config.mount_backend != "auto")
    // forces every module; otherwise each module's mode (module_mode.json, default
    // "auto") decides, with auto following OverlayFS -> Magic Mount -> none.
    // Kasumi is selected only by an explicit module/global setting.
    int settings_error = 0;
    const auto modes = load_module_modes(settings_error);
    if (settings_error) {
        report.detail = "cannot read module modes: " + std::string(std::strerror(settings_error));
        return report;
    }
    const auto rules = load_module_rules(settings_error);
    if (settings_error) {
        report.detail = "cannot read module rules: " + std::string(std::strerror(settings_error));
        return report;
    }
    std::vector<ModuleEntry> overlay_set;
    std::vector<ModuleEntry> magic_set;
    std::vector<ModuleEntry> kasumi_set;
    BackendAvailability availability;
    assignments.clear();
    ModuleScanner scanner;
    if (!modules.empty() && !scanner.prepare_scan(config, false)) {
        report.detail = "cannot prepare backend selection storage";
        return report;
    }
    for (const auto& m : modules) {
        const auto configured = modes.find(m.id);
        const std::string_view selected = configured.empty() ? "auto" : configured;
        const auto plan = prepare_backend_plan(config, selected, availability, true);
        ModuleFacts facts;
        ModuleProperties ignored;
        if (!scanner.observe(m.path.c_str(), facts, ignored, scan_error, plan.inspect_root_files,
                             false)) {
            report.detail = "module scan failed: " + std::string(std::strerror(scan_error));
            return report;
        }
        const std::string mode(plan.select(facts));
        assignments.push_back({m.id, mode});
        fsutil::mlog("selected module=" + m.id + " backend=" + mode);
        if (mode == "overlay") {
            overlay_set.push_back(m);
        } else if (mode == "magic") {
            magic_set.push_back(m);
        } else if (mode == "kasumi") {
            kasumi_set.push_back(m);
        }
        // "none" → skip
    }
    (void)module_runtime::assign_modules(assignments);

    fsutil::mlog("orchestrator: overlay=" + std::to_string(overlay_set.size()) + " magic=" +
                 std::to_string(magic_set.size()) + " kasumi=" + std::to_string(kasumi_set.size()));

    bool non_kasumi_mounts_ok = true;
    bool kasumi_rules_ok = true;
    if (!overlay_set.empty() || !magic_set.empty()) {
        non_kasumi_mounts_ok = fsutil::run_in_init_mount_ns([&]() {
            bool r = true;
            // Preserve backend layering: OverlayFS is applied above Magic Mount.
            if (!magic_set.empty()) {
                r = magic::mount_modules(magic_set, config) && r;
            }
            if (!overlay_set.empty()) {
                r = overlay::mount_modules(overlay_set, config) && r;
            }
            return r;
        });
    }
    if (manage_kasumi) {
        kasumi_rules_ok = kasumi_prepare_ok && fsutil::run_in_init_mount_ns([&]() {
                              return kasumi::mount_modules(kasumi_set, config, rules);
                          });
        if (kasumi_rules_ok && !kasumi_set.empty() &&
            !kasumi::record_replayable_mappings(kasumi_set)) {
            kasumi_rules_ok = false;
            fsutil::mlog("kasumi: failed to commit the boot mapping plan", logging::Level::Error);
        }
        if (!kasumi_rules_ok || kasumi_set.empty()) {
            kasumi::clear_replayable_mappings();
        }
    } else {
        kasumi::clear_replayable_mappings();
    }

    bool policy_ok = true;
    bool features_ok = true;
    if (manage_kasumi && kasumi_prepare_ok && kasumi_rules_ok) {
        policy_ok = apply_kasumi_policy(config);
        features_ok = policy_ok && apply_kasumi_features(config);
    } else if (manage_kasumi) {
        policy_ok = false;
        features_ok = false;
    }
    bool kasumi_ok =
        initial_kasumi_ok && kasumi_prepare_ok && policy_ok && features_ok && kasumi_rules_ok;
    if (manage_kasumi) {
        if (!::kagami::kasumi::set_enabled(kasumi_ok)) {
            kasumi_ok = false;
        }
        if (!kasumi_ok) {
            std::string error;
            (void)kasumi::deactivate(error);
        }
    }
    const bool ok = non_kasumi_mounts_ok && policy_ok && features_ok && kasumi_ok;

    report.backend = "hybrid(overlay=" + std::to_string(overlay_set.size()) +
                     ",magic=" + std::to_string(magic_set.size()) +
                     ",kasumi=" + std::to_string(kasumi_set.size()) + ")";
    report.ok = ok;
    report.mounts = static_cast<int>(magic::active_mounts(config).size());
    report.detail = ok ? "ok" : "some backends reported errors (see controller log)";
    fsutil::mlog("mount plan complete backend=" + report.backend + " modules=" +
                     std::to_string(report.modules) + " result=" + (ok ? "ok" : "failed"),
                 ok ? logging::Level::Info : logging::Level::Error);
    update_self_status(ok, overlay_set.size(), magic_set.size(), kasumi_set.size());
    if (ok) {
        spawn_boot_completed_watcher();  // clears the bootloop counter once boot completes
    }
    return report;
}

}  // namespace

MountReport mount_all_enabled(const Config& config) {
    module_runtime::Operation operation;
    auto report = mount_all_enabled_impl(config);
    operation.finish(report.ok);
    return report;
}

bool unmount_all(const Config& config) {
    module_runtime::Operation operation;
    // Unwind the hybrid plan from its topmost backend.
    const bool ok = fsutil::run_in_init_mount_ns([&]() {
        bool r = true;
        r = kasumi::unmount_all(config) && r;
        r = overlay::unmount_all(config) && r;
        r = magic::unmount_all(config) && r;
        r = storage::teardown_shared(config) && r;
        return r;
    });
    operation.finish(ok);
    return ok;
}

namespace {
bool refresh_kasumi_modules_impl(const Config& config) {
    if (!config.kasumi_enabled) {
        if (!::kagami::kasumi::module_loaded() && !::kagami::kasumi::is_available()) {
            return true;
        }
        std::string error;
        if (!kasumi::deactivate(error)) {
            fsutil::mlog("kasumi: disable failed: " + error, logging::Level::Error);
            return false;
        }
        return true;
    }
    if (!kasumi_usable()) {
        return false;
    }
    const auto replayable = kasumi::replayable_module_ids();
    if (replayable.empty()) {
        fsutil::mlog("kasumi: refusing post-boot mapping replay without a "
                     "Kasumi-owned boot plan",
                     logging::Level::Error);
        return false;
    }
    const std::set<std::string> replayable_set(replayable.begin(), replayable.end());
    int scan_error = 0;
    const auto all = enumerate_mountable_modules(config, scan_error);
    if (scan_error) {
        errno = scan_error;
        return false;
    }
    std::vector<ModuleEntry> selected;
    for (const auto& module : all) {
        if (replayable_set.count(module.id) != 0) {
            selected.push_back(module);
        }
    }
    std::vector<module_runtime::Assignment> assignments;
    assignments.reserve(selected.size());
    for (const auto& module : selected) {
        assignments.push_back({module.id, "kasumi"});
    }
    int settings_error = 0;
    const auto rules = load_module_rules(settings_error);
    if (settings_error) {
        errno = settings_error;
        return false;
    }
    std::string error;
    const bool prepare_ok = kasumi::deactivate(error);
    if (!prepare_ok) {
        fsutil::mlog("kasumi: reset before refresh failed: " + error, logging::Level::Error);
    } else {
        (void)module_runtime::assign_modules(assignments, "kasumi");
    }
    const bool mount_ok = prepare_ok && fsutil::run_in_init_mount_ns([&]() {
                              return kasumi::mount_modules(selected, config, rules);
                          });
    const bool overlay_xattr_ok = mount_ok && fsutil::run_in_init_mount_ns([&]() {
                                      return overlay::restore_xattr_hiding(config);
                                  });
    const bool policy_ok = overlay_xattr_ok && apply_kasumi_policy(config);
    const bool features_ok = policy_ok && apply_kasumi_features(config);
    bool ok = prepare_ok && mount_ok && overlay_xattr_ok && policy_ok && features_ok;
    if (!::kagami::kasumi::set_enabled(ok)) {
        ok = false;
    }
    if (!ok) {
        std::string cleanup_error;
        (void)kasumi::deactivate(cleanup_error);
    }
    return ok;
}

}  // namespace

bool refresh_kasumi_modules(const Config& config) {
    module_runtime::Operation operation;
    const bool ok = refresh_kasumi_modules_impl(config);
    operation.finish(ok);
    return ok;
}

void recovery_boot_completed() {
    std::error_code ec;
    fs::remove(recovery_attempts_file(), ec);  // boot confirmed: clear the counter
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
    out << "{\"boot_attempts\":" << read_boot_attempts() << ",\"max_attempts\":" << kMaxBootAttempts
        << ",\"mounting_disabled\":" << (disabled ? "true" : "false") << "}";
    return out.str();
}

}  // namespace kagami::mount
