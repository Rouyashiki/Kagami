#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/module_scan.hpp"
#include "mount/module_settings.hpp"

namespace kagami::mount {

enum class BackendKind {
    Kasumi,
    Overlayfs,
    MagicMount,
};

struct BackendStatus {
    BackendKind kind;
    std::string name;
    bool available;
    bool preferred;
    std::string detail;
};

std::vector<BackendStatus> backend_statuses();
std::vector<BackendStatus> backend_statuses(const ::kagami::kasumi::VersionInfo& version,
                                            int magic_mounts);
std::string backend_kind_name(BackendKind kind);

// A regular module eligible for mounting: /data/adb/modules/<id>.
struct ModuleEntry {
    std::string id;
    std::filesystem::path path;
};

// Reuse only while selecting one operation's modules, before mount/LKM changes.
struct BackendAvailability {
    std::optional<bool> overlay;
    std::optional<bool> kasumi;
};

struct BackendPlan {
    std::string_view selected;
    std::string_view fallback;
    bool inspect_root_files = false;
    std::string_view select(const ModuleFacts& facts) const noexcept {
        if (!facts.backend_content)
            return "none";
        return inspect_root_files && facts.needs_magic ? fallback : selected;
    }
};
BackendPlan prepare_backend_plan(const Config& config, std::string_view mode,
                                 BackendAvailability& observed, bool allow_kasumi);

struct MountReport {
    bool ok = false;
    int modules = 0;  // enabled modules processed
    int mounts = 0;   // committed partition mount points
    std::string backend;
    std::string detail;
};

// Enabled, mountable modules under runtime_modules_dir(): skips entries with
// no module.prop or with a disable/remove/skip_mount marker.
std::vector<ModuleEntry> enumerate_mountable_modules(const Config& config, int& error);

ModuleModes load_module_modes(int& error);
ModuleRules load_module_rules(int& error);
bool update_module_mode(std::string_view id, std::string_view mode, int& error);
bool update_module_rule(std::string_view id, std::string_view path, std::string_view mode,
                        bool remove, int& error);

// Rebuild only Kasumi mappings in the init namespace. This is safe to expose
// post-boot for the Kasumi hot-mount controls; Overlay/Magic remain boot-only.
bool refresh_kasumi_modules(const Config& config);

// Pick a backend (magic mount), enter the init mount ns, mount every enabled
// module. Safe to call from the daemon or a one-shot CLI.
MountReport mount_all_enabled(const Config& config);

// Tear down everything Kagami mounted (enters the init mount ns).
bool unmount_all(const Config& config);

// Bootloop protection: mount_all_enabled() bumps an unconfirmed-boot counter and
// refuses once it exceeds the limit. recovery_boot_completed() clears it once a
// boot reaches boot_completed; recovery_reset() re-enables after a trip.
void recovery_boot_completed();
void recovery_reset();
std::string recovery_status_json();

}  // namespace kagami::mount
