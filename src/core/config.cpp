#include "kagami/config.hpp"
#include <algorithm>
#include <array>
#include <iterator>

#include "core/flat_json.hpp"
#include "core/log.hpp"
#include "core/prepared_input.hpp"
#include "core/runtime.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace kagami {

namespace {
bool ensure_parent_dir(const std::string& path, std::string& error) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return true;
    char parent[PATH_MAX];
    bounded::Writer out(parent, sizeof(parent));
    if (!out.write(std::string_view(path.data(), slash), '\0')) {
        error = "config parent path is too long";
        return false;
    }
    for (std::size_t i = 1; i <= slash; ++i) {
        if (parent[i] != '/' && parent[i] != '\0')
            continue;
        const char saved = parent[i];
        parent[i] = '\0';
        if (::mkdir(parent, 0755) != 0 && errno != EEXIST) {
            error = "mkdir " + std::string(parent) + ": " + std::strerror(errno);
            return false;
        }
        parent[i] = saved;
    }
    return true;
}
}  // namespace

bool write_file_atomic(const std::string& path, std::string_view data, std::string& error) {
    if (!ensure_parent_dir(path, error))
        return false;
    char temporary[PATH_MAX];
    bounded::Writer name(temporary, sizeof(temporary));
    if (!name.write(path, ".tmp.XXXXXX", '\0')) {
        error = "config temporary path is too long";
        return false;
    }
    const int fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) {
        error = "create " + path + ": " + std::strerror(errno);
        return false;
    }
    size_t offset = 0;
    bool ok = true;
    while (offset < data.size()) {
        const auto written = write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            if (written == 0)
                errno = EIO;
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok)
        ok = fsync(fd) == 0;
    int saved_errno = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (!ok || rename(temporary, path.c_str()) != 0) {
        error = "write " + path + ": " + std::strerror(ok ? errno : saved_errno);
        unlink(temporary);
        return false;
    }
    return true;
}

class ConfigFileLock {
public:
    ConfigFileLock() = default;
    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;
    ConfigFileLock(ConfigFileLock&&) = delete;
    ConfigFileLock& operator=(ConfigFileLock&&) = delete;
    ~ConfigFileLock() {
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }

    bool acquire(const std::string& path, std::string& error) {
        if (!ensure_parent_dir(path, error)) {
            return false;
        }
        char lock_path[PATH_MAX];
        bounded::Writer name(lock_path, sizeof(lock_path));
        if (!name.write(path, ".lock", '\0')) {
            error = "config lock path is too long";
            return false;
        }
        fd_ = ::open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) {
            error = "open " + std::string(lock_path) + ": " + std::strerror(errno);
            return false;
        }
        if (::flock(fd_, LOCK_EX) != 0) {
            error = "lock " + std::string(lock_path) + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
};

std::string_view default_config_json() noexcept {
    return R"({
  "moduledir": "/data/adb/modules",
  "tempdir": "",
  "mountsource": "KSU",
  "work_dir": "/dev/kagami",
  "mirror_dir": "",
  "mirror_img": "/data/adb/kagami/mirror.img",
  "mirror_img_size_mb": 2048,
  "overlay_writable": false,
  "logfile": "/data/adb/kagami/daemon.log",
  "debug": false,
  "verbose": false,
  "lkm_autoload": false,
  "fs_type": "auto",
  "disable_umount": false,
  "enable_nuke": true,
  "enable_kernel_debug": false,
  "enable_stealth": true,
  "kasumi_feature_config_version": 3,
  "enable_overlay_xattr_hide": false,
  "enable_mount_hide": false,
  "mount_hide_mode": "normal",
  "enable_maps_spoof": false,
  "enable_statfs_spoof": false,
  "enable_selinux_fix": false,
  "kasumi_enabled": true,
  "overlayfs_enabled": true,
  "magic_mount_enabled": true,
  "mount_backend": "auto",
  "policy": {
    "owner": "auto",
    "use_allow_uids": false,
    "use_deny_uids": false,
    "include_isolated_uids": false,
    "allow_uids": [],
    "deny_uids": []
  },
  "partitions": []
}
)";
}

bool write_default_config(const std::string& path, std::string& error) {
    ConfigFileLock lock;
    if (!lock.acquire(path, error)) {
        return false;
    }
    return write_file_atomic(path, default_config_json(), error);
}

