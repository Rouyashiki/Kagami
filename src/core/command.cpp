#include "core/command.hpp"

#include "core/daemon.hpp"
#include "core/json.hpp"
#include "core/lkm.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"
#include "kagami/kasumi_uapi_compat.hpp"
#include "mount/backend.hpp"
#include "mount/kasumi.hpp"
#include "mount/magic_mount.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace kagami {

namespace fs = std::filesystem;

static const std::vector<std::string> kBuiltinPartitions = {
    "system", "vendor", "product", "system_ext", "odm", "oem",
};

static fs::path data_dir() {
    return runtime_data_dir();
}

static fs::path modules_dir() {
    return runtime_modules_dir();
}

static fs::path config_file() {
    return runtime_config_file();
}

static fs::path user_hide_rules_file() {
    return data_dir() / "user_hide_rules.json";
}

static void print_usage() {
    std::cout
        << "Kagami " << KAGAMI_VERSION << "\n"
        << "usage:\n"
        << "  kagamid version\n"
        << "  kagamid config show\n"
        << "  kagamid config gen [-o PATH]\n"
        << "  kagamid config merge-json JSON\n"
        << "  kagamid config apply [PATH]\n"
        << "  kagamid config policy set <owner> <allow-csv|-> <deny-csv|-> <isolated:on|off>\n"
        << "  kagamid daemon status|serve|call|ping|stop\n"
        << "  kagamid api system|storage|lkm|kasumi|features|hooks|policy|backends|meta\n"
        << "  kagamid module list|add|delete|set-mode|add-rule|remove-rule|hot-mount|hot-unmount|check-conflicts|mount-all|unmount|normalize\n"
        << "  kagamid recovery status|boot-completed|reset\n"
        << "  kagamid kasumi version|list|enable|disable|clear|set-mirror|fix-mounts|hide-overlay-xattrs|maps|policy\n"
        << "  kagamid lkm load|unload|status|autoload|set-autoload|set-kmi|clear-kmi\n";
}

static std::string arg_or_default(const std::vector<std::string>& args, std::size_t index, const std::string& fallback) {
    return index < args.size() ? args[index] : fallback;
}

// True once the device has finished booting. Magic mount must only run during
// the post-fs-data boot stage (metamount.sh); mounting over the live system
// afterwards breaks mount namespaces, so the CLI refuses mount-all post-boot.
static bool system_boot_completed() {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("sys.boot_completed", value) > 0) {
        return std::string(value) == "1";
    }
#endif
    return false;
}

static void print_string_array(const std::vector<std::string>& values) {
    std::cout << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << json_quote(values[i]);
    }
    std::cout << "]";
}

static void print_u32_array(const std::vector<std::uint32_t>& values) {
    std::cout << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << values[i];
    }
    std::cout << "]";
}

static std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (std::getline(in, line)) {
        return line;
    }
    return "";
}

static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "";
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

static bool write_file(const fs::path& path, const std::string& content) {
    try {
        fs::create_directories(path.parent_path());
    } catch (...) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << content;
    return out.good();
}

static bool is_builtin_partition(const std::string& name) {
    return std::find(kBuiltinPartitions.begin(), kBuiltinPartitions.end(), name) != kBuiltinPartitions.end();
}

