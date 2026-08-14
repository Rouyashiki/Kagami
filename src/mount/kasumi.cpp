#include "mount/kasumi.hpp"

#include "core/json_value.hpp"
#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"
#include "kagami/kasumi_uapi.h"
#include "mount/mount_fs.hpp"
#include "mount/storage.hpp"

#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace kagami::mount::kasumi {

namespace fs = std::filesystem;
using fsutil::mlog;

namespace {

fs::path active_file() { return runtime_data_dir() / "run" / "kasumi_active"; }
fs::path mapping_plan_file() {
    return runtime_data_dir() / "run" / "kasumi_mapping_plan";
}

std::string current_boot_id() {
    std::ifstream in("/proc/sys/kernel/random/boot_id");
    std::string value;
    std::getline(in, value);
    return value;
}

bool marker_matches_current_boot(const fs::path& path) {
    const std::string boot_id = current_boot_id();
    if (boot_id.empty()) {
        return false;
    }
    std::ifstream in(path);
    std::string stored_boot_id;
    return static_cast<bool>(std::getline(in, stored_boot_id)) &&
           stored_boot_id == boot_id;
}

bool write_boot_marker(const fs::path& path, const std::string& detail = {}) {
    const std::string boot_id = current_boot_id();
    if (boot_id.empty()) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    fs::path temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << boot_id << "\n";
    if (!detail.empty()) {
        out << detail << "\n";
    }
    out.flush();
    const bool write_ok = out.good();
    out.close();
    if (!write_ok || out.fail()) {
        fs::remove(temporary, ec);
        return false;
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool path_matches_rule(const std::string& path, const std::string& prefix) {
    if (path == prefix) {
        return true;
    }
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return prefix.back() == '/' || path[prefix.size()] == '/';
}

std::string effective_mode(const std::string& path, const std::vector<ModuleRule>& rules) {
    std::string mode = "kasumi";
    std::size_t longest = 0;
    for (const auto& rule : rules) {
        if (rule.path.empty() || rule.path.front() != '/') {
            continue;
        }
        if (path_matches_rule(path, rule.path) && rule.path.size() >= longest) {
            longest = rule.path.size();
            mode = rule.mode;
        }
    }
    return mode;
}

// Resolve symlinked partition paths (e.g. /system/vendor -> /vendor) without
// discarding a non-existent leaf that a module adds.
std::string resolve_virtual_path(const std::string& value) {
    fs::path path(value);
    if (!path.has_parent_path()) {
        return value;
    }
    fs::path current = path.parent_path();
    std::vector<fs::path> suffix;
    std::error_code ec;
    while (!current.empty() && current != "/" && !fs::exists(current, ec)) {
        if (ec) {
            return value;
        }
        suffix.push_back(current.filename());
        current = current.parent_path();
    }
    if (fs::exists(current, ec) && !ec) {
        current = fs::canonical(current, ec);
        if (ec) {
            return value;
        }
    }
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) {
        current /= *it;
    }
    current /= path.filename();
    return current.string();
}

bool is_sub_partition(const std::string& name, const std::vector<std::string>& parts) {
    return name != "system" &&
           std::find(parts.begin(), parts.end(), name) != parts.end();
}

void relabel_tree(const std::string& node, const std::string& target, const std::string& parent_ctx) {
    std::string context;
    if (!fsutil::get_context(target, context)) {
        context = parent_ctx;
    }
    if (!context.empty()) {
        fsutil::set_context(node, context);
    }
    struct stat st = {};
    if (lstat(node.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;
    }
    DIR* dir = opendir(node.c_str());
    if (!dir) {
        return;
    }
    while (dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        relabel_tree(node + "/" + entry->d_name, target + "/" + entry->d_name, context);
    }
    closedir(dir);
}

void relabel_partition(const std::string& destination, const std::string& partition,
                       const std::vector<std::string>& parts) {
    if (partition != "system") {
        relabel_tree(destination, fsutil::partition_mount_point(partition), "");
        return;
    }
    DIR* dir = opendir(destination.c_str());
    if (!dir) {
        return;
    }
    std::error_code ec;
    while (dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const std::string child = destination + "/" + entry->d_name;
        const std::string target = is_sub_partition(entry->d_name, parts) && fs::is_directory(child, ec)
                                       ? "/" + std::string(entry->d_name)
                                       : "/system/" + std::string(entry->d_name);
        relabel_tree(child, target, "");
    }
    closedir(dir);
}

bool mirror_modules(const std::vector<ModuleEntry>& modules, const storage::Handle& storage,
                    const std::vector<std::string>& parts) {
    if (storage.mode == storage::Mode::Erofs) {
        mlog("kasumi: erofs mirror is read-only and cannot be refreshed at boot");
        return false;
    }
    bool ok = true;
    for (const auto& module : modules) {
        const fs::path module_destination = fs::path(storage.content_dir) / module.id;
        fsutil::rm_rf(module_destination.string());
        for (const auto& partition : parts) {
            const fs::path source = module.path / partition;
            std::error_code ec;
            if (!fs::is_directory(source, ec) || fs::is_empty(source, ec)) {
                continue;
            }
            const fs::path destination = module_destination / partition;
            fs::create_directories(destination, ec);
            if (ec || !fsutil::copy_tree(source.string(), destination.string())) {
                mlog("kasumi: failed to mirror " + source.string());
                ok = false;
                continue;
            }
            relabel_partition(destination.string(), partition, parts);
        }
    }
    return ok;
}

std::vector<std::string> user_hide_rules() {
    std::vector<std::string> out;
    std::ifstream in(runtime_data_dir() / "user_hide_rules.json");
    if (!in) {
        return out;
    }
    std::stringstream data;
    data << in.rdbuf();
    JsonValue root;
    std::string error;
    if (!parse_json(data.str(), root, error) || !root.is_array()) {
        return out;
    }
    for (const auto& item : root.array_value) {
        if (item.is_string() && !item.string_value.empty() && item.string_value.front() == '/') {
            out.push_back(item.string_value);
        }
    }
    return out;
}

struct RuleBatch {
    std::vector<std::pair<std::string, std::string>> add;
    std::vector<std::pair<std::string, std::string>> merge;
    std::set<std::string> hide;
};

void compile_tree(const fs::path& source_root, const std::string& virtual_root,
                  const std::vector<ModuleRule>& rules, RuleBatch& batch) {
    std::error_code ec;
    if (!fs::is_directory(source_root, ec)) {
        return;
    }
    auto it = fs::recursive_directory_iterator(source_root, ec);
    const auto end = fs::recursive_directory_iterator();
    for (; it != end && !ec; it.increment(ec)) {
        const fs::path source = it->path();
        const fs::path relative = fs::relative(source, source_root, ec);
        if (ec) {
            break;
        }
        const std::string virtual_path = resolve_virtual_path((fs::path(virtual_root) / relative).string());
        std::string mode = effective_mode(virtual_path, rules);
        if (mode == "hide") {
            batch.hide.insert(virtual_path);
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (mode == "none") {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (mode != "kasumi" && mode != "auto") {
            // A module selected for Kasumi cannot safely create an OverlayFS or
            // Magic subtree after boot. Keep the path visible through Kasumi
            // rather than silently dropping it; mixed-backend planning remains
            // a boot-time concern for a later planner pass.
            mode = "kasumi";
        }

        struct stat st = {};
        if (lstat(source.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISCHR(st.st_mode) && major(st.st_rdev) == 0 && minor(st.st_rdev) == 0) {
            batch.hide.insert(virtual_path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            std::error_code target_ec;
            if (fs::is_directory(virtual_path, target_ec) && !target_ec) {
                batch.merge.emplace_back(virtual_path, source.string());
                it.disable_recursion_pending();
            }
            continue;
        }
        if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            if (S_ISLNK(st.st_mode)) {
                std::error_code target_ec;
                if (fs::is_directory(virtual_path, target_ec) && !target_ec) {
                    mlog("kasumi: refusing to replace directory with symlink " + virtual_path);
                    continue;
                }
            }
            batch.add.emplace_back(virtual_path, source.string());
        }
    }
    if (ec) {
        mlog("kasumi: scan failed for " + source_root.string() + ": " + ec.message());
    }
}

bool configure_mirror(const std::string& mirror) {
    if (!::kagami::kasumi::set_mirror_path(mirror)) {
        mlog("kasumi: failed to set mirror path " + mirror);
        return false;
    }
    return true;
}

bool disable_kernel_features(std::string* error = nullptr) {
    bool ok = true;
    const auto disable = [&](bool result, const char* name) {
        if (!result) {
            ok = false;
            if (error && error->empty()) {
                *error = std::string("failed to disable Kasumi ") + name;
            }
        }
    };
    disable(::kagami::kasumi::set_debug(false), "kernel debug");
    disable(::kagami::kasumi::set_stealth(false), "stealth");
    disable(::kagami::kasumi::set_mount_hide(false), "mount hide");
    disable(::kagami::kasumi::set_maps_spoof(false), "maps spoof");
    disable(::kagami::kasumi::set_statfs_spoof(false), "statfs spoof");
    disable(::kagami::kasumi::set_selinux_guard(false), "SELinux guard");
    disable(::kagami::kasumi::set_cmdline(""), "cmdline spoof");
    return ok;
}

} // namespace

bool apply_policy_config(const PolicyConfig& policy, std::string& error) {
    ::kagami::kasumi::PolicyOwner owner = ::kagami::kasumi::PolicyOwner::Auto;
    if (policy.owner == "auto") {
        owner = ::kagami::kasumi::PolicyOwner::Auto;
    } else if (policy.owner == "kernelsu" || policy.owner == "ksu") {
        owner = ::kagami::kasumi::PolicyOwner::KernelSU;
    } else if (policy.owner == "apatch") {
        owner = ::kagami::kasumi::PolicyOwner::APatch;
    } else if (policy.owner == "manual") {
        owner = ::kagami::kasumi::PolicyOwner::Manual;
    } else if (policy.owner == "disabled" || policy.owner == "off") {
        owner = ::kagami::kasumi::PolicyOwner::Disabled;
    } else {
        error = policy.owner == "magisk"
                    ? "Magisk has no Kasumi API 17 policy provider; use manual"
                    : "invalid policy owner: " + policy.owner;
        return false;
    }

    std::uint32_t flags = 0;
    if (policy.use_allow_uids || !policy.allow_uids.empty() ||
        policy.include_isolated_uids ||
        owner == ::kagami::kasumi::PolicyOwner::Manual) {
        flags |= KSM_POLICY_FLAG_USE_ALLOW_UIDS;
    }
    if (policy.use_deny_uids || !policy.deny_uids.empty()) {
        flags |= KSM_POLICY_FLAG_USE_DENY_UIDS;
    }
    if (policy.include_isolated_uids) {
        flags |= KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS;
    }

    // Policy/provider changes are only legal while the kernel gate is down.
    if (!::kagami::kasumi::set_enabled(false)) {
        error = "failed to disable Kasumi before replacing policy";
        return false;
    }
    if (!::kagami::kasumi::replace_policy(owner, flags, policy.allow_uids,
                                           policy.deny_uids)) {
        error = "failed to atomically replace Kasumi policy";
        return false;
    }
    return true;
}

bool apply_feature_config(const Config& config, std::string& error) {
    bool ok = true;
    const auto apply = [&](bool result, const char* name) {
        if (!result) {
            if (error.empty()) {
                error = std::string("failed to set Kasumi ") + name;
            }
            ok = false;
        }
    };

    apply(::kagami::kasumi::set_debug(config.enable_kernel_debug), "kernel debug");
    apply(::kagami::kasumi::set_stealth(config.enable_stealth), "stealth");
    apply(::kagami::kasumi::set_mount_hide(config.enable_mount_hide), "mount hide");
    apply(::kagami::kasumi::set_maps_spoof(config.enable_maps_spoof), "maps spoof");
    apply(::kagami::kasumi::set_statfs_spoof(config.enable_statfs_spoof),
          "statfs spoof");
    apply(::kagami::kasumi::set_selinux_guard(config.enable_selinux_fix),
          "SELinux guard");
    apply(::kagami::kasumi::set_cmdline(config.cmdline_value), "cmdline spoof");
    if (!ok) {
        (void)disable_kernel_features();
    }
    return ok;
}

bool disable_control_state(std::string& error) {
    bool ok = true;
    if (!::kagami::kasumi::set_enabled(false)) {
        error = "failed to disable Kasumi";
        ok = false;
    }
    if (!disable_kernel_features(&error)) {
        ok = false;
    }
    return ok;
}

bool restore_persisted_hide_rules(std::string& error) {
    bool ok = true;
    for (const auto& path : user_hide_rules()) {
        if (!::kagami::kasumi::hide_path(path)) {
            ok = false;
        }
    }
    if (!ok) {
        error = "failed to restore one or more persistent hide rules";
    }
    return ok;
}

bool deactivate(std::string& error) {
    bool ok = disable_control_state(error);
    if (!::kagami::kasumi::clear_rules()) {
        if (error.empty()) {
            error = "failed to clear Kasumi rules";
        }
        ok = false;
    }
    invalidate_active_state();
    return ok;
}

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                   const ModuleRuleMap& rules) {
    if (!::kagami::kasumi::is_available()) {
        mlog("kasumi: backend requested but protocol is unavailable");
        return false;
    }
    const auto persisted_hide_rules = user_hide_rules();
    if (modules.empty()) {
        std::string error;
        const bool ok = restore_persisted_hide_rules(error);
        if (ok) {
            if (!write_boot_marker(active_file())) {
                mlog("kasumi: failed to record restored runtime state");
            }
            mlog("kasumi: installed add=0 merge=0 hide=" +
                 std::to_string(persisted_hide_rules.size()));
        } else {
            std::error_code ec;
            fs::remove(active_file(), ec);
            mlog("kasumi: one or more persistent hide rules failed");
        }
        return ok;
    }

    Config mirror_config = config;
    // Kasumi shares the same module mirror as OverlayFS. It never needs the
    // optional OverlayFS upper/work layer by itself.
    mirror_config.overlay_writable = false;
    storage::Handle mirror = storage::setup(mirror_config);
    if (!mirror.ok) {
        mlog("kasumi: mirror storage setup failed");
        return false;
    }
    const std::vector<std::string>& partitions =
        config.partitions.empty() ? fsutil::kManagedPartitions : config.partitions;
    if (!mirror_modules(modules, mirror, partitions) ||
        !configure_mirror(mirror.content_dir)) {
        return false;
    }

    RuleBatch batch;
    for (const auto& module : modules) {
        const auto rule_it = rules.find(module.id);
        const std::vector<ModuleRule> empty_rules;
        const auto& module_rules = rule_it == rules.end() ? empty_rules : rule_it->second;
        for (const auto& rule : module_rules) {
            if (rule.mode == "hide" && !rule.path.empty() && rule.path.front() == '/') {
                batch.hide.insert(resolve_virtual_path(rule.path));
            }
        }
    }

    // Kernel rules use last-write-wins semantics. Enumerated modules are sorted
    // by id, so emit them backwards to retain the same deterministic priority as
    // YukiSU's hymo planner.
    for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
        const auto rule_it = rules.find(it->id);
        const std::vector<ModuleRule> empty_rules;
        const auto& module_rules = rule_it == rules.end() ? empty_rules : rule_it->second;
        const fs::path source = fs::path(mirror.content_dir) / it->id;
        for (const auto& partition : partitions) {
            compile_tree(source / partition, "/" + partition, module_rules, batch);
        }
    }

    bool ok = true;
    for (const auto& rule : batch.add) {
        ok = ::kagami::kasumi::add_rule(rule.first, rule.second, 0) && ok;
    }
    for (const auto& rule : batch.merge) {
        ok = ::kagami::kasumi::add_merge_rule(rule.first, rule.second) && ok;
    }
    for (const auto& path : persisted_hide_rules) {
        batch.hide.insert(path);
    }
    for (const auto& path : batch.hide) {
        ok = ::kagami::kasumi::hide_path(path) && ok;
    }
    std::error_code ec;
    fs::create_directories(active_file().parent_path(), ec);
    if (ok && !write_boot_marker(active_file(), mirror.content_dir)) {
        ok = false;
        mlog("kasumi: failed to record restored runtime state");
    }
    if (ok) {
        mlog("kasumi: installed add=" + std::to_string(batch.add.size()) +
             " merge=" + std::to_string(batch.merge.size()) +
             " hide=" + std::to_string(batch.hide.size()));
    } else {
        fs::remove(active_file(), ec);
        mlog("kasumi: one or more rule operations failed");
    }
    return ok;
}

bool unmount_all(const Config& config) {
    (void)config; // shared storage is released centrally after OverlayFS too.
    bool ok = true;
    if (::kagami::kasumi::module_loaded() || ::kagami::kasumi::is_available()) {
        std::string error;
        ok = deactivate(error);
        if (!ok) {
            mlog("kasumi: " + error);
        }
    }
    std::error_code ec;
    fs::remove(active_file(), ec);
    clear_replayable_mappings();
    return ok;
}

bool is_active() {
    return marker_matches_current_boot(active_file());
}

void invalidate_active_state() {
    std::error_code ec;
    fs::remove(active_file(), ec);
}

std::vector<std::string> replayable_module_ids() {
    std::vector<std::string> ids;
    if (!marker_matches_current_boot(mapping_plan_file())) {
        return ids;
    }
    std::ifstream in(mapping_plan_file());
    std::string line;
    std::getline(in, line); // boot id
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ids.push_back(line);
        }
    }
    return ids;
}

bool has_replayable_mappings() {
    return !replayable_module_ids().empty();
}

bool record_replayable_mappings(const std::vector<ModuleEntry>& modules) {
    if (modules.empty()) {
        clear_replayable_mappings();
        return true;
    }
    std::ostringstream ids;
    for (const auto& module : modules) {
        ids << module.id << "\n";
    }
    if (write_boot_marker(mapping_plan_file(), ids.str())) {
        return true;
    }
    clear_replayable_mappings();
    return false;
}

void clear_replayable_mappings() {
    std::error_code ec;
    fs::remove(mapping_plan_file(), ec);
}

} // namespace kagami::mount::kasumi
