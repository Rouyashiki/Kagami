#include "kagami/config.hpp"

#include "core/json_value.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace kagami {

static bool ensure_parent_dir(const std::string& path, std::string& error) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return true;
    }

    std::string current;
    std::stringstream parts(path.substr(0, slash));
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty()) {
            current = "/";
            continue;
        }
        if (current.size() > 1) {
            current += "/";
        }
        current += part;
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            error = "mkdir " + current + ": " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

std::string default_config_json() {
    return R"({
  "moduledir": "/data/adb/modules",
  "tempdir": "",
  "mountsource": "KSU",
  "work_dir": "/dev/kagami",
  "mirror_dir": "/dev/kagami_mirror",
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
  "kasumi_feature_config_version": 2,
  "enable_overlay_xattr_hide": false,
  "enable_mount_hide": false,
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
  "cmdline_value": "",
  "partitions": []
}
)";
}

bool write_default_config(const std::string& path, std::string& error) {
    if (!ensure_parent_dir(path, error)) {
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    out << default_config_json();
    return out.good();
}

static std::vector<std::string> json_string_array_or_empty(const JsonValue* value) {
    std::vector<std::string> out;
    if (!value || !value->is_array()) {
        return out;
    }
    for (const auto& item : value->array_value) {
        if (item.is_string()) {
            out.push_back(item.string_value);
        }
    }
    return out;
}

static std::vector<std::uint32_t> json_u32_array_or_empty(const JsonValue* value) {
    std::vector<std::uint32_t> out;
    if (!value || !value->is_array()) {
        return out;
    }
    for (const auto& item : value->array_value) {
        if (item.is_number() && item.number_value >= 0) {
            out.push_back(static_cast<std::uint32_t>(item.number_value));
        }
    }
    return out;
}

static bool json_bool_or(const JsonValue* root, const char* key, bool fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? value->bool_or(fallback) : fallback;
}

static std::string json_string_or(const JsonValue* root, const char* key, const std::string& fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? value->string_or(fallback) : fallback;
}

static int json_int_or(const JsonValue* root, const char* key, int fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? static_cast<int>(value->u32_or(static_cast<std::uint32_t>(fallback))) : fallback;
}

bool parse_config_json(const std::string& json, Config& config, std::string& error) {
    JsonValue root;
    if (!parse_json(json, root, error)) {
        return false;
    }
    if (!root.is_object()) {
        error = "config root must be a JSON object";
        return false;
    }

    config.module_dir = json_string_or(&root, "moduledir", config.module_dir);
    config.data_dir = json_string_or(&root, "data_dir", config.data_dir);
    config.log_file = json_string_or(&root, "logfile", config.log_file);
    config.mount_source = json_string_or(&root, "mountsource", config.mount_source);
    config.work_dir = json_string_or(&root, "work_dir", config.work_dir);
    // mirror_* is the unified storage configuration. Accept the former
    // overlay_* keys as a one-way compatibility migration; kasumi_* was
    // deliberately not retained because Kasumi no longer owns a separate base.
    config.mirror_dir = root.find("mirror_dir")
                            ? json_string_or(&root, "mirror_dir", config.mirror_dir)
                            : json_string_or(&root, "overlay_dir", config.mirror_dir);
    config.mirror_img = root.find("mirror_img")
                            ? json_string_or(&root, "mirror_img", config.mirror_img)
                            : json_string_or(&root, "overlay_img", config.mirror_img);
    config.mirror_img_size_mb = root.find("mirror_img_size_mb")
                                    ? json_int_or(&root, "mirror_img_size_mb", config.mirror_img_size_mb)
                                    : json_int_or(&root, "overlay_img_size_mb", config.mirror_img_size_mb);
    config.overlay_writable = json_bool_or(&root, "overlay_writable", config.overlay_writable);
    config.fs_type = json_string_or(&root, "fs_type", config.fs_type);
    config.debug = json_bool_or(&root, "debug", config.debug);
    config.verbose = json_bool_or(&root, "verbose", config.verbose);
    config.lkm_autoload = json_bool_or(&root, "lkm_autoload", config.lkm_autoload);
    config.kasumi_enabled = json_bool_or(&root, "kasumi_enabled", config.kasumi_enabled);
    config.enable_kernel_debug =
        json_bool_or(&root, "enable_kernel_debug", config.enable_kernel_debug);
    config.enable_stealth = json_bool_or(&root, "enable_stealth", config.enable_stealth);
    const bool has_split_kasumi_features =
        json_int_or(&root, "kasumi_feature_config_version", 0) >= 2 ||
        root.find("enable_overlay_xattr_hide") != nullptr ||
        root.find("enable_mount_hide") != nullptr ||
        root.find("enable_maps_spoof") != nullptr ||
        root.find("enable_statfs_spoof") != nullptr;
    if (has_split_kasumi_features) {
        config.enable_overlay_xattr_hide = json_bool_or(
            &root, "enable_overlay_xattr_hide", config.enable_overlay_xattr_hide);
        config.enable_mount_hide =
            json_bool_or(&root, "enable_mount_hide", config.enable_mount_hide);
        config.enable_maps_spoof =
            json_bool_or(&root, "enable_maps_spoof", config.enable_maps_spoof);
        config.enable_statfs_spoof =
            json_bool_or(&root, "enable_statfs_spoof", config.enable_statfs_spoof);
        config.enable_selinux_fix =
            json_bool_or(&root, "enable_selinux_fix", config.enable_selinux_fix);
    } else {
        // Legacy enable_hidexattr drove all four kernel projections, overlay
        // xattr hiding, and implicitly enabled stealth. Preserve that state in
        // memory; the WebUI writes only the split v2 fields on the next save.
        const bool legacy = json_bool_or(&root, "enable_hidexattr", false);
        config.enable_overlay_xattr_hide = legacy;
        config.enable_mount_hide = legacy;
        config.enable_maps_spoof = legacy;
        config.enable_statfs_spoof = legacy;
        config.enable_selinux_fix =
            legacy || json_bool_or(&root, "enable_selinux_fix", false);
        config.enable_stealth = config.enable_stealth || legacy;
    }
    config.overlayfs_enabled = json_bool_or(&root, "overlayfs_enabled", config.overlayfs_enabled);
    config.magic_mount_enabled = json_bool_or(&root, "magic_mount_enabled", config.magic_mount_enabled);
    config.mount_backend = json_string_or(&root, "mount_backend", config.mount_backend);
    config.partitions = json_string_array_or_empty(root.find("partitions"));

    const JsonValue* policy = root.find("policy");
    if (policy && policy->is_object()) {
        config.policy.owner = json_string_or(policy, "owner", config.policy.owner);
        config.policy.use_allow_uids = json_bool_or(policy, "use_allow_uids", config.policy.use_allow_uids);
        config.policy.use_deny_uids = json_bool_or(policy, "use_deny_uids", config.policy.use_deny_uids);
        config.policy.include_isolated_uids = json_bool_or(policy, "include_isolated_uids", config.policy.include_isolated_uids);
        config.policy.allow_uids = json_u32_array_or_empty(policy->find("allow_uids"));
        config.policy.deny_uids = json_u32_array_or_empty(policy->find("deny_uids"));
    }

    return true;
}

bool read_config_file(const std::string& path, Config& config, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_config_json(buffer.str(), config, error);
}

bool update_lkm_autoload_config(const std::string& path, bool enabled, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (!write_default_config(path, error)) {
            return false;
        }
        in.clear();
        in.open(path, std::ios::binary);
    }
    if (!in) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }

    std::ostringstream input;
    input << in.rdbuf();
    JsonValue root;
    if (!parse_json(input.str(), root, error) || !root.is_object()) {
        if (error.empty()) {
            error = "config root must be an object";
        }
        return false;
    }

    JsonValue value;
    value.type = JsonValue::Type::Bool;
    value.bool_value = enabled;
    root.object_value["lkm_autoload"] = value;

    if (!ensure_parent_dir(path, error)) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    out << stringify_json(root, 2) << "\n";
    if (!out.good()) {
        error = "write " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool update_policy_config(const std::string& path, const PolicyConfig& policy, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (!write_default_config(path, error)) {
            return false;
        }
        in.clear();
        in.open(path, std::ios::binary);
    }
    if (!in) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }

    std::ostringstream input;
    input << in.rdbuf();
    JsonValue root;
    if (!parse_json(input.str(), root, error) || !root.is_object()) {
        if (error.empty()) {
            error = "config root must be an object";
        }
        return false;
    }

    const auto bool_value = [](bool value) {
        JsonValue out;
        out.type = JsonValue::Type::Bool;
        out.bool_value = value;
        return out;
    };
    const auto string_value = [](const std::string& value) {
        JsonValue out;
        out.type = JsonValue::Type::String;
        out.string_value = value;
        return out;
    };
    const auto uid_array = [](const std::vector<std::uint32_t>& values) {
        JsonValue out;
        out.type = JsonValue::Type::Array;
        for (const auto value : values) {
            JsonValue number;
            number.type = JsonValue::Type::Number;
            number.number_value = value;
            out.array_value.push_back(number);
        }
        return out;
    };

    JsonValue policy_json;
    policy_json.type = JsonValue::Type::Object;
    policy_json.object_value["owner"] = string_value(policy.owner);
    policy_json.object_value["use_allow_uids"] = bool_value(policy.use_allow_uids);
    policy_json.object_value["use_deny_uids"] = bool_value(policy.use_deny_uids);
    policy_json.object_value["include_isolated_uids"] = bool_value(policy.include_isolated_uids);
    policy_json.object_value["allow_uids"] = uid_array(policy.allow_uids);
    policy_json.object_value["deny_uids"] = uid_array(policy.deny_uids);
    root.object_value["policy"] = policy_json;

    if (!ensure_parent_dir(path, error)) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    out << stringify_json(root, 2) << "\n";
    if (!out.good()) {
        error = "write " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace kagami