static bool valid_module_id(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

static bool valid_module_mode(const std::string& mode) {
    return mode == "auto" || mode == "kasumi" || mode == "overlay" || mode == "magic" ||
           mode == "none" || mode == "hide";
}

static bool parse_unsigned_long(const std::string& value, unsigned long& out) {
    try {
        std::size_t parsed = 0;
        const unsigned long result = std::stoul(value, &parsed, 0);
        if (parsed != value.size()) {
            return false;
        }
        out = result;
        return true;
    } catch (...) {
        return false;
    }
}

static bool module_has_partition_content(const fs::path& module_path, const std::string& partition) {
    const fs::path root = module_path / partition;
    if (!fs::is_directory(root)) {
        return false;
    }
    try {
        return fs::recursive_directory_iterator(root) != fs::recursive_directory_iterator();
    } catch (...) {
        return false;
    }
}

static std::vector<std::string> parse_json_string_array(const std::string& json) {
    std::vector<std::string> values;
    bool in_string = false;
    bool escape = false;
    std::string value;

    for (char c : json) {
        if (!in_string) {
            if (c == '"') {
                in_string = true;
                value.clear();
            }
            continue;
        }
        if (escape) {
            value.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            values.push_back(value);
            in_string = false;
            continue;
        }
        value.push_back(c);
    }
    return values;
}

static std::vector<std::string> load_user_hide_rules() {
    return parse_json_string_array(read_file(user_hide_rules_file()));
}

static bool save_user_hide_rules(const std::vector<std::string>& rules) {
    std::ostringstream out;
    out << "[\n";
    for (std::size_t i = 0; i < rules.size(); ++i) {
        out << "  " << json_quote(rules[i]);
        if (i + 1 < rules.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "]\n";
    return write_file(user_hide_rules_file(), out.str());
}

static std::map<std::string, std::string> read_prop_file(const fs::path& path) {
    std::map<std::string, std::string> props;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        props[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return props;
}

static std::string kernel_release() {
    const std::string version = read_first_line("/proc/version");
    const std::string marker = "Linux version ";
    const auto start = version.find(marker);
    if (start == std::string::npos) {
        return version.empty() ? "Unknown" : version;
    }
    const auto value_start = start + marker.size();
    const auto value_end = version.find(' ', value_start);
    return version.substr(value_start, value_end - value_start);
}

static std::string selinux_status() {
    const std::string enforce = read_first_line("/sys/fs/selinux/enforce");
    if (enforce == "0") {
        return "Permissive";
    }
    if (enforce == "1") {
        return "Enforcing";
    }
    return "Unknown";
}

static std::string format_bytes(unsigned long long bytes) {
    const char* units[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0) {
        out << static_cast<unsigned long long>(value) << units[unit];
    } else {
        out.setf(std::ios::fixed);
        out.precision(value < 10.0 ? 1 : 0);
        out << value << units[unit];
    }
    return out.str();
}

static std::string partition_mount_point(const std::string& partition) {
    if (partition == "system") {
        return "/system";
    }
    return "/" + partition;
}

static bool path_is_read_only_mount(const std::string& mount_point) {
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream parts(line);
        std::string dev;
        std::string mp;
        std::string fs_type;
        std::string options;
        parts >> dev >> mp >> fs_type >> options;
        if (mp == mount_point) {
            return options.find("ro") != std::string::npos;
        }
    }
    return false;
}

struct MountEntry {
    std::string mount_point;
    std::string fstype;
    std::string source;
};

// Parse /proc/self/mountinfo into (mount_point, fstype, source) tuples.
static std::vector<MountEntry> read_mountinfo() {
    std::vector<MountEntry> out;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        std::istringstream pre(line.substr(0, sep));
        std::vector<std::string> f;
        std::string tok;
        while (pre >> tok) {
            f.push_back(tok);
        }
        if (f.size() < 5) {
            continue;
        }
        std::istringstream post(line.substr(sep + 3));
        MountEntry e;
        e.mount_point = f[4];
        post >> e.fstype >> e.source;
        out.push_back(e);
    }
    return out;
}

static Config current_config() {
    Config config;
    std::string error;
    read_config_file(config_file().string(), config, error); // struct defaults on miss
    return config;
}

static int print_storage_json() {
    const Config config = current_config();
    const std::string mirror_base = config.mirror_dir;

    // Report the active backend's storage base (tmpfs/ext4/erofs): the shared
    // mirror root if mounted, else the Magic Mount work tmpfs, else fall back
    // to the host /data filesystem. Loop-backed ext4/EROFS uses a loop-device
    // source, so the mirror is identified by mountpoint rather than source.
    std::string mode = "host";
    fs::path target = data_dir();
    for (const auto& m : read_mountinfo()) {
        if (m.mount_point == mirror_base ||
            (m.source == config.mount_source && m.mount_point == config.work_dir)) {
            mode = m.fstype;
            target = m.mount_point;
            break;
        }
    }

    struct statvfs st = {};
    if (statvfs(target.c_str(), &st) != 0) {
        std::cout << "{\"error\":\"not mounted\"}\n";
        return 0;
    }

    const auto total = static_cast<unsigned long long>(st.f_blocks) * st.f_frsize;
    const auto avail = static_cast<unsigned long long>(st.f_bavail) * st.f_frsize;
    const auto used = total > avail ? total - avail : 0;
    const int percent = total > 0 ? static_cast<int>((used * 100ULL) / total) : 0;

    std::cout
        << "{"
        << "\"size\":" << json_quote(format_bytes(total)) << ","
        << "\"used\":" << json_quote(format_bytes(used)) << ","
        << "\"avail\":" << json_quote(format_bytes(avail)) << ","
        << "\"percent\":" << percent << ","
        << "\"mode\":" << json_quote(mode)
        << "}\n";
    return 0;
}

static int print_partitions_json() {
    std::cout << "[";
    for (std::size_t i = 0; i < kBuiltinPartitions.size(); ++i) {
        const auto& name = kBuiltinPartitions[i];
        const auto mount_point = partition_mount_point(name);
        if (i > 0) {
            std::cout << ",";
        }
        std::cout
            << "{"
            << "\"name\":" << json_quote(name) << ","
            << "\"mount_point\":" << json_quote(mount_point) << ","
            << "\"fs_type\":\"\","
            << "\"is_read_only\":" << (path_is_read_only_mount(mount_point) ? "true" : "false") << ","
            << "\"exists_as_symlink\":" << (fs::is_symlink(mount_point) ? "true" : "false")
            << "}";
    }
    std::cout << "]";
    return 0;
}

static int print_features_json(int bitmask) {
    const auto names = kasumi::feature_names(bitmask);
    std::cout << "{\"bitmask\":" << bitmask << ",\"names\":";
    print_string_array(names);
    std::cout << "}";
    return 0;
}

static void print_lkm_unload_status_json() {
    const auto unload = lkm::unload_status();
    const auto& quiesce = unload.quiesce;
    std::cout << "{"
              << "\"attempted\":" << (unload.attempted ? "true" : "false") << ","
              << "\"quiesce_supported\":"
              << (unload.quiesce_supported ? "true" : "false") << ","
              << "\"capability_errno\":" << unload.capability_errno << ","
              << "\"delete_errno\":" << unload.delete_errno << ","
              << "\"quiesce\":{"
              << "\"ok\":" << (quiesce.ok ? "true" : "false") << ","
              << "\"errno\":" << quiesce.last_errno << ","
              << "\"version\":" << quiesce.version << ","
              << "\"size\":" << quiesce.size << ","
              << "\"flags\":" << quiesce.flags << ","
              << "\"state\":" << static_cast<std::uint32_t>(quiesce.state) << ","
              << "\"busy_mask\":" << quiesce.busy_mask << ","
              << "\"pending_getfd\":" << quiesce.pending_getfd << ","
              << "\"pending_marker\":" << quiesce.pending_marker << ","
              << "\"pending_redirect\":" << quiesce.pending_redirect << ","
              << "\"live_proc_proxy\":" << quiesce.live_proc_proxy << ","
              << "\"live_file_view\":" << quiesce.live_file_view << ","
              << "\"control_files\":" << quiesce.control_files << ","
              << "\"module_refs\":" << quiesce.module_refs << ","
              << "\"err\":" << quiesce.err << "}}";
}

static std::string policy_owner_name(kasumi::PolicyOwner owner) {
    switch (owner) {
    case kasumi::PolicyOwner::Auto:
        return "auto";
    case kasumi::PolicyOwner::KernelSU:
        return "kernelsu";
    case kasumi::PolicyOwner::APatch:
        return "apatch";
    case kasumi::PolicyOwner::Magisk:
        return "magisk";
    case kasumi::PolicyOwner::Manual:
        return "manual";
    case kasumi::PolicyOwner::Disabled:
        return "disabled";
    }
    return "unknown";
}

static bool parse_policy_owner(const std::string& value, kasumi::PolicyOwner& owner) {
    if (value == "auto") {
        owner = kasumi::PolicyOwner::Auto;
    } else if (value == "kernelsu" || value == "ksu") {
        owner = kasumi::PolicyOwner::KernelSU;
    } else if (value == "apatch") {
        owner = kasumi::PolicyOwner::APatch;
    } else if (value == "manual") {
        owner = kasumi::PolicyOwner::Manual;
    } else if (value == "disabled" || value == "off") {
        owner = kasumi::PolicyOwner::Disabled;
    } else {
        return false;
    }
    return true;
}

static std::string policy_uid_list_name(kasumi::PolicyUidList list) {
    switch (list) {
    case kasumi::PolicyUidList::Allow:
        return "allow";
    case kasumi::PolicyUidList::Deny:
        return "deny";
    case kasumi::PolicyUidList::All:
        return "all";
    }
    return "unknown";
}

static bool parse_policy_uid_list(const std::string& value, kasumi::PolicyUidList& list) {
    if (value == "allow" || value == "allowlist") {
        list = kasumi::PolicyUidList::Allow;
    } else if (value == "deny" || value == "denylist") {
        list = kasumi::PolicyUidList::Deny;
    } else if (value == "all") {
        list = kasumi::PolicyUidList::All;
    } else {
        return false;
    }
    return true;
}

template <typename Mutation>
static bool mutate_policy_preserving_enabled(const kasumi::PolicyState& state,
                                              const std::string& operation,
                                              Mutation&& mutation) {
    if (state.enabled && !kasumi::set_enabled(false)) {
        std::cerr << "failed to disable Kasumi before policy update: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    const bool changed = mutation();
    const int mutation_errno = errno;
    if (state.enabled && !kasumi::set_enabled(true)) {
        const int restore_errno = errno;
        if (!changed) {
            std::cerr << "failed to " << operation << ": "
                      << std::strerror(mutation_errno) << "\n";
        }
        std::cerr << "failed to re-enable Kasumi after policy update: "
                  << std::strerror(restore_errno) << "\n";
        errno = restore_errno;
        return false;
    }
    if (!changed) {
        errno = mutation_errno;
        std::cerr << "failed to " << operation << ": "
                  << std::strerror(mutation_errno) << "\n";
        return false;
    }
    return true;
}

static std::uint32_t parse_policy_flags(const std::vector<std::string>& args, std::size_t start, std::uint32_t fallback) {
    if (start >= args.size()) {
        return fallback;
    }
    std::uint32_t flags = 0;
    for (std::size_t i = start; i < args.size(); ++i) {
        const auto& value = args[i];
        if (value == "allow" || value == "allow-uids") {
            flags |= KSM_POLICY_FLAG_USE_ALLOW_UIDS;
        } else if (value == "deny" || value == "deny-uids") {
            flags |= KSM_POLICY_FLAG_USE_DENY_UIDS;
        } else if (value == "isolated" || value == "include-isolated") {
            flags |= KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS;
        } else if (value.rfind("0x", 0) == 0) {
            flags |= static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
        }
    }
    return flags;
}

static bool parse_uid_values(const std::vector<std::string>& args, std::size_t start, std::vector<std::uint32_t>& uids) {
    uids.clear();
    for (std::size_t i = start; i < args.size(); ++i) {
        char* end = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(args[i].c_str(), &end, 0);
        if (errno != 0 || end == args[i].c_str() || *end != '\0' || value > UINT32_MAX) {
            return false;
        }
        uids.push_back(static_cast<std::uint32_t>(value));
    }
    return true;
}

static bool parse_uid_csv(const std::string& value, std::vector<std::uint32_t>& uids) {
    uids.clear();
    if (value.empty() || value == "-") {
        return true;
    }
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (item.empty()) {
            return false;
        }
        char* end = nullptr;
        errno = 0;
        const unsigned long uid = std::strtoul(item.c_str(), &end, 10);
        if (errno != 0 || end == item.c_str() || *end != '\0' || uid == 0 || uid > UINT32_MAX) {
            return false;
        }
        uids.push_back(static_cast<std::uint32_t>(uid));
    }
    std::sort(uids.begin(), uids.end());
    uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
    return true;
}

static void print_roots_json(std::uint32_t roots) {
    std::vector<std::string> names;
    if (roots & (1U << 0)) {
        names.emplace_back("kernelsu");
    }
    if (roots & (1U << 1)) {
        names.emplace_back("kernelsu_redirect");
    }
    if (roots & (1U << 2)) {
        names.emplace_back("apatch");
    }
    if (roots & (1U << 3)) {
        names.emplace_back("magisk");
    }
    if (roots & (1U << 4)) {
        names.emplace_back("multi");
    }
    if (roots & (1U << 5)) {
        names.emplace_back("non_root");
    }
    std::cout << "{\"bitmask\":" << roots << ",\"names\":";
    print_string_array(names);
    std::cout << "}";
}

static void print_policy_flags_json(std::uint32_t flags) {
    std::cout << "{"
              << "\"bitmask\":" << flags << ","
              << "\"use_allow_uids\":" << ((flags & KSM_POLICY_FLAG_USE_ALLOW_UIDS) ? "true" : "false") << ","
              << "\"use_deny_uids\":" << ((flags & KSM_POLICY_FLAG_USE_DENY_UIDS) ? "true" : "false") << ","
              << "\"include_isolated_uids\":" << ((flags & KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS) ? "true" : "false")
              << "}";
}

static int print_policy_json() {
    const auto snapshot = kasumi::policy_snapshot();
    const auto& state = snapshot.state;

    std::cout << "{"
              << "\"ok\":" << (state.ok ? "true" : "false") << ","
              << "\"errno\":" << state.last_errno << ","
              << "\"err\":" << state.err << ","
              << "\"api_version\":" << state.version << ","
              << "\"generation\":" << state.generation << ","
			  << "\"enabled\":" << (state.enabled ? "true" : "false") << ","
              << "\"owner\":" << json_quote(policy_owner_name(state.owner)) << ","
              << "\"effective_owner\":" << json_quote(policy_owner_name(state.effective_owner)) << ","
              << "\"flags\":";
    print_policy_flags_json(state.flags);
    std::cout << ",\"detected_roots\":";
    print_roots_json(state.detected_roots);
    std::cout << ",\"allow_count\":" << state.allow_count
              << ",\"deny_count\":" << state.deny_count
              << ",\"max_uid_count\":" << state.max_uid_count
              << ",\"allow_uids\":";
    print_u32_array(snapshot.allow_uids);
    std::cout << ",\"deny_uids\":";
    print_u32_array(snapshot.deny_uids);
    std::cout << "}\n";
    return state.ok ? 0 : 1;
}

static int print_kasumi_snapshot_json() {
    const auto version = kasumi::version_info();
    const bool available = version.status == kasumi::Status::Available;
    const int bitmask = available ? kasumi::features() : 0;
    const std::string hook_text = available ? kasumi::hooks() : "";
    const auto policy_snapshot = available ? kasumi::policy_snapshot() : kasumi::PolicySnapshot{};
    const auto& policy = policy_snapshot.state;
    const Config config = current_config();

    std::cout << "{"
              << "\"transport\":\"daemon\","
              << "\"available\":" << (available ? "true" : "false") << ","
              << "\"status\":" << static_cast<int>(version.status) << ","
              << "\"expected_protocol\":" << version.expected_protocol << ","
              << "\"kernel_protocol\":" << version.kernel_protocol << ","
              << "\"uid\":" << version.process_uid << ","
              << "\"euid\":" << version.process_euid << ","
              << "\"last_errno\":" << version.last_errno << ","
              << "\"last_error\":"
              << json_quote(version.last_errno == 0 ? "" : std::strerror(version.last_errno)) << ","
              << "\"modules_visible\":" << (version.modules_visible ? "true" : "false") << ","
              << "\"mount_base\":" << json_quote(config.mirror_dir) << ","
              << "\"features\":{\"bitmask\":" << bitmask << ",\"names\":";
    print_string_array(kasumi::feature_names(bitmask));
    std::cout << "},\"hooks\":" << json_quote(hook_text) << ","
              << "\"policy\":{"
              << "\"ok\":" << (policy.ok ? "true" : "false") << ","
              << "\"errno\":" << policy.last_errno << ","
              << "\"err\":" << policy.err << ","
              << "\"api_version\":" << policy.version << ","
              << "\"generation\":" << policy.generation << ","
			  << "\"enabled\":" << (policy.enabled ? "true" : "false") << ","
              << "\"owner\":" << json_quote(policy_owner_name(policy.owner)) << ","
              << "\"effective_owner\":" << json_quote(policy_owner_name(policy.effective_owner)) << ","
              << "\"flags\":" << policy.flags << ","
              << "\"detected_roots\":" << policy.detected_roots << ","
              << "\"allow_count\":" << policy.allow_count << ","
              << "\"deny_count\":" << policy.deny_count << ","
              << "\"max_uid_count\":" << policy.max_uid_count << ","
              << "\"allow_uids\":";
    print_u32_array(policy_snapshot.allow_uids);
    std::cout << ",\"deny_uids\":";
    print_u32_array(policy_snapshot.deny_uids);
    std::cout << "}}\n";
    return 0;
}

static int apply_config_file(const fs::path& path, bool kernel_state_lost = false,
                             bool force_enable = false) {
    Config config;
    std::string error;
    if (!read_config_file(path.string(), config, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (force_enable) {
        config.kasumi_enabled = true;
    }
    if (!config.kasumi_enabled) {
        if ((kasumi::module_loaded() || kasumi::is_available()) &&
            !mount::kasumi::deactivate(error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (!kasumi::is_available()) {
        std::cerr << "Kasumi is unavailable\n";
        return 1;
    }

    // Rebuild module mappings only when this boot already established that
    // Kasumi owns them. A first post-boot load may follow an Overlay/Magic
    // fallback and must not migrate or stack backends underneath live mounts.
    if (mount::kasumi::has_replayable_mappings() &&
        (kernel_state_lost || !mount::kasumi::is_active())) {
        if (!mount::refresh_kasumi_modules(config)) {
            std::cerr << "failed to replay persisted Kasumi configuration\n";
            return 1;
        }
        return print_policy_json();
    }

    if (!mount::kasumi::apply_policy_config(config.policy, error)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << error << "\n";
        return 1;
    }
    if (!mount::kasumi::restore_persisted_hide_rules(error)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << error << "\n";
        return 1;
    }
    if (!mount::kasumi::apply_feature_config(config, error)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << error << "\n";
        return 1;
    }
    if (!mount::fsutil::run_in_init_mount_ns(
            [&]() { return mount::overlay::restore_xattr_hiding(config); })) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << "failed to restore OverlayFS xattr hiding\n";
        return 1;
    }
    if (!kasumi::set_enabled(true)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        std::cerr << "failed to enable Kasumi after config apply\n";
        return 1;
    }
    return print_policy_json();
}

static void print_backend_statuses_json() {
    const auto statuses = mount::backend_statuses();
    std::cout << "[";
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto& status = statuses[i];
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << "{"
                  << "\"kind\":" << json_quote(mount::backend_kind_name(status.kind)) << ","
                  << "\"name\":" << json_quote(status.name) << ","
                  << "\"available\":" << (status.available ? "true" : "false") << ","
                  << "\"preferred\":" << (status.preferred ? "true" : "false") << ","
                  << "\"detail\":" << json_quote(status.detail)
                  << "}";
    }
    std::cout << "]";
}

static int print_system_json() {
    const auto version = kasumi::version_info();
    const int bitmask = version.status == kasumi::Status::Available ? kasumi::features() : 0;
    const std::string hook_text = version.status == kasumi::Status::Available ? kasumi::hooks() : "";

    // Count our live mounts (our mount source) for the stats panel.
    const Config config = current_config();
    int total_mounts = 0;
    int overlay_mounts = 0;
    for (const auto& m : read_mountinfo()) {
        if (m.source != config.mount_source) {
            continue;
        }
        ++total_mounts;
        if (m.fstype == "overlay") {
            ++overlay_mounts;
        }
    }

    std::cout
        << "{"
        << "\"kernel\":" << json_quote(kernel_release()) << ","
        << "\"selinux\":" << json_quote(selinux_status()) << ","
        << "\"mount_base\":" << json_quote(config.mirror_dir) << ","
        << "\"kasumi_available\":" << (version.status == kasumi::Status::Available ? "true" : "false") << ","
        << "\"kasumi_status\":" << static_cast<int>(version.status) << ","
        << "\"hooks\":" << json_quote(hook_text) << ","
        << "\"features\":";
    print_features_json(bitmask);
    std::cout
        << ",\"mountStats\":{\"total_mounts\":" << total_mounts
        << ",\"successful_mounts\":" << total_mounts
        << ",\"failed_mounts\":0,\"tmpfs_created\":0,\"files_mounted\":0,\"dirs_mounted\":0,"
        << "\"symlinks_created\":0,\"overlayfs_mounts\":" << overlay_mounts
        << ",\"success_rate\":" << (total_mounts > 0 ? 100 : 0) << "},"
        << "\"detectedPartitions\":";
    print_partitions_json();
    std::cout << ",\"backends\":";
    print_backend_statuses_json();
    std::cout << "}\n";
    return 0;
}

static int print_meta_json() {
    std::cout << "{"
              << "\"module_id\":\"kagami\","
              << "\"metamodule\":true,"
              << "\"version\":" << json_quote(KAGAMI_VERSION) << ","
              << "\"data_dir\":" << json_quote(runtime_data_dir().string()) << ","
              << "\"modules_dir\":" << json_quote(runtime_modules_dir().string()) << ","
              << "\"config_file\":" << json_quote(runtime_config_file().string()) << ","
              << "\"socket\":" << json_quote(runtime_socket_file().string()) << ","
              << "\"pid_file\":" << json_quote(runtime_pid_file().string()) << ","
              << "\"log_file\":" << json_quote(runtime_log_file().string()) << ","
              << "\"backends\":";
    print_backend_statuses_json();
    std::cout << "}\n";
    return 0;
}

static int print_kasumi_version_json() {
    const auto version = kasumi::version_info();
    const std::string rules = version.status == kasumi::Status::Available ? kasumi::active_rules() : "";
    const auto modules = kasumi::active_modules_from_rules(rules);
    const bool mismatch = version.status == kasumi::Status::KernelTooOld ||
                          version.status == kasumi::Status::ClientTooOld;

    const Config config = current_config();
    std::cout
        << "{"
        << "\"backend\":\"kasumi\","
        << "\"protocol_version\":" << version.expected_protocol << ","
        << "\"kernel_version\":" << version.kernel_protocol << ","
        << "\"kasumi_available\":" << (version.status == kasumi::Status::Available ? "true" : "false") << ","
        << "\"protocol_mismatch\":" << (mismatch ? "true" : "false") << ","
        << "\"mismatch_message\":"
        << json_quote(mismatch ? "Kasumi protocol mismatch" : "") << ","
        << "\"active_modules\":";
    print_string_array(modules);
    std::cout << ",\"mount_base\":" << json_quote(config.mirror_dir) << "}\n";
    return 0;
}

static int print_kasumi_rules_json() {
    const std::string rules = kasumi::active_rules();
    std::istringstream lines(rules);
    std::string line;
    bool first = true;

    std::cout << "[";
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream parts(line);
        std::string type;
        parts >> type;
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });

        if (!first) {
            std::cout << ",";
        }
        first = false;

        std::cout << "{\"type\":" << json_quote(type);
        if (type == "ADD" || type == "MERGE") {
            std::string target;
            std::string source;
            parts >> target >> source;
            std::cout << ",\"target\":" << json_quote(target)
                      << ",\"source\":" << json_quote(source);
        } else if (type == "HIDE") {
            std::string path;
            parts >> path;
            std::cout << ",\"path\":" << json_quote(path);
        } else {
            std::string rest;
            std::getline(parts, rest);
            if (!rest.empty() && rest[0] == ' ') {
                rest.erase(0, 1);
            }
            std::cout << ",\"args\":" << json_quote(rest);
        }
        std::cout << "}";
    }
    std::cout << "]\n";
    return 0;
}

static int handle_config(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "show") {
        std::string config = read_file(config_file());
        if (config.empty()) {
            config = default_config_json();
        }
        // Inject runtime-probed fields the WebUI reads from the merged config.
        std::string runtime = ",\"tmpfs_xattr_supported\":";
        runtime += mount::fsutil::tmpfs_xattr_supported() ? "true" : "false";
        runtime += ",\"kasumi_available\":";
        runtime += kasumi::is_available() ? "true" : "false";
        const auto brace = config.rfind('}');
        if (brace != std::string::npos) {
            config.insert(brace, runtime);
        }
        std::cout << config;
        return 0;
    }
    if (sub == "gen") {
        std::string output = config_file().string();
        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "-o" || args[i] == "--output") {
                output = args[i + 1];
            }
        }
        std::string error;
        if (!write_default_config(output, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "merge-json") {
        if (args.size() != 3) {
            std::cerr << "usage: kagamid config merge-json JSON\n";
            return 1;
        }
        std::string error;
        if (!merge_config_json(config_file().string(), args[2], error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "apply") {
        return apply_config_file(arg_or_default(args, 2, config_file().string()));
    }
    if (sub == "policy") {
        if (arg_or_default(args, 2, "") != "set" || args.size() != 7) {
            std::cerr << "usage: kagamid config policy set <owner> <allow-csv|-> <deny-csv|-> <isolated:on|off>\n";
            return 1;
        }
        kasumi::PolicyOwner parsed_owner = kasumi::PolicyOwner::Auto;
        if (!parse_policy_owner(args[3], parsed_owner)) {
            std::cerr << "policy owner must be auto|kernelsu|apatch|manual|disabled\n";
            return 1;
        }
        PolicyConfig policy;
        policy.owner = policy_owner_name(parsed_owner);
        if (!parse_uid_csv(args[4], policy.allow_uids) || !parse_uid_csv(args[5], policy.deny_uids)) {
            std::cerr << "policy UID values must be positive decimal integers separated by commas\n";
            return 1;
        }
        const std::string isolated = args[6];
        if (isolated != "on" && isolated != "off") {
            std::cerr << "policy isolated value must be on|off\n";
            return 1;
        }
        policy.use_allow_uids = !policy.allow_uids.empty();
        policy.use_deny_uids = !policy.deny_uids.empty();
        policy.include_isolated_uids = isolated == "on";
        std::string error;
        if (!update_policy_config(config_file().string(), policy, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "{\"ok\":true,\"persisted\":true}\n";
        return 0;
    }
    if (sub == "sync-partitions") {
        std::set<std::string> partitions;
        const fs::path module_root = modules_dir();
        if (fs::is_directory(module_root)) {
            for (const auto& module : fs::directory_iterator(module_root)) {
                if (!module.is_directory()) {
                    continue;
                }
                for (const auto& child : fs::directory_iterator(module.path())) {
                    if (child.is_directory()) {
                        const std::string name = child.path().filename().string();
                        if (!is_builtin_partition(name) && module_has_partition_content(module.path(), name)) {
                            partitions.insert(name);
                        }
                    }
                }
            }
        }
        if (partitions.empty()) {
            std::cout << "No new partitions\n";
        } else {
            for (const auto& partition : partitions) {
                std::cout << "Added partition: " << partition << "\n";
            }
        }
        return 0;
    }
    print_usage();
    return 1;
}

static int handle_api(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "system") {
        return print_system_json();
    }
    if (sub == "storage") {
        return print_storage_json();
    }
    if (sub == "lkm") {
        const auto version = kasumi::version_info();
        const bool builtin = version.status == kasumi::Status::Available && !lkm::is_loaded();
        std::cout << "{\"loaded\":" << (lkm::is_loaded() ? "true" : "false")
                  << ",\"builtin\":" << (builtin ? "true" : "false")
                  << ",\"autoload\":" << (lkm::get_autoload() ? "true" : "false")
                  << ",\"kmi_override\":" << json_quote(lkm::get_kmi_override())
                  << ",\"detected_kmi\":" << json_quote(lkm::current_kmi())
                  << ",\"asset\":" << json_quote(lkm::find_asset())
                  << ",\"last_error\":" << json_quote(lkm::last_error())
                  << ",\"unload\":";
        print_lkm_unload_status_json();
        std::cout << "}\n";
        return 0;
    }
    if (sub == "kasumi") {
        return print_kasumi_snapshot_json();
    }
    if (sub == "features") {
        const auto version = kasumi::version_info();
        return print_features_json(version.status == kasumi::Status::Available ? kasumi::features() : 0);
    }
    if (sub == "hooks") {
        if (!kasumi::is_available()) {
            std::cerr << "Kasumi not available.\n";
            return 1;
        }
        std::cout << kasumi::hooks() << "\n";
        return 0;
    }
    if (sub == "policy") {
        return print_policy_json();
    }
    if (sub == "backends") {
        print_backend_statuses_json();
        std::cout << "\n";
        return 0;
    }
    if (sub == "meta") {
        return print_meta_json();
    }
    print_usage();
    return 1;
}

static int handle_module(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "list") {
        const auto modes = mount::load_module_modes();
        const auto rule_map = mount::load_module_rules();
        const auto replayable = mount::kasumi::replayable_module_ids();
        const std::set<std::string> kasumi_boot_plan(replayable.begin(), replayable.end());
        const Config cfg = current_config();
        const fs::path module_root = cfg.module_dir.empty() ? modules_dir() : fs::path(cfg.module_dir);
        std::cout << "{\"modules\":[";
        bool first = true;
        if (fs::is_directory(module_root)) {
            for (const auto& entry : fs::directory_iterator(module_root)) {
                if (!entry.is_directory()) {
                    continue;
                }
                // Skip modules that opt out of metamodule mounting.
                std::error_code mec;
                if (fs::exists(entry.path() / "disable", mec) ||
                    fs::exists(entry.path() / "remove", mec) ||
                    fs::exists(entry.path() / "skip_mount", mec) ||
                    fs::exists(data_dir() / "run" / "hot_unmounted" / entry.path().filename(), mec) ||
                    !fs::exists(entry.path() / "module.prop", mec)) {
                    continue;
                }
                // Only list modules that contribute mounts (have a managed
                // partition tree); skip plain modules (zygisk, etc.).
                bool has_mount_content = false;
                const std::vector<std::string>& parts =
                    cfg.partitions.empty() ? mount::fsutil::kManagedPartitions : cfg.partitions;
                for (const auto& part : parts) {
                    std::error_code ec;
                    if (fs::is_directory(entry.path() / part, ec)) {
                        has_mount_content = true;
                        break;
                    }
                }
                if (!has_mount_content) {
                    continue;
                }
                const std::string id = entry.path().filename().string();
                const auto props = read_prop_file(entry.path() / "module.prop");
                if (!first) {
                    std::cout << ",";
                }
                first = false;
                const auto mode_it = modes.find(id);
                const std::string mode = mode_it == modes.end() ? "auto" : mode_it->second;
                const auto rules_it = rule_map.find(id);
                // The same-boot plan is authoritative for Kasumi ownership.
                // Without it, an explicit Kasumi choice may already be running
                // through the boot-time Overlay/Magic fallback.
                std::string strategy;
                if (kasumi_boot_plan.count(id) != 0) {
                    strategy = "kasumi";
                } else {
                    Config fallback_config = cfg;
                    fallback_config.kasumi_enabled = false;
                    strategy = mount::resolve_module_backend(
                        mount::ModuleEntry{id, entry.path()}, fallback_config, modes);
                }
                std::cout << "{"
                          << "\"id\":" << json_quote(id) << ","
                          << "\"name\":" << json_quote(props.count("name") ? props.at("name") : id) << ","
                          << "\"version\":" << json_quote(props.count("version") ? props.at("version") : "") << ","
                          << "\"author\":" << json_quote(props.count("author") ? props.at("author") : "") << ","
                          << "\"description\":" << json_quote(props.count("description") ? props.at("description") : "") << ","
                          << "\"mode\":" << json_quote(mode) << ","
                          << "\"strategy\":" << json_quote(strategy) << ","
                          << "\"path\":" << json_quote(entry.path().string()) << ","
                          << "\"rules\":[";
                if (rules_it != rule_map.end()) {
                    for (std::size_t i = 0; i < rules_it->second.size(); ++i) {
                        if (i > 0) {
                            std::cout << ",";
                        }
                        std::cout << "{\"path\":" << json_quote(rules_it->second[i].path)
                                  << ",\"mode\":" << json_quote(rules_it->second[i].mode) << "}";
                    }
                }
                std::cout << "]}";
            }
        }
        std::cout << "]}\n";
        return 0;
    }
    if (sub == "set-mode") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string mode = arg_or_default(args, 3, "");
        if (!valid_module_id(id) || (mode != "auto" && mode != "kasumi" && mode != "overlay" &&
                                    mode != "magic" && mode != "none")) {
            std::cerr << "usage: kagamid module set-mode <id> auto|kasumi|overlay|magic|none\n";
            return 1;
        }
        auto modes = mount::load_module_modes();
        if (mode == "auto") {
            modes.erase(id);
        } else {
            modes[id] = mode;
        }
        if (!mount::save_module_modes(modes)) {
            std::cerr << "failed to save module modes\n";
            return 1;
        }
        return 0;
    }
    if (sub == "add-rule") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string path = arg_or_default(args, 3, "");
        const std::string mode = arg_or_default(args, 4, "");
        if (!valid_module_id(id) || path.empty() || path.front() != '/' || !valid_module_mode(mode)) {
            std::cerr << "usage: kagamid module add-rule <id> <absolute-path> kasumi|overlay|magic|none|hide\n";
            return 1;
        }
        auto rules = mount::load_module_rules();
        auto& module_rules = rules[id];
        const auto existing = std::find_if(module_rules.begin(), module_rules.end(), [&](const auto& rule) {
            return rule.path == path;
        });
        if (existing == module_rules.end()) {
            module_rules.push_back({path, mode});
        } else {
            existing->mode = mode;
        }
        if (!mount::save_module_rules(rules)) {
            std::cerr << "failed to save module rules\n";
            return 1;
        }
        return 0;
    }
    if (sub == "remove-rule") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string path = arg_or_default(args, 3, "");
        if (!valid_module_id(id) || path.empty() || path.front() != '/') {
            std::cerr << "usage: kagamid module remove-rule <id> <absolute-path>\n";
            return 1;
        }
        auto rules = mount::load_module_rules();
        const auto rules_it = rules.find(id);
        if (rules_it == rules.end()) {
            return 0;
        }
        auto& module_rules = rules_it->second;
        module_rules.erase(std::remove_if(module_rules.begin(), module_rules.end(), [&](const auto& rule) {
            return rule.path == path;
        }), module_rules.end());
        if (module_rules.empty()) {
            rules.erase(rules_it);
        }
        if (!mount::save_module_rules(rules)) {
            std::cerr << "failed to save module rules\n";
            return 1;
        }
        return 0;
    }
    if (sub == "hot-mount" || sub == "hot-unmount" || sub == "add" || sub == "delete") {
        const std::string id = arg_or_default(args, 2, "");
        if (!valid_module_id(id)) {
            std::cerr << "usage: kagamid module " << sub << " <id>\n";
            return 1;
        }
        const Config cfg = current_config();
        const fs::path module_root = cfg.module_dir.empty() ? modules_dir() : fs::path(cfg.module_dir);
        const fs::path module_path = module_root / id;
        if (!fs::is_directory(module_path) || !fs::exists(module_path / "module.prop")) {
            std::cerr << "module not found: " << id << "\n";
            return 1;
        }
        const auto replayable = mount::kasumi::replayable_module_ids();
        if (std::find(replayable.begin(), replayable.end(), id) == replayable.end()) {
            std::cerr << "module was not assigned to Kasumi this boot; reboot to change its backend\n";
            return 1;
        }
        const fs::path marker = data_dir() / "run" / "hot_unmounted" / id;
        std::error_code ec;
        const bool unmounting = sub == "hot-unmount" || sub == "delete";
        const bool existed = fs::exists(marker, ec);
        fs::create_directories(marker.parent_path(), ec);
        if (ec) {
            std::cerr << "failed to prepare hot-mount state: " << ec.message() << "\n";
            return 1;
        }
        if (unmounting) {
            std::ofstream(marker, std::ios::trunc).put('\n');
        } else {
            fs::remove(marker, ec);
        }
        if (!mount::refresh_kasumi_modules(cfg)) {
            if (unmounting && !existed) {
                fs::remove(marker, ec);
            } else if (!unmounting && existed) {
                std::ofstream(marker, std::ios::trunc).put('\n');
            }
            std::cerr << "failed to refresh Kasumi mappings\n";
            return 1;
        }
        std::cout << "{\"ok\":true,\"module\":" << json_quote(id)
                  << ",\"action\":" << json_quote(sub) << "}\n";
        return 0;
    }
    if (sub == "check-conflicts") {
        const Config cfg = current_config();
        const auto modules = mount::enumerate_mountable_modules(cfg);
        const std::vector<std::string>& parts =
            cfg.partitions.empty() ? mount::fsutil::kManagedPartitions : cfg.partitions;
        std::map<std::string, std::vector<std::string>> owners;
        for (const auto& module : modules) {
            for (const auto& part : parts) {
                const fs::path root = module.path / part;
                std::error_code ec;
                auto it = fs::recursive_directory_iterator(root, ec);
                const auto end = fs::recursive_directory_iterator();
                for (; it != end && !ec; it.increment(ec)) {
                    if (it->is_regular_file(ec) || it->is_symlink(ec)) {
                        owners[(fs::path("/") / part / fs::relative(it->path(), root, ec)).string()].push_back(module.id);
                    }
                }
            }
        }
        std::cout << "[";
        bool first = true;
        for (const auto& [path, ids] : owners) {
            if (ids.size() < 2) {
                continue;
            }
            if (!first) {
                std::cout << ",";
            }
            first = false;
            std::cout << "{\"file\":" << json_quote(path) << ",\"modules\":";
            print_string_array(ids);
            std::cout << "}";
        }
        std::cout << "]\n";
        return 0;
    }
    if (sub == "mount-all") {
        // Boot-only entry (the metamodule metamount.sh hook). Refuse once the
        // device has booted: magic mount over the live system post-boot breaks
        // mount namespaces (it propagates into adbd / service namespaces).
        if (system_boot_completed() && std::getenv("KAGAMI_MOUNT_HERE") == nullptr) {
            std::cerr << "refusing module mount-all: magic mount only runs at boot via "
                         "metamount.sh; post-boot mounting breaks namespaces\n";
            return 1;
        }
        Config config;
        std::string cfg_err;
        read_config_file(config_file().string(), config, cfg_err); // defaults on error
        const auto report = mount::mount_all_enabled(config);
        std::cout << "{"
                  << "\"ok\":" << (report.ok ? "true" : "false") << ","
                  << "\"backend\":" << json_quote(report.backend) << ","
                  << "\"modules\":" << report.modules << ","
                  << "\"mounts\":" << report.mounts << ","
                  << "\"detail\":" << json_quote(report.detail)
                  << "}\n";
        return report.ok ? 0 : 1;
    }
    if (sub == "normalize") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty()) {
            std::cerr << "usage: kagamid module normalize <module_path>\n";
            return 1;
        }
        const bool ok = mount::magic::normalize_module(path);
        std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"module\":" << json_quote(path) << "}\n";
        return ok ? 0 : 1;
    }
    if (sub == "unmount") {
        // Tear down Kagami's own mounts (source-gated; never touches real partitions).
        Config config;
        std::string cfg_err;
        read_config_file(config_file().string(), config, cfg_err); // defaults on error
        const bool ok = mount::unmount_all(config);
        std::cout << "{\"ok\":" << (ok ? "true" : "false") << "}\n";
        return ok ? 0 : 1;
    }
    print_usage();
    return 1;
}

