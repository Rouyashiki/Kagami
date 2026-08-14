#include "kagami/kasumi_client.hpp"

#include "kagami/kasumi_uapi_compat.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace kagami::kasumi {

static_assert(static_cast<std::uint32_t>(QuiesceState::Active) ==
              KSM_QUIESCE_STATE_ACTIVE);
static_assert(static_cast<std::uint32_t>(QuiesceState::Draining) ==
              KSM_QUIESCE_STATE_DRAINING);
static_assert(static_cast<std::uint32_t>(QuiesceState::Ready) ==
              KSM_QUIESCE_STATE_READY);
static_assert(static_cast<std::uint32_t>(QuiesceState::Failed) ==
              KSM_QUIESCE_STATE_FAILED);
static_assert(static_cast<std::uint32_t>(MountHideMode::Normal) ==
              KSM_MOUNT_HIDE_MODE_NORMAL);
static_assert(static_cast<std::uint32_t>(MountHideMode::Aggressive) ==
              KSM_MOUNT_HIDE_MODE_AGGRESSIVE);
static_assert(sizeof(kasumi_quiesce_arg) == 64);

std::string default_mirror_path() {
    return "/dev/kagami_mirror";
}

static bool lkm_in_proc_modules() {
#if defined(__linux__)
    std::ifstream modules("/proc/modules");
    std::string line;
    while (std::getline(modules, line)) {
        if (line.compare(0, 11, "kasumi_lkm ") == 0 ||
            line.compare(0, 11, "kasumi_lkm\t") == 0) {
            return true;
        }
    }
#endif
    return false;
}

#if defined(__linux__)
static int s_kasumi_fd = -1;
static int s_last_getfd_errno = 0;
static bool s_connection_persistent = false;
static bool s_protocol_checked = false;

static int anon_fd() {
    if (s_kasumi_fd >= 0) {
        return s_kasumi_fd;
    }

    int fd = -1;
    errno = 0;
    syscall(SYS_reboot, KSM_MAGIC1, KSM_MAGIC2, KSM_CMD_GET_FD, &fd);
    s_last_getfd_errno = errno;
    if (fd >= 0) {
        s_kasumi_fd = fd;
        s_last_getfd_errno = 0;
    }
    return fd;
}
#endif

static int execute(unsigned long cmd, void* arg) {
#if defined(__linux__)
    const int fd = anon_fd();
    if (fd < 0) {
#if defined(__linux__)
        if (s_last_getfd_errno != 0) {
            errno = s_last_getfd_errno;
        } else {
            errno = ENODEV;
        }
#else
        errno = ENODEV;
#endif
        return -1;
    }
    if (cmd != KSM_IOC_GET_VERSION && !s_protocol_checked) {
        int version = 0;
        if (ioctl(fd, KSM_IOC_GET_VERSION, &version) != 0 ||
            version != KSM_PROTOCOL_VERSION) {
            const int saved_errno = version != KSM_PROTOCOL_VERSION
                                        ? EPROTO
                                        : (errno != 0 ? errno : EIO);
            close(s_kasumi_fd);
            s_kasumi_fd = -1;
            s_protocol_checked = false;
            errno = saved_errno;
            return -1;
        }
        s_protocol_checked = true;
    }
    const int rc = ioctl(fd, cmd, arg);
    const int saved_errno = errno;
    if (!s_connection_persistent) {
        close(s_kasumi_fd);
        s_kasumi_fd = -1;
        s_protocol_checked = false;
    }
    errno = saved_errno;
    return rc;
#else
    (void)cmd;
    (void)arg;
    errno = ENOSYS;
    return -1;
#endif
}

static bool ioctl_arg_ok(int rc, int arg_err) {
    if (rc != 0) {
        return false;
    }
    if (arg_err != 0) {
        errno = arg_err < 0 ? -arg_err : arg_err;
        return false;
    }
    return true;
}

VersionInfo version_info() {
    VersionInfo info;
    info.expected_protocol = KSM_PROTOCOL_VERSION;
    info.process_uid = process_uid();
    info.process_euid = process_euid();
    info.modules_visible = lkm_in_proc_modules();

    int version = 0;
    if (execute(KSM_IOC_GET_VERSION, &version) != 0) {
        info.last_errno = errno;
        info.status = Status::NotPresent;
        return info;
    }

    info.kernel_protocol = version;
    if (version < KSM_PROTOCOL_VERSION) {
        info.status = Status::KernelTooOld;
    } else if (version > KSM_PROTOCOL_VERSION) {
        info.status = Status::ClientTooOld;
    } else {
        info.status = Status::Available;
    }
    return info;
}