namespace {
bool json_bool_or(json::Value root, const char* key, bool fallback) noexcept {
    return root.find(key).bool_or(fallback);
}
Argument json_string_or(json::Value root, const char* key, std::string_view fallback) noexcept {
    return Argument(root.find(key).string_or(fallback));
}
int json_int_or(json::Value root, const char* key, int fallback) noexcept {
    return static_cast<int>(root.find(key).u32_or(static_cast<std::uint32_t>(fallback)));
}
void apply_config_document(json::Value root, Config& config) {
    config.module_dir = json_string_or(root, "moduledir", config.module_dir);
    config.data_dir = json_string_or(root, "data_dir", config.data_dir);
    config.log_file = json_string_or(root, "logfile", config.log_file);
    config.mount_source = json_string_or(root, "mountsource", config.mount_source);
    config.work_dir = json_string_or(root, "work_dir", config.work_dir);
    // mirror_* is the unified storage configuration. Accept the former
    // overlay_* keys as a one-way compatibility migration; kasumi_* was
    // deliberately not retained because Kasumi no longer owns a separate base.
    config.mirror_dir = root.find("mirror_dir")
                            ? json_string_or(root, "mirror_dir", config.mirror_dir)
                            : json_string_or(root, "overlay_dir", config.mirror_dir);
    config.mirror_img = root.find("mirror_img")
                            ? json_string_or(root, "mirror_img", config.mirror_img)
                            : json_string_or(root, "overlay_img", config.mirror_img);
    config.mirror_img_size_mb =
        root.find("mirror_img_size_mb")
            ? json_int_or(root, "mirror_img_size_mb", config.mirror_img_size_mb)
            : json_int_or(root, "overlay_img_size_mb", config.mirror_img_size_mb);
    config.overlay_writable = json_bool_or(root, "overlay_writable", config.overlay_writable);
    config.fs_type = json_string_or(root, "fs_type", config.fs_type);
    config.debug = json_bool_or(root, "debug", config.debug);
    config.verbose = json_bool_or(root, "verbose", config.verbose);

#if defined(KAGAMI_EMBEDDED)
    config.lkm_autoload = false;
#else
    config.lkm_autoload = json_bool_or(root, "lkm_autoload", config.lkm_autoload);
#endif
    config.kasumi_enabled = json_bool_or(root, "kasumi_enabled", config.kasumi_enabled);
    config.enable_kernel_debug =
        json_bool_or(root, "enable_kernel_debug", config.enable_kernel_debug);
    config.enable_stealth = json_bool_or(root, "enable_stealth", config.enable_stealth);
    const bool has_split_kasumi_features =
        json_int_or(root, "kasumi_feature_config_version", 0) >= 2 ||
        root.find("enable_overlay_xattr_hide") || root.find("enable_mount_hide") ||
        root.find("enable_maps_spoof") || root.find("enable_statfs_spoof");
    if (has_split_kasumi_features) {
        config.enable_overlay_xattr_hide =
            json_bool_or(root, "enable_overlay_xattr_hide", config.enable_overlay_xattr_hide);
        config.enable_mount_hide =
            json_bool_or(root, "enable_mount_hide", config.enable_mount_hide);
        config.mount_hide_mode = json_string_or(root, "mount_hide_mode", config.mount_hide_mode);
        if (config.mount_hide_mode != "normal" && config.mount_hide_mode != "aggressive") {
            config.mount_hide_mode = "normal";
        }
        config.enable_maps_spoof =
            json_bool_or(root, "enable_maps_spoof", config.enable_maps_spoof);
        config.enable_statfs_spoof =
            json_bool_or(root, "enable_statfs_spoof", config.enable_statfs_spoof);
        config.enable_selinux_fix =
            json_bool_or(root, "enable_selinux_fix", config.enable_selinux_fix);
    } else {
        // Legacy enable_hidexattr drove all four kernel projections, overlay
        // xattr hiding, and implicitly enabled stealth. Preserve that state in
        // memory; the WebUI writes only the split v3 fields on the next save.
        const bool legacy = json_bool_or(root, "enable_hidexattr", false);
        config.enable_overlay_xattr_hide = legacy;
        config.enable_mount_hide = legacy;
        config.enable_maps_spoof = legacy;
        config.enable_statfs_spoof = legacy;
        config.enable_selinux_fix = legacy || json_bool_or(root, "enable_selinux_fix", false);
        config.enable_stealth = config.enable_stealth || legacy;
    }
    config.overlayfs_enabled = json_bool_or(root, "overlayfs_enabled", config.overlayfs_enabled);
    config.magic_mount_enabled =
        json_bool_or(root, "magic_mount_enabled", config.magic_mount_enabled);
    config.mount_backend = json_string_or(root, "mount_backend", config.mount_backend);

    const auto policy = root.find("policy");
    if (policy && policy.is_object()) {
        config.policy.owner = json_string_or(policy, "owner", config.policy.owner);
        config.policy.use_allow_uids =
            json_bool_or(policy, "use_allow_uids", config.policy.use_allow_uids);
        config.policy.use_deny_uids =
            json_bool_or(policy, "use_deny_uids", config.policy.use_deny_uids);
        config.policy.include_isolated_uids =
            json_bool_or(policy, "include_isolated_uids", config.policy.include_isolated_uids);
    }
}
}  // namespace