static int handle_kasumi(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "version") {
        return print_kasumi_version_json();
    }
    if (sub == "list") {
        return print_kasumi_rules_json();
    }
    if (sub == "features") {
        const auto version = kasumi::version_info();
        return print_features_json(version.status == kasumi::Status::Available ? kasumi::features() : 0);
    }
    if (sub == "maps") {
        const std::string op = arg_or_default(args, 2, "");
        if (op == "clear") {
            if (!kasumi::clear_maps_rules()) {
                std::cerr << "failed to clear Kasumi maps rules\n";
                return 1;
            }
            return 0;
        }
        if (op == "add") {
            if (args.size() < 8) {
                std::cerr << "usage: kagamid kasumi maps add <target-ino> <target-dev> <spoof-ino> <spoof-dev> <spoof-path>\n";
                return 1;
            }
            unsigned long target_ino = 0;
            unsigned long target_dev = 0;
            unsigned long spoof_ino = 0;
            unsigned long spoof_dev = 0;
            if (!parse_unsigned_long(args[3], target_ino) || !parse_unsigned_long(args[4], target_dev) ||
                !parse_unsigned_long(args[5], spoof_ino) || !parse_unsigned_long(args[6], spoof_dev)) {
                std::cerr << "Kasumi maps values must be unsigned integers\n";
                return 1;
            }
            if (!kasumi::add_maps_rule(target_ino, target_dev, spoof_ino, spoof_dev, args[7])) {
                std::cerr << "failed to add Kasumi maps rule\n";
                return 1;
            }
            return 0;
        }
        std::cerr << "usage: kagamid kasumi maps clear|add ...\n";
        return 1;
    }
    if (sub == "policy") {
        const auto op = arg_or_default(args, 2, "show");
        if (op == "show" || op == "state") {
            return print_policy_json();
        }
        if (op == "owner" || op == "set") {
            kasumi::PolicyOwner owner = kasumi::PolicyOwner::Auto;
            if (!parse_policy_owner(arg_or_default(args, 3, ""), owner)) {
                std::cerr << "policy owner must be auto|kernelsu|apatch|manual|disabled\n";
                return 1;
            }
            const auto current = kasumi::policy_snapshot();
            if (!current.state.ok) {
                std::cerr << "failed to read current Kasumi policy\n";
                return 1;
            }
            const std::uint32_t flags = parse_policy_flags(args, 4, current.state.flags);
            const auto allow_uids = (flags & KSM_POLICY_FLAG_USE_ALLOW_UIDS)
                                        ? current.allow_uids
                                        : std::vector<std::uint32_t>{};
            const auto deny_uids = (flags & KSM_POLICY_FLAG_USE_DENY_UIDS)
                                       ? current.deny_uids
                                       : std::vector<std::uint32_t>{};
            return mutate_policy_preserving_enabled(
                       current.state, "set Kasumi policy owner",
                       [&]() {
                           return kasumi::replace_policy(owner, flags,
                                                         allow_uids, deny_uids);
                       })
                       ? 0
                       : 1;
        }
        if (op == "allow" || op == "deny") {
            std::vector<std::uint32_t> uids;
            if (!parse_uid_values(args, 3, uids)) {
                std::cerr << "policy " << op << " requires numeric uid values\n";
                return 1;
            }
            auto current = kasumi::policy_snapshot();
            if (!current.state.ok) {
                std::cerr << "failed to read current Kasumi policy\n";
                return 1;
            }
            if (op == "allow") {
                current.state.flags |= KSM_POLICY_FLAG_USE_ALLOW_UIDS;
                current.allow_uids = std::move(uids);
            } else {
                current.state.flags |= KSM_POLICY_FLAG_USE_DENY_UIDS;
                current.deny_uids = std::move(uids);
            }
            return mutate_policy_preserving_enabled(
                       current.state, "set Kasumi policy " + op + " uid list",
                       [&]() {
                           return kasumi::replace_policy(
                               current.state.owner, current.state.flags,
                               current.allow_uids, current.deny_uids);
                       })
                       ? 0
                       : 1;
        }
        if (op == "clear") {
            kasumi::PolicyUidList list = kasumi::PolicyUidList::All;
            if (!parse_policy_uid_list(arg_or_default(args, 3, "all"), list)) {
                std::cerr << "policy clear target must be allow|deny|all\n";
                return 1;
            }
            auto current = kasumi::policy_snapshot();
            if (!current.state.ok) {
                std::cerr << "failed to read current Kasumi policy\n";
                return 1;
            }
            if (list == kasumi::PolicyUidList::Allow || list == kasumi::PolicyUidList::All) {
				current.state.flags &= ~KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS;
				if (current.state.owner != kasumi::PolicyOwner::Manual) {
					current.state.flags &= ~KSM_POLICY_FLAG_USE_ALLOW_UIDS;
				}
                current.allow_uids.clear();
            }
            if (list == kasumi::PolicyUidList::Deny || list == kasumi::PolicyUidList::All) {
                current.state.flags &= ~KSM_POLICY_FLAG_USE_DENY_UIDS;
                current.deny_uids.clear();
            }
            return mutate_policy_preserving_enabled(
                       current.state,
                       "clear Kasumi policy " + policy_uid_list_name(list) +
                           " uid list",
                       [&]() {
                           return kasumi::replace_policy(
                               current.state.owner, current.state.flags,
                               current.allow_uids, current.deny_uids);
                       })
                       ? 0
                       : 1;
        }
        if (op == "apply") {
            return apply_config_file(arg_or_default(args, 3, config_file().string()));
        }
        if (op == "reset") {
            const auto current = kasumi::policy_snapshot();
            if (!current.state.ok) {
                std::cerr << "failed to read current Kasumi policy\n";
                return 1;
            }
            return mutate_policy_preserving_enabled(
                       current.state, "reset Kasumi policy",
                       []() { return kasumi::reset_policy(); })
                       ? 0
                       : 1;
        }
        std::cerr << "usage: kagamid kasumi policy [show|owner OWNER [allow] [deny] [isolated]|allow UID...|deny UID...|clear allow|deny|all|reset|apply [config]]\n";
        return 1;
    }
    if (sub == "enable") {
        return apply_config_file(config_file(), false, true);
    }
    if (sub == "disable") {
        if (!kasumi::set_enabled(false)) {
            std::cerr << "failed to set Kasumi enabled state\n";
            return 1;
        }
        return 0;
    }
    if (sub == "clear") {
        if (!kasumi::clear_rules()) {
            std::cerr << "failed to clear Kasumi rules\n";
            return 1;
        }
        mount::kasumi::invalidate_active_state();
        return 0;
    }
    if (sub == "hide-path" || sub == "delete-rule") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            std::cerr << "Kasumi path must be absolute\n";
            return 1;
        }
        const bool ok = sub == "hide-path" ? kasumi::hide_path(path)
                                             : kasumi::delete_rule(path);
        if (!ok) {
            std::cerr << "failed to update Kasumi path rule\n";
            return 1;
        }
        return 0;
    }
    if (sub == "fix-mounts") {
        if (!kasumi::fix_mounts()) {
            std::cerr << "failed to reorder Kasumi mount ids\n";
            return 1;
        }
        return 0;
    }
    if (sub == "set-mirror") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            std::cerr << "kasumi set-mirror requires an absolute path\n";
            return 1;
        }
        if (!kasumi::set_mirror_path(path)) {
            std::cerr << "failed to set Kasumi mirror path\n";
            return 1;
        }
        return 0;
    }
    if (sub == "hide-overlay-xattrs") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            std::cerr << "kasumi hide-overlay-xattrs requires an absolute path\n";
            return 1;
        }
        if (!kasumi::hide_overlay_xattrs(path)) {
            std::cerr << "failed to hide overlay xattrs for " << path << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "mount-hide" || sub == "maps-spoof" || sub == "statfs-spoof" ||
        sub == "selinux-fix") {
        const bool on = arg_or_default(args, 2, "off") == "on";
        bool ok = false;
        if (sub == "mount-hide") {
            ok = kasumi::set_mount_hide(on);
        } else if (sub == "maps-spoof") {
            ok = kasumi::set_maps_spoof(on);
        } else if (sub == "statfs-spoof") {
            ok = kasumi::set_statfs_spoof(on);
        } else {
            ok = kasumi::set_selinux_guard(on);
        }
        if (!ok) {
            std::cerr << "failed to set Kasumi " << sub << "\n";
            return 1;
        }
        return 0;
    }
    print_usage();
    return 1;
}