int last_getfd_errno() {
#if defined(__linux__)
    return s_last_getfd_errno;
#else
    return ENOSYS;
#endif
}

int process_uid() {
#if defined(__linux__)
    return static_cast<int>(getuid());
#else
    return -1;
#endif
}

int process_euid() {
#if defined(__linux__)
    return static_cast<int>(geteuid());
#else
    return -1;
#endif
}

bool is_available() {
    return version_info().status == Status::Available;
}

bool module_loaded() { return lkm_in_proc_modules(); }

void set_connection_persistent(bool persistent) {
#if defined(__linux__)
    s_connection_persistent = persistent;
    if (!persistent && s_kasumi_fd >= 0) {
        close(s_kasumi_fd);
        s_kasumi_fd = -1;
        s_protocol_checked = false;
    }
#else
    (void)persistent;
#endif
}

void release_connection() {
#if defined(__linux__)
    if (s_kasumi_fd >= 0) {
        close(s_kasumi_fd);
        s_kasumi_fd = -1;
    }
    s_protocol_checked = false;
    s_last_getfd_errno = 0;
#endif
}

std::string active_rules() {
    std::vector<char> buffer(64 * 1024, '\0');
    kasumi_syscall_list_arg arg = {};
    arg.buf = buffer.data();
    arg.size = buffer.size();
    if (execute(KSM_IOC_LIST_RULES, &arg) != 0) {
        return "";
    }
    return std::string(buffer.data());
}

std::string hooks() {
    std::vector<char> buffer(8 * 1024, '\0');
    kasumi_syscall_list_arg arg = {};
    arg.buf = buffer.data();
    arg.size = buffer.size();
    if (execute(KSM_IOC_GET_HOOKS, &arg) != 0) {
        return "";
    }
    return std::string(buffer.data());
}

FeatureCapabilities feature_capabilities() {
    FeatureCapabilities capabilities;
    int bitmask = 0;
    if (execute(KSM_IOC_GET_FEATURES, &bitmask) != 0) {
        capabilities.last_errno = errno != 0 ? errno : EIO;
        return capabilities;
    }
    capabilities.ok = true;
    capabilities.bitmask = bitmask;
    capabilities.quiesce = (bitmask & KSM_FEATURE_QUIESCE) != 0;
    return capabilities;
}

int features() {
    return feature_capabilities().bitmask;
}

std::vector<std::string> feature_names(int bitmask) {
    std::vector<std::string> names;
    if (bitmask & KSM_FEATURE_MOUNT_HIDE)
        names.emplace_back("mount_hide");
    if (bitmask & KSM_FEATURE_MAPS_SPOOF)
        names.emplace_back("maps_spoof");
    if (bitmask & KSM_FEATURE_STATFS_SPOOF)
        names.emplace_back("statfs_spoof");
    if (bitmask & KSM_FEATURE_CMDLINE_SPOOF)
        names.emplace_back("cmdline_spoof");
    if (bitmask & KSM_FEATURE_KSTAT_SPOOF)
        names.emplace_back("kstat_spoof");
    if (bitmask & KSM_FEATURE_MERGE_DIR)
        names.emplace_back("merge_dir");
    if (bitmask & KSM_FEATURE_SELINUX_BYPASS)
        names.emplace_back("selinux_bypass");
    if (bitmask & KSM_FEATURE_FAKE_MOUNTINFO)
        names.emplace_back("fake_mountinfo");
    if (bitmask & KSM_FEATURE_MOUNT_HIDE_AGGRESSIVE)
        names.emplace_back("mount_hide_aggressive");
    if (bitmask & KSM_FEATURE_SELINUX_FIX)
        names.emplace_back("selinux_fix");
    if (bitmask & KSM_FEATURE_QUIESCE)
        names.emplace_back("quiesce");
    return names;
}

QuiesceSnapshot prepare_unload() {
    QuiesceSnapshot snapshot;
    kasumi_quiesce_arg arg = {};
    arg.version = KSM_QUIESCE_API_VERSION;
    arg.size = static_cast<__u32>(sizeof(arg));

    const int rc = execute(KSM_IOC_PREPARE_UNLOAD, &arg);
    snapshot.version = arg.version;
    snapshot.size = arg.size;
    snapshot.flags = arg.flags;
    snapshot.state = static_cast<QuiesceState>(arg.state);
    snapshot.busy_mask = arg.busy_mask;
    snapshot.pending_getfd = arg.pending_getfd;
    snapshot.pending_marker = arg.pending_marker;
    snapshot.pending_redirect = arg.pending_redirect;
    snapshot.live_proc_proxy = arg.live_proc_proxy;
    snapshot.live_file_view = arg.live_file_view;
    snapshot.control_files = arg.control_files;
    snapshot.module_refs = arg.module_refs;
    snapshot.err = arg.err;
    if (rc != 0) {
        snapshot.last_errno = errno != 0 ? errno : EIO;
        return snapshot;
    }
    if (arg.err != 0) {
        snapshot.last_errno = arg.err < 0 ? -arg.err : arg.err;
        errno = snapshot.last_errno;
        return snapshot;
    }
    snapshot.ok = true;
    return snapshot;
}