namespace {
bool prepare_document(PreparedInput& input, json::Document& document, std::string& error) {
    int code = 0;
    if (!document.prepare_document(input.view(), code) ||
        !document.parse(input.data(), input.size(), code)) {
        error = code == EINVAL ? "invalid config JSON" : std::strerror(code);
        return false;
    }
    if (!document.root().is_object()) {
        error = "config root must be a JSON object";
        return false;
    }
    return true;
}
bool prepare_config_input(const std::string& path, PreparedInput& input, std::string& error,
                          bool allow_missing) {
    error.clear();
    int code = 0;
    if (input.prepare_file(path.c_str(), code))
        return true;
    if (allow_missing && code == ENOENT && input.prepare_text(default_config_json(), code))
        return true;
    error = "open " + path + ": " + std::strerror(code);
    return false;
}
bool prepare_config_document(const std::string& path, PreparedInput& input,
                             json::Document& document, std::string& error) {
    return prepare_config_input(path, input, error, true) &&
           prepare_document(input, document, error);
}
template <typename Serialize>
bool save_document(const std::string& path, const json::ObjectWriter& document, std::string& error,
                   const Serialize& serialize) {
    bounded::Storage bytes;
    if (!bytes.prepare_storage(json::Document::maximum_bytes, json::Document::maximum_bytes)) {
        error = "cannot prepare config output";
        return false;
    }
    bounded::Writer output(bytes.data(), bytes.size());
    if (!document.write_each(output, serialize) || !output.append('\n')) {
        error = "config output exceeds capacity";
        return false;
    }
    return write_file_atomic(path, output.view(), error);
}
bool save_document(const std::string& path, const json::ObjectWriter& document,
                   std::string& error) {
    return save_document(
        path, document, error,
        [](bounded::Writer& out, std::string_view, json::Value value, const json::Edit* edit,
           unsigned indent) noexcept { return json::write_entry(out, value, edit, indent); });
}
bool replace(json::ObjectWriter& writer, json::Edit edit, std::string& error) {
    if (writer.replace(edit))
        return true;
    error = "config edit capacity exceeded";
    return false;
}
}  // namespace

bool PreparedConfig::prepare_values(json::Value root, PreparedInput&& input, std::string& error) {
    if (prepared_) {
        error = "configuration storage has already been prepared";
        return false;
    }
    const auto parts = root.find("partitions");
    const auto policy = root.find("policy");
    const auto allow = policy.find("allow_uids");
    const auto deny = policy.find("deny_uids");
    bounded::Array<Argument> partitions;
    bounded::Array<std::uint32_t> uids;
    if (!partitions.prepare_array(parts.is_array() ? parts.size() : 0,
                                  json::Document::maximum_nodes) ||
        !uids.prepare_array(
            (allow.is_array() ? allow.size() : 0) + (deny.is_array() ? deny.size() : 0),
            json::Document::maximum_nodes)) {
        error = "cannot prepare configuration arrays";
        return false;
    }
    if (parts.is_array()) {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto part = parts.child(i);
            if (part.is_string() && !partitions.append(Argument(part.string_or()))) {
                error = "configuration partition capacity exceeded";
                return false;
            }
        }
    }
    const auto append_uids = [&](json::Value values) {
        if (!values.is_array())
            return true;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto uid = values.child(i);
            if (uid.is_number() && uid.number_or(-1) >= 0 &&
                uid.number_or(-1) <= std::numeric_limits<std::uint32_t>::max() &&
                !uids.append(uid.u32_or(0)))
                return false;
        }
        return true;
    };
    if (!append_uids(allow)) {
        error = "configuration UID capacity exceeded";
        return false;
    }
    const auto allow_count = uids.size();
    if (!append_uids(deny)) {
        error = "configuration UID capacity exceeded";
        return false;
    }
    Config next = static_cast<const Config&>(*this);  // Scalar and borrowed-view values only.
    apply_config_document(root, next);
    next.partitions = {partitions.view().data(), partitions.size()};
    if (policy.is_object()) {
        next.policy.allow_uids = {uids.view().data(), allow_count};
        next.policy.deny_uids = {uids.size() ? uids.view().data() + allow_count : nullptr,
                                 uids.size() - allow_count};
    }
