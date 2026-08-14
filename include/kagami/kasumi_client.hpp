#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace kagami::kasumi {

enum class Status {
    Available = 0,
    NotPresent = 1,
    KernelTooOld = 2,
    ClientTooOld = 3,
};

struct VersionInfo {
    int expected_protocol = 0;
    int kernel_protocol = 0;
    int last_errno = 0;
    int process_uid = -1;
    int process_euid = -1;
    bool modules_visible = false;
    Status status = Status::NotPresent;
};

struct FeatureCapabilities {
    bool ok = false;
    int last_errno = 0;
    int bitmask = 0;
    bool quiesce = false;
};

enum class QuiesceState : std::uint32_t {
    Active = 0,
    Draining = 1,
    Ready = 2,
    Failed = 3,
};

struct QuiesceSnapshot {
    bool ok = false;
    int last_errno = 0;
    std::uint32_t version = 0;
    std::uint32_t size = 0;
    std::uint32_t flags = 0;
    QuiesceState state = QuiesceState::Active;
    std::uint32_t busy_mask = 0;
    std::uint32_t pending_getfd = 0;
    std::uint32_t pending_marker = 0;
    std::uint32_t pending_redirect = 0;
    std::uint32_t live_proc_proxy = 0;
    std::uint32_t live_file_view = 0;
    std::uint32_t control_files = 0;
    std::uint32_t module_refs = 0;
    int err = 0;
};

enum class PolicyOwner : std::uint32_t {
    Auto = 0,
    KernelSU = 1,
    APatch = 2,
    Magisk = 3,
    Manual = 4,
    Disabled = 5,
};

enum class PolicyUidList : std::uint32_t {
    Allow = 1,
    Deny = 2,
    All = 3,
};

enum class MountHideMode : std::uint32_t {
    Normal = 0,
    Aggressive = 1,
};

struct PolicyState {
    bool ok = false;
    int last_errno = 0;
    int err = 0;
    std::uint32_t version = 0;
    PolicyOwner owner = PolicyOwner::Auto;
    PolicyOwner effective_owner = PolicyOwner::Auto;
    std::uint32_t flags = 0;
    std::uint32_t detected_roots = 0;
    std::uint32_t allow_count = 0;
    std::uint32_t deny_count = 0;
    std::uint32_t max_uid_count = 0;
    std::uint64_t generation = 0;
    bool enabled = false;
};

struct PolicySnapshot {
    PolicyState state;
    std::vector<std::uint32_t> allow_uids;
    std::vector<std::uint32_t> deny_uids;
};

std::string default_mirror_path();
VersionInfo version_info();
bool is_available();
bool module_loaded();
void set_connection_persistent(bool persistent);
void release_connection();
std::string active_rules();
std::string hooks();
FeatureCapabilities feature_capabilities();
int features();
std::vector<std::string> feature_names(int bitmask);
std::vector<std::string> active_modules_from_rules(const std::string& rules);
QuiesceSnapshot prepare_unload();

bool set_enabled(bool enable);
bool set_debug(bool enable);
bool set_stealth(bool enable);
bool fix_mounts();
bool hide_overlay_xattrs(const std::string& path);
bool set_mount_hide(bool enable, MountHideMode mode = MountHideMode::Normal);
bool set_maps_spoof(bool enable);
bool set_statfs_spoof(bool enable);
bool set_selinux_guard(bool enable);
bool set_cmdline(const std::string& cmdline);
bool clear_rules();
bool add_rule(const std::string& target, const std::string& source, int type);
bool add_merge_rule(const std::string& target, const std::string& source);
bool set_mirror_path(const std::string& path);
bool hide_path(const std::string& path);
bool delete_rule(const std::string& path);
bool add_maps_rule(unsigned long target_ino, unsigned long target_dev, unsigned long spoofed_ino, unsigned long spoofed_dev, const std::string& spoofed_path);
bool clear_maps_rules();
PolicySnapshot policy_snapshot();
bool replace_policy(PolicyOwner owner, std::uint32_t flags,
                    const std::vector<std::uint32_t>& allow_uids,
                    const std::vector<std::uint32_t>& deny_uids);
bool reset_policy();
int last_getfd_errno();
int process_uid();
int process_euid();

} // namespace kagami::kasumi