static int handle_lkm(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "set-autoload") {
        const std::string value = arg_or_default(args, 2, "");
        if (value != "on" && value != "off" && value != "1" && value != "0" &&
            value != "true" && value != "false") {
            std::cerr << "usage: kagamid lkm set-autoload on|off\n";
            return 1;
        }
        const bool on = value == "on" || value == "1" || value == "true";
        if (!lkm::set_autoload(on)) {
            std::cerr << lkm::last_error() << "\n";
            return 1;
        }
        std::cout << "autoload=" << (on ? "on" : "off") << "\n";
        return 0;
    }
    if (sub == "set-kmi") {
        const std::string kmi = arg_or_default(args, 2, "");
        if (!lkm::set_kmi_override(kmi)) {
            std::cerr << lkm::last_error() << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "clear-kmi") {
        if (!lkm::clear_kmi_override()) {
            std::cerr << lkm::last_error() << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "load") {
        const bool already_available = kasumi::is_available();
        if (!lkm::load()) {
            std::cerr << "failed to load Kasumi LKM: " << lkm::last_error() << "\n";
            return 1;
        }
        return already_available ? 0 : apply_config_file(config_file(), true);
    }
    if (sub == "unload" || sub == "force-unload") {
        if (lkm::unload(sub != "force-unload")) {
            return 0;
        }
        std::cerr << "failed to unload Kasumi LKM: " << lkm::last_error() << "\n";
        return 1;
    }
    if (sub == "autoload") {
        const bool enabled = lkm::get_autoload();
        const bool already_available = kasumi::is_available();
        if (!lkm::autoload()) {
            std::cerr << "Kasumi LKM autoload failed: " << lkm::last_error() << "\n";
            return 1;
        }
        if (!enabled) {
            return 0;
        }
        return already_available ? 0 : apply_config_file(config_file(), true);
    }
    if (sub == "status") {
        std::cout << "{\"loaded\":" << (lkm::is_loaded() ? "true" : "false")
                  << ",\"autoload\":" << (lkm::get_autoload() ? "true" : "false")
                  << ",\"kmi_override\":" << json_quote(lkm::get_kmi_override())
                  << ",\"detected_kmi\":" << json_quote(lkm::current_kmi())
                  << ",\"asset\":" << json_quote(lkm::find_asset())
                  << ",\"last_error\":" << json_quote(lkm::last_error())
                  << ",\"unload\":";
        print_lkm_unload_status_json();
        std::cout << "}\n";
        return 0;
    }
    std::cerr << "usage: kagamid lkm load|unload|force-unload|status|autoload|set-autoload|set-kmi|clear-kmi\n";
    return 1;
}

static int handle_hide(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "list") {
        const auto rules = load_user_hide_rules();
        print_string_array(rules);
        std::cout << "\n";
        return 0;
    }
    if (sub == "add") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path[0] != '/') {
            std::cerr << "hide path must be absolute\n";
            return 1;
        }
        auto rules = load_user_hide_rules();
        if (std::find(rules.begin(), rules.end(), path) == rules.end()) {
            rules.push_back(path);
        }
        if (!save_user_hide_rules(rules)) {
            return 1;
        }
        if (kasumi::is_available()) {
            kasumi::hide_path(path);
        }
        return 0;
    }
    if (sub == "remove") {
        const std::string path = arg_or_default(args, 2, "");
        auto rules = load_user_hide_rules();
        rules.erase(std::remove(rules.begin(), rules.end(), path), rules.end());
        if (!save_user_hide_rules(rules)) {
            return 1;
        }
        if (kasumi::is_available()) {
            kasumi::delete_rule(path);
        }
        return 0;
    }
    print_usage();
    return 1;
}

