#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace kagami {

struct PolicyConfig {
    std::string owner = "auto";
    bool use_allow_uids = false;
    bool use_deny_uids = false;
    bool include_isolated_uids = false;
    std::vector<std::uint32_t> allow_uids;
    std::vector<std::uint32_t> deny_uids;
};

struct Config {
    std::string module_dir = "/data/adb/modules";
    std::string data_dir = "/data/adb/kagami";
    std::string log_file = "/data/adb/kagami/daemon.log";
    std::string mount_source = "KSU";
    std::string fs_type = "auto";
    // Work tmpfs for magic mount. Module files are copied here (NOT bind-mounted
    // from /data onto system). /dev is a writable tmpfs everywhere; /debug_ramdisk
    // is read-only on some devices. Configurable; avoid /mnt.
    std::string work_dir = "/dev/kagami";
    // Shared OverlayFS/Kasumi module mirror. It MUST NOT live under /data:
    // redirected files need the target partition's SELinux context, not
    // data_file. OverlayFS and Kasumi intentionally share this one mount so a
    // hybrid boot has a single tmpfs/loop-image lifetime. Magic Mount keeps its
    // independent work tmpfs and therefore does not create this base on its own.
    // fs_type selects the storage mode ("auto" tries tmpfs then ext4; "tmpfs"/
    // "ext4"/"erofs" force one). The ext4/erofs backing image persists on /data.
    std::string mirror_dir = "/dev/kagami_mirror";
    std::string mirror_img = "/data/adb/kagami/mirror.img";
    int mirror_img_size_mb = 2048;
    // Read-only overlay by default (lowerdirs only), matching meta-overlayfs. A
    // writable upper/work layer is opt-in: it breaks early-boot vendor/system init
    // on some devices (SELinux-denied writes through the overlay -> RIL fails).
    bool overlay_writable = false;
    std::vector<std::string> partitions;
    bool debug = false;
    bool verbose = false;
    // LKM loading is opt-in. A packaged Kasumi .ko must never be inserted just
    // because Kagami itself is enabled; explicit Kasumi users can enable this
    // boot-time action, or load the LKM manually from the control plane.
    bool lkm_autoload = false;
    bool kasumi_enabled = true;
    bool enable_kernel_debug = false;
    bool enable_stealth = true;
    bool enable_hidexattr = false;
    bool enable_selinux_fix = false;
    std::string uname_release;
    std::string uname_version;
    // Empty means scoped uname spoofing. "global" uses Kasumi protocol 15+
    // and changes init_uts_ns for every task, so it is always opt-in.
    std::string uname_mode = "scoped";
    bool overlayfs_enabled = true;
    bool magic_mount_enabled = true;
    // Global backend override: "auto" uses per-module modes (module_mode.json:
    // each module auto/overlay/magic/kasumi/none); "overlay"/"magic"/"none" force
    // every module. Per-module "auto" keeps Kagami's original priority:
    // OverlayFS -> Magic Mount -> none. Kasumi is opt-in through an explicit
    // per-module or global "kasumi" setting.
    std::string mount_backend = "auto";
    PolicyConfig policy;
};

std::string default_config_json();
bool write_default_config(const std::string& path, std::string& error);
bool parse_config_json(const std::string& json, Config& config, std::string& error);
bool read_config_file(const std::string& path, Config& config, std::string& error);
bool update_lkm_autoload_config(const std::string& path, bool enabled, std::string& error);
bool update_policy_config(const std::string& path, const PolicyConfig& policy, std::string& error);

} // namespace kagami