std::vector<std::string> active_modules_from_rules(const std::string& rules) {
    std::set<std::string> modules;
    std::istringstream lines(rules);
    std::string line;
    while (std::getline(lines, line)) {
        const std::vector<std::string> prefixes = {
            "/data/adb/modules/", "/dev/kagami_mirror/", "/mnt/",
        };
        for (const auto& prefix : prefixes) {
            std::size_t pos = line.find(prefix);
            while (pos != std::string::npos) {
                const std::size_t start = pos + prefix.size();
                const std::size_t end = line.find('/', start);
                if (end != std::string::npos && end > start) {
                    modules.insert(line.substr(start, end - start));
                }
                pos = line.find(prefix, start);
            }
        }
    }
    return {modules.begin(), modules.end()};
}

bool set_enabled(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_ENABLED, &value) == 0;
}

bool set_debug(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_DEBUG, &value) == 0;
}

bool set_stealth(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_STEALTH, &value) == 0;
}

bool fix_mounts() { return execute(KSM_IOC_REORDER_MNT_ID, nullptr) == 0; }

bool hide_overlay_xattrs(const std::string& path) {
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(KSM_IOC_HIDE_OVERLAY_XATTRS, &arg) == 0;
}

bool set_mount_hide(bool enable, MountHideMode mode) {
    const int bitmask = features();
    const bool mode_supported =
        (bitmask & KSM_FEATURE_MOUNT_HIDE_AGGRESSIVE) != 0;

    if (enable && mode == MountHideMode::Aggressive && !mode_supported) {
        errno = EOPNOTSUPP;
        return false;
    }
    if (mode_supported) {
        int raw_mode = static_cast<int>(mode);
        if (execute(KSM_IOC_SET_MOUNT_HIDE_MODE, &raw_mode) != 0)
            return false;
    }
    kasumi_mount_hide_arg arg = {};
    arg.enable = enable ? 1 : 0;
    return ioctl_arg_ok(execute(KSM_IOC_SET_MOUNT_HIDE, &arg), arg.err);
}

bool set_maps_spoof(bool enable) {
    kasumi_maps_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    return ioctl_arg_ok(execute(KSM_IOC_SET_MAPS_SPOOF, &arg), arg.err);
}

bool set_statfs_spoof(bool enable) {
    kasumi_statfs_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    return ioctl_arg_ok(execute(KSM_IOC_SET_STATFS_SPOOF, &arg), arg.err);
}

bool set_selinux_guard(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SELINUX_FIX, &value) == 0;
}

bool set_cmdline(const std::string& cmdline) {
    kasumi_spoof_cmdline arg = {};
    std::strncpy(arg.cmdline, cmdline.c_str(), KSM_FAKE_CMDLINE_SIZE - 1);
    return ioctl_arg_ok(execute(KSM_IOC_SET_CMDLINE, &arg), arg.err);
}

bool clear_rules() { return execute(KSM_IOC_CLEAR_ALL, nullptr) == 0; }

bool add_rule(const std::string& target, const std::string& source, int type) {
    kasumi_syscall_arg arg = {};
    arg.src = target.c_str();
    arg.target = source.c_str();
    arg.type = type;
    return execute(KSM_IOC_ADD_RULE, &arg) == 0;
}

bool add_merge_rule(const std::string& target, const std::string& source) {
    kasumi_syscall_arg arg = {};
    arg.src = target.c_str();
    arg.target = source.c_str();
    return execute(KSM_IOC_ADD_MERGE_RULE, &arg) == 0;
}

bool set_mirror_path(const std::string& path) {
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(KSM_IOC_SET_MIRROR_PATH, &arg) == 0;
}

bool hide_path(const std::string& path) {
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(KSM_IOC_HIDE_RULE, &arg) == 0;
}

bool delete_rule(const std::string& path) {
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(KSM_IOC_DEL_RULE, &arg) == 0;
}