#if defined(KAGAMI_EMBEDDED)
    // These filesystem adapters remain at the preparation boundary. Every
    // resulting view is terminated and owned by this configuration operation.
    const auto module_path = runtime_modules_dir();
    const auto data_path = runtime_data_dir();
    const auto log_path = runtime_log_file();
    const auto image_path = data_path / "mirror.img";
    const auto capacity = module_path.native().size() + data_path.native().size() +
                          log_path.native().size() + image_path.native().size() + 4;
    bounded::Storage paths;
    if (!paths.prepare_storage(capacity, 4 * PATH_MAX)) {
        error = "cannot prepare embedded configuration paths";
        return false;
    }
    bounded::Writer destination(paths.data(), paths.size());
    const auto append = [&](const auto& path, Argument& target) {
        const auto offset = destination.view().size();
        if (!destination.write(path.native(), '\0'))
            return false;
        target = Argument(std::string_view(paths.data() + offset, path.native().size()));
        return true;
    };
    if (!append(module_path, next.module_dir) || !append(data_path, next.data_dir) ||
        !append(log_path, next.log_file) || !append(image_path, next.mirror_img)) {
        error = "embedded configuration path capacity exceeded";
        return false;
    }
    runtime_paths_ = std::move(paths);
#endif
    input_ = std::move(input);
    partitions_ = std::move(partitions);
    uids_ = std::move(uids);
    static_cast<Config&>(*this) = next;
    prepared_ = true;
    logging::set_debug_enabled(debug || verbose);
    return true;
}

bool parse_config_json(std::string_view text, PreparedConfig& config, std::string& error) {
    PreparedInput input;
    json::Document document;
    int code = 0;
    if (!input.prepare_text(text, code)) {
        error = std::strerror(code);
        return false;
    }
    if (!prepare_document(input, document, error))
        return false;
    return config.prepare_values(document.root(), std::move(input), error);
}

bool read_config_json(const std::string& path, PreparedInput& input, std::string& error,
                      bool allow_missing) {
    if (!prepare_config_input(path, input, error, allow_missing))
        return false;
    json::ValidationCursor cursor(input.data(), input.size());
    if (cursor.peek() != '{' || !cursor.skip(0) || !cursor.end()) {
        error = "invalid config JSON object";
        return false;
    }
    return true;
}

bool read_config_file(const std::string& path, PreparedConfig& config, std::string& error) {
    PreparedInput input;
    json::Document document;
    if (!prepare_config_input(path, input, error, false) ||
        !prepare_document(input, document, error))
        return false;
    return config.prepare_values(document.root(), std::move(input), error);
}

