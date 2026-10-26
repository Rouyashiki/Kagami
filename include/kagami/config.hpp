#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include "core/arguments.hpp"
#include "core/prepared_input.hpp"
#include "kagami/fixed_array.hpp"

namespace kagami {

class PreparedInput;

struct PolicyConfig {
    Argument owner = "auto";
    bool use_allow_uids = false;
    bool use_deny_uids = false;
    bool include_isolated_uids = false;
    bounded::Span<std::uint32_t> allow_uids;
    bounded::Span<std::uint32_t> deny_uids;
};

struct Config {
    Argument module_dir = "/data/adb/modules";
    Argument data_dir = "/data/adb/kagami";
    Argument log_file = "/data/adb/kagami/daemon.log";
    Argument mount_source = "KSU";
    Argument fs_type = "auto";
    // Work tmpfs for magic mount. Module files are copied here (NOT bind-mounted
    // from /data onto system). /dev is a writable tmpfs everywhere; /debug_ramdisk
    // is read-only on some devices. Configurable; avoid /mnt.
    Argument work_dir = "/dev/kagami";
    // OverlayFS module mirror. It MUST NOT live under /data: an overlay lowerdir
    // exposes the source's SELinux context and data_file is wrong for the target
    // partition, so content is relabeled into this mirror. Kasumi does NOT use it
    // (its vnode clones the source SID and redirects straight to /data/adb/modules);
    // Magic Mount keeps its own work tmpfs. Empty (default, and the retired
    // "/dev/kagami_mirror") => a per-boot random /mnt/<rand> mountpoint with no
    // fixed signature; any other explicit path overrides. fs_type selects the
    // storage mode ("auto" tries tmpfs then ext4; "tmpfs"/"ext4"/"erofs" force
    // one). The ext4/erofs backing image persists on /data.
    Argument mirror_dir = "";
    Argument mirror_img = "/data/adb/kagami/mirror.img";
    int mirror_img_size_mb = 2048;
    // Read-only overlay by default (lowerdirs only), matching meta-overlayfs. A
    // writable upper/work layer is opt-in: it breaks early-boot vendor/system init
    // on some devices (SELinux-denied writes through the overlay -> RIL fails).
    bool overlay_writable = false;
    Arguments partitions;
    bool debug = false;
    bool verbose = false;
    // LKM loading is opt-in. A packaged Kasumi .ko must never be inserted just
    // because Kagami itself is enabled; explicit Kasumi users can enable this
    // boot-time action, or load the LKM manually from the control plane.
    bool lkm_autoload = false;
    bool kasumi_enabled = true;
    bool enable_kernel_debug = false;
    bool enable_stealth = true;
    bool enable_overlay_xattr_hide = false;
    bool enable_mount_hide = false;
    bool enable_maps_spoof = false;
    bool enable_statfs_spoof = false;
    bool enable_selinux_fix = false;
    bool overlayfs_enabled = true;
    bool magic_mount_enabled = true;
    // Global backend override: "auto" uses per-module modes (module_mode.json:
    // each module auto/overlay/magic/kasumi/none); "overlay"/"magic"/"none" force
    // every module. Per-module "auto" keeps Kagami's original priority:
    // OverlayFS -> Magic Mount -> none. Kasumi is opt-in through an explicit
    // per-module or global "kasumi" setting.
    Argument mount_backend = "auto";
    PolicyConfig policy;
};

namespace json {
class Value;
}

// The Config base is only a borrowed view. This unique owner keeps all decoded
// strings and fixed arrays alive for the operation. Preparation happens once;
// reloading creates another owner and publishes its view only after success.
class PreparedConfig : public Config {
public:
    PreparedConfig() noexcept = default;
    PreparedConfig(const PreparedConfig&) = delete;
    PreparedConfig& operator=(const PreparedConfig&) = delete;
    PreparedConfig(PreparedConfig&&) noexcept = default;
    PreparedConfig& operator=(PreparedConfig&&) noexcept = default;
    ~PreparedConfig() = default;
    [[nodiscard]] bool prepare_values(json::Value root, PreparedInput&& input, std::string& error);

private:
    PreparedInput input_;
    bounded::Array<Argument> partitions_;
    bounded::Array<std::uint32_t> uids_;
    bounded::Storage runtime_paths_;
    bool prepared_ = false;
};

std::string_view default_config_json() noexcept;
bool write_default_config(const std::string& path, std::string& error);
bool parse_config_json(std::string_view text, PreparedConfig& config, std::string& error);
// Preserve unknown fields and reject unreadable or malformed saved documents.
// Defaults are allowed only when the file does not exist.
bool read_config_json(const std::string& path, PreparedInput& input, std::string& error,
                      bool allow_missing = false);
bool read_config_file(const std::string& path, PreparedConfig& config, std::string& error);
bool merge_config_json(const std::string& path, const std::string& updates, std::string& error);
bool update_lkm_autoload_config(const std::string& path, bool enabled, std::string& error);
bool update_policy_config(const std::string& path, const PolicyConfig& policy, std::string& error);

}  // namespace kagami