static int handle_recovery(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "status");
    if (sub == "boot-completed") {
        mount::recovery_boot_completed();
        std::cout << "{\"ok\":true,\"action\":\"boot-completed\"}\n";
        return 0;
    }
    if (sub == "reset") {
        mount::recovery_reset();
        std::cout << "{\"ok\":true,\"action\":\"reset\"}\n";
        return 0;
    }
    if (sub == "status") {
        std::cout << mount::recovery_status_json() << "\n";
        return 0;
    }
    std::cerr << "usage: kagamid recovery status|boot-completed|reset\n";
    return 1;
}

int run_command(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "version" || args[0] == "--version") {
        std::cout << KAGAMI_VERSION << "\n";
        return 0;
    }
    if (args[0] == "daemon") {
        return run_daemon_command(args);
    }
    if (args[0] == "config") {
        return handle_config(args);
    }
    if (args[0] == "api") {
        return handle_api(args);
    }
    if (args[0] == "module") {
        return handle_module(args);
    }
    if (args[0] == "kasumi") {
        return handle_kasumi(args);
    }
    if (args[0] == "debug") {
        if (args.size() >= 2 && (args[1] == "enable" || args[1] == "disable")) {
            return kasumi::set_debug(args[1] == "enable") ? 0 : 1;
        }
        if (args.size() >= 3 && args[1] == "stealth") {
            return kasumi::set_stealth(args[2] == "enable") ? 0 : 1;
        }
        if (args.size() >= 2 && args[1] == "set-cmdline") {
            return kasumi::set_cmdline(arg_or_default(args, 2, "")) ? 0 : 1;
        }
        if (args.size() >= 2 && args[1] == "clear-cmdline") {
            return kasumi::set_cmdline("") ? 0 : 1;
        }
        return 0;
    }
    if (args[0] == "lkm") {
        return handle_lkm(args);
    }
    if (args[0] == "hide") {
        return handle_hide(args);
    }
    if (args[0] == "recovery") {
        return handle_recovery(args);
    }

    print_usage();
    return 1;
}

CommandResult run_command_capture(const std::vector<std::string>& args) {
    std::ostringstream stdout_buffer;
    std::ostringstream stderr_buffer;
    auto* old_stdout = std::cout.rdbuf(stdout_buffer.rdbuf());
    auto* old_stderr = std::cerr.rdbuf(stderr_buffer.rdbuf());
    errno = 0;
    const int exit_code = run_command(args);
    const int error_number = exit_code == 0 ? 0 : (errno != 0 ? errno : EIO);
    std::cout.rdbuf(old_stdout);
    std::cerr.rdbuf(old_stderr);
    return {exit_code, error_number, stdout_buffer.str(), stderr_buffer.str()};
}

} // namespace kagami