bool merge_config_json(const std::string& path, const std::string& updates, std::string& error) {
    PreparedInput patch_input;
    json::Document patch_document;
    int code = 0;
    if (!patch_input.prepare_text(updates, code)) {
        error = std::strerror(code);
        return false;
    }
    if (!prepare_document(patch_input, patch_document, error))
        return false;
    const auto patch = patch_document.root();
    if (patch.find("lkm_autoload")) {
        error = "use 'lkm set-autoload' to update lkm_autoload";
        return false;
    }
    ConfigFileLock lock;
    if (!lock.acquire(path, error))
        return false;
    PreparedInput input;
    json::Document document;
    if (!prepare_config_document(path, input, document, error))
        return false;
    const auto root = document.root();
    json::ObjectWriter merged(root, patch);
    const auto has_split_features = [](json::Value value) noexcept {
        return json_int_or(value, "kasumi_feature_config_version", 0) >= 2 ||
               value.find("enable_overlay_xattr_hide") || value.find("enable_mount_hide") ||
               value.find("mount_hide_mode") || value.find("enable_maps_spoof") ||
               value.find("enable_statfs_spoof");
    };
    if (!has_split_features(root) && has_split_features(patch)) {
        const bool legacy = json_bool_or(root, "enable_hidexattr", false);
        const auto migrate = [&](json::Edit edit) {
            return patch.find(edit.key) || replace(merged, edit, error);
        };
        if (!migrate({"kasumi_feature_config_version", {}, {}, json::Type::Number, 3}) ||
            !migrate({"mount_hide_mode", {}, "normal", json::Type::String}) ||
            !migrate({"enable_hidexattr", {}, {}, json::Type::Null, 0, false, true}))
            return false;
        for (const auto* key :
             {"enable_overlay_xattr_hide", "enable_mount_hide", "enable_maps_spoof",
              "enable_statfs_spoof", "enable_selinux_fix", "enable_stealth"}) {
            const std::string_view name(key);
            const bool enabled = legacy ||
                                 (name == "enable_selinux_fix" && json_bool_or(root, key, false)) ||
                                 (name == "enable_stealth" && json_bool_or(root, key, true));
            if (!migrate({name, {}, {}, json::Type::Bool, 0, enabled}))
                return false;
        }
    }
    for (const auto* key : {"kasumi_available", "tmpfs_xattr_supported"}) {
        if (patch.find(key)) {
            const auto original = root.find(key);
            if (!replace(merged, {key, original, {}, json::Type::Null, 0, false, !original}, error))
                return false;
        }
    }
    if (patch.find("mount_hide_mode") &&
        !replace(merged, {"kasumi_feature_config_version", {}, {}, json::Type::Number, 3}, error))
        return false;
    const bool saved = save_document(path, merged, error);
    if (saved)
        logging::set_debug_enabled(merged.find("debug").bool_or(false) ||
                                   merged.find("verbose").bool_or(false));
    return saved;
}

bool update_lkm_autoload_config(const std::string& path, bool enabled, std::string& error) {
    ConfigFileLock lock;
    if (!lock.acquire(path, error))
        return false;
    PreparedInput input;
    json::Document document;
    if (!prepare_config_document(path, input, document, error))
        return false;
    json::ObjectWriter merged(document.root());
    return replace(merged, {"lkm_autoload", {}, {}, json::Type::Bool, 0, enabled}, error) &&
           save_document(path, merged, error);
}

bool update_policy_config(const std::string& path, const PolicyConfig& policy, std::string& error) {
    ConfigFileLock lock;
    if (!lock.acquire(path, error))
        return false;
    PreparedInput input;
    json::Document document;
    if (!prepare_config_document(path, input, document, error))
        return false;
    json::ObjectWriter merged(document.root());
    if (!replace(merged, {"policy", {}}, error))
        return false;
    return save_document(
        path, merged, error,
        [&](bounded::Writer& out, std::string_view key, json::Value value, const json::Edit* edit,
            unsigned indent) noexcept {
            if (key != "policy")
                return json::write_entry(out, value, edit, indent);
            const auto uids = [&](const auto& values) noexcept {
                if (!out.append('['))
                    return false;
                bool first = true;
                for (const auto uid : values) {
                    if ((!first && !out.append(',')) || !out.append("\n      ") || !out.append(uid))
                        return false;
                    first = false;
                }
                return (values.empty() || out.append("\n    ")) && out.append(']');
            };
            return out.append("{\n    \"allow_uids\": ") && uids(policy.allow_uids) &&
                   out.append(",\n    \"deny_uids\": ") && uids(policy.deny_uids) &&
                   out.write(",\n    \"include_isolated_uids\": ",
                             policy.include_isolated_uids ? "true" : "false",
                             ",\n    \"owner\": ") &&
                   json::write_quoted(out, policy.owner) &&
                   out.write(
                       ",\n    \"use_allow_uids\": ", policy.use_allow_uids ? "true" : "false",
                       ",\n    \"use_deny_uids\": ", policy.use_deny_uids ? "true" : "false",
                       "\n  }");
        });
}

}  // namespace kagami