bool add_maps_rule(unsigned long target_ino, unsigned long target_dev, unsigned long spoofed_ino, unsigned long spoofed_dev, const std::string& spoofed_path) {
    kasumi_maps_rule arg = {};
    arg.target_ino = target_ino;
    arg.target_dev = target_dev;
    arg.spoofed_ino = spoofed_ino;
    arg.spoofed_dev = spoofed_dev;
    std::strncpy(arg.spoofed_pathname, spoofed_path.c_str(), KSM_MAX_LEN_PATHNAME - 1);
    return ioctl_arg_ok(execute(KSM_IOC_ADD_MAPS_RULE, &arg), arg.err);
}

bool clear_maps_rules() {
    return execute(KSM_IOC_CLEAR_MAPS_RULES, nullptr) == 0;
}

static PolicyState policy_state() {
    PolicyState state;
    kasumi_policy_state_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    if (execute(KSM_IOC_GET_POLICY, &arg) != 0) {
        state.last_errno = errno;
        state.err = errno == 0 ? -1 : -errno;
        return state;
    }
    state.version = arg.version;
    state.owner = static_cast<PolicyOwner>(arg.owner);
    state.effective_owner = static_cast<PolicyOwner>(arg.effective_owner);
    state.flags = arg.flags;
    state.detected_roots = arg.detected_roots;
    state.allow_count = arg.allow_count;
    state.deny_count = arg.deny_count;
    state.max_uid_count = arg.max_uid_count;
    state.generation = arg.generation;
    state.enabled = arg.enabled != 0;
    state.err = arg.err;
    state.ok = arg.err == 0;
    return state;
}

static bool read_policy_uids(PolicyUidList list, std::uint32_t count,
                             std::vector<std::uint32_t>& uids,
                             std::uint64_t& generation) {
    uids.assign(count, 0);
    kasumi_policy_uid_list_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    arg.list = static_cast<std::uint32_t>(list);
    arg.count = count;
    arg.uids = uids.empty()
        ? 0
        : static_cast<__aligned_u64>(reinterpret_cast<std::uintptr_t>(uids.data()));
    if (!ioctl_arg_ok(execute(KSM_IOC_GET_POLICY_UIDS, &arg), arg.err)) {
        return false;
    }
    if (arg.count < uids.size()) {
        uids.resize(arg.count);
    }
    generation = arg.generation;
    return true;
}

PolicySnapshot policy_snapshot() {
    PolicySnapshot snapshot;
    for (int attempt = 0; attempt < 3; ++attempt) {
        snapshot = {};
        snapshot.state = policy_state();
        if (!snapshot.state.ok) {
            return snapshot;
        }

        std::uint64_t allow_generation = 0;
        std::uint64_t deny_generation = 0;
        if (!read_policy_uids(PolicyUidList::Allow, snapshot.state.allow_count,
                              snapshot.allow_uids, allow_generation) ||
            !read_policy_uids(PolicyUidList::Deny, snapshot.state.deny_count,
                              snapshot.deny_uids, deny_generation)) {
            snapshot.state.ok = false;
            snapshot.state.last_errno = errno;
            snapshot.state.err = errno == 0 ? -1 : -errno;
            return snapshot;
        }
        if (snapshot.state.generation == allow_generation &&
            snapshot.state.generation == deny_generation) {
            return snapshot;
        }
    }

    errno = EAGAIN;
    snapshot = {};
    snapshot.state.last_errno = EAGAIN;
    snapshot.state.err = -EAGAIN;
    return snapshot;
}

bool replace_policy(PolicyOwner owner, std::uint32_t flags,
                    const std::vector<std::uint32_t>& allow_uids,
                    const std::vector<std::uint32_t>& deny_uids) {
    kasumi_policy_replace_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    arg.owner = static_cast<std::uint32_t>(owner);
    arg.flags = flags;
    arg.allow_count = static_cast<std::uint32_t>(allow_uids.size());
    arg.deny_count = static_cast<std::uint32_t>(deny_uids.size());
    arg.allow_uids = allow_uids.empty()
        ? 0
        : static_cast<__aligned_u64>(reinterpret_cast<std::uintptr_t>(allow_uids.data()));
    arg.deny_uids = deny_uids.empty()
        ? 0
        : static_cast<__aligned_u64>(reinterpret_cast<std::uintptr_t>(deny_uids.data()));
    return ioctl_arg_ok(execute(KSM_IOC_REPLACE_POLICY, &arg), arg.err);
}

bool reset_policy() {
    kasumi_policy_config_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    return ioctl_arg_ok(execute(KSM_IOC_RESET_POLICY, &arg), arg.err);
}

} // namespace kagami::kasumi
