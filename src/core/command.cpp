#include "core/command.hpp"
#include <iterator>

#include <chrono>
#include "core/daemon.hpp"
#include "core/human_bytes.hpp"
#include "core/json.hpp"
#include "core/json_value.hpp"
#include "core/lkm.hpp"
#include "core/log.hpp"
#include "core/mount_table.hpp"
#include "core/prepared_input.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"
#include "kagami/kasumi_uapi_compat.hpp"
#include "mount/backend.hpp"
#include "mount/kasumi.hpp"
#include "mount/magic_mount.hpp"
#include "mount/module_inventory.hpp"
#include "mount/module_runtime.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"
#include "mount/storage.hpp"

#include <sys/statvfs.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "core/number.hpp"
#include "core/output.hpp"

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace kagami {

namespace fs = std::filesystem;

namespace {
inline constexpr std::array<const char*, 6> partition_paths = {"/system",     "/vendor", "/product",
                                                               "/system_ext", "/odm",    "/oem"};
inline constexpr std::array<std::string_view, 6> partition_names = {
    "system", "vendor", "product", "system_ext", "odm", "oem"};
const auto& builtin_partitions() noexcept {
    return partition_names;
}
}  // namespace

namespace {
fs::path data_dir() {
    return runtime_data_dir();
}

fs::path modules_dir() {
    return runtime_modules_dir();
}

fs::path config_file() {
    return runtime_config_file();
}

fs::path user_hide_rules_file() {
    return data_dir() / "user_hide_rules.json";
}

void print_usage(output::Context& destination) {
    output::out(
        destination, "Kagami ", KAGAMI_VERSION, "\n", "usage:\n", "  kagamid version\n",
        "  kagamid config show\n", "  kagamid config gen [-o PATH]\n",
        "  kagamid config merge-json JSON\n", "  kagamid config apply [PATH]\n",
        "  kagamid config policy set <owner> <allow-csv|-> <deny-csv|-> <isolated:on|off>\n",
        "  kagamid daemon status|serve|call|ping|stop\n",
        "  kagamid api system|storage|lkm|kasumi|features|hooks|policy|backends|meta\n",
        "  kagamid module "
        "list|add|delete|set-mode|add-rule|remove-rule|hot-mount|hot-unmount|check-conflicts|"
        "mount-all|unmount|normalize\n",
        "  kagamid recovery status|boot-completed|reset\n",
        "  kagamid kasumi "
        "version|list|enable|disable|clear|fix-mounts|hide-overlay-xattrs|mount-hide "
        "off|normal|aggressive|maps|policy\n",
        "  kagamid lkm load|unload|status|autoload|set-autoload|set-kmi|clear-kmi\n");
}

std::string arg_or_default(Arguments args, std::size_t index, const std::string& fallback) {
    return index < args.size() ? std::string(std::string_view(args[index])) : fallback;
}

// True once the device has finished booting. Magic mount must only run during
// the post-fs-data boot stage (metamount.sh); mounting over the live system
// afterwards breaks mount namespaces, so the CLI refuses mount-all post-boot.
bool system_boot_completed() {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("sys.boot_completed", value) > 0) {
        return std::string(value) == "1";
    }
#endif
    return false;
}

void print_string_array(output::Context& destination, const std::vector<std::string>& values) {
    output::out(destination, "[");
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            output::out(destination, ",");
        }
        output::out(destination, bounded::quoted(values[i]));
    }
    output::out(destination, "]");
}

void print_u32_array(output::Context& destination, const std::vector<std::uint32_t>& values) {
    output::out(destination, "[");
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            output::out(destination, ",");
        }
        output::out(destination, values[i]);
    }
    output::out(destination, "]");
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "";
    }
    std::ostringstream out;
    std::copy(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(),
              std::ostreambuf_iterator<char>(out));
    return out.str();
}

bool write_file(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << content;
    return out.good();
}

bool is_builtin_partition(const std::string& name) {
    return std::find(builtin_partitions().begin(), builtin_partitions().end(), name) !=
           builtin_partitions().end();
}

bool valid_module_id(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

bool valid_module_mode(const std::string& mode) {
    return mode == "auto" || mode == "kasumi" || mode == "overlay" || mode == "magic" ||
           mode == "none" || mode == "hide";
}

bool parse_unsigned_long(const std::string& value, unsigned long& out) {
    return parse_unsigned(value, out);
}

bool module_has_partition_content(const fs::path& module_path, const std::string& partition) {
    const fs::path root = module_path / partition;
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return false;
    const fs::directory_iterator first(root, ec);
    return !ec && first != fs::directory_iterator{};
}

std::vector<std::string> parse_json_string_array(const std::string& json) {
    std::vector<std::string> values;
    bool in_string = false;
    bool escape = false;
    std::string value;

    for (char const c : json) {
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

std::vector<std::string> load_user_hide_rules() {
    return parse_json_string_array(read_file(user_hide_rules_file()));
}

bool save_user_hide_rules(const std::vector<std::string>& rules) {
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

std::string_view kernel_release(std::string_view version) noexcept {
    version = mount_table::next_line(version);
    constexpr std::string_view marker = "Linux version ";
    const auto start = version.find(marker);
    if (start == std::string_view::npos)
        return version.empty() ? "Unknown" : version;
    version.remove_prefix(start + marker.size());
    version.remove_suffix(version.size() - std::min(version.find(' '), version.size()));
    return version;
}

const char* selinux_status() noexcept {
    const posix::Fd fd(open("/sys/fs/selinux/enforce", O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
        return "Unknown";
    char bytes[2];
    std::size_t size = 0;
    while (size < sizeof(bytes)) {
        const auto count = read(fd.get(), bytes + size, sizeof(bytes) - size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return "Unknown";
        if (!count)
            break;
        size += static_cast<std::size_t>(count);
    }
    if (!size || (size == 2 && bytes[1] != '\n'))
        return "Unknown";
    if (bytes[0] == '0')
        return "Permissive";
    return bytes[0] == '1' ? "Enforcing" : "Unknown";
}

}  // namespace

namespace {
PreparedConfig current_config() {
    PreparedConfig config;
    std::string error;
    read_config_file(config_file().string(), config, error);  // struct defaults on miss
    return config;
}

int print_storage_json(output::Context& destination) {
    const auto config = current_config();
    const std::string mirror_base = mount::storage::current_mirror_dir(config);

    // Report the active backend's storage base (tmpfs/ext4/erofs): the shared
    // mirror root if mounted, else the Magic Mount work tmpfs, else fall back
    // to the host /data filesystem. Loop-backed ext4/EROFS uses a loop-device
    // source, so the mirror is identified by mountpoint rather than source.
    mount_table::Table mounts;
    int error = 0;
    if (!mounts.prepare_file("/proc/self/mountinfo", error)) {
        output::err(destination, "mount table read failed: ", std::strerror(error), '\n');
        return 1;
    }
    std::string_view mode = "host";
    const auto default_target = data_dir();
    const char* target = default_target.c_str();
    mounts.consume([&](const mount_table::Entry& m) noexcept {
        if (m.mount_point == mirror_base ||
            (m.source == config.mount_source && m.mount_point == config.work_dir)) {
            mode = m.fstype;
            target = m.mount_point.data();  // decoded token is explicitly terminated
            return false;
        }
        return true;
    });

    struct statvfs st = {};
    if (statvfs(target, &st) != 0) {
        output::out(destination, "{\"error\":\"not mounted\"}\n");
        return 0;
    }

    const auto total = static_cast<unsigned long long>(st.f_blocks) * st.f_frsize;
    const auto avail = static_cast<unsigned long long>(st.f_bavail) * st.f_frsize;
    const auto used = total > avail ? total - avail : 0;
    const int percent = total > 0 ? static_cast<int>((used * 100ULL) / total) : 0;

    output::out(destination, "{\"size\":\"");
    if (!write_human_bytes(destination.out, total) || !destination.out.append("\",\"used\":\"") ||
        !write_human_bytes(destination.out, used) || !destination.out.append("\",\"avail\":\"") ||
        !write_human_bytes(destination.out, avail)) {
        destination.error = ENOBUFS;
        return 1;
    }
    output::out(destination, "\",\"percent\":", percent, ",\"mode\":", bounded::quoted(mode),
                "}\n");
    return 0;
}

int print_partitions_json(output::Context& destination) {
    PreparedInput mounts;
    int error = 0;
    if (!mounts.prepare_file("/proc/mounts", error, PreparedInput::maximum)) {
        output::err(destination, "partition table read failed: ", std::strerror(error), '\n');
        return 1;
    }
    const auto readonly = mount_table::read_permissions(mounts.view(), partition_paths);
    output::out(destination, "[");
    for (std::size_t i = 0; i < builtin_partitions().size(); ++i) {
        const auto& name = builtin_partitions()[i];
        const auto* mount_point = partition_paths[i];
        if (i > 0) {
            output::out(destination, ",");
        }
        // lstat initializes every field consumed below on success.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct stat info;
        const int status = lstat(mount_point, &info);
        if (status && errno != ENOENT && errno != ENOTDIR) {
            output::err(destination, "partition metadata read failed\n");
            return 1;
        }
        const bool symlink = status == 0 && S_ISLNK(info.st_mode);
        output::out(destination, "{", "\"name\":", bounded::quoted(name), ",",
                    "\"mount_point\":", bounded::quoted(mount_point), ",", "\"fs_type\":\"\",",
                    "\"is_read_only\":", (readonly[i] ? "true" : "false"), ",",
                    "\"exists_as_symlink\":", (symlink ? "true" : "false"), "}");
    }
    output::out(destination, "]");
    return 0;
}

int print_features_json(output::Context& destination, int bitmask) {
    const auto names = kasumi::feature_names(bitmask);
    output::out(destination, "{\"bitmask\":", bitmask, ",\"names\":");
    print_string_array(destination, names);
    output::out(destination, "}");
    return 0;
}
}  // namespace

#if !defined(KAGAMI_EMBEDDED)
namespace {
void print_lkm_unload_status_json(output::Context& destination) {
    const auto unload = lkm::unload_status();
    const auto& quiesce = unload.quiesce;
    output::out(
        destination, "{", "\"attempted\":", (unload.attempted ? "true" : "false"), ",",
        "\"quiesce_supported\":", (unload.quiesce_supported ? "true" : "false"), ",",
        "\"capability_errno\":", unload.capability_errno, ",",
        "\"delete_errno\":", unload.delete_errno, ",", "\"quiesce\":{",
        "\"ok\":", (quiesce.ok ? "true" : "false"), ",", "\"errno\":", quiesce.last_errno, ",",
        "\"version\":", quiesce.version, ",", "\"size\":", quiesce.size, ",",
        "\"flags\":", quiesce.flags, ",", "\"state\":", static_cast<std::uint32_t>(quiesce.state),
        ",", "\"busy_mask\":", quiesce.busy_mask, ",", "\"pending_getfd\":", quiesce.pending_getfd,
        ",", "\"pending_marker\":", quiesce.pending_marker, ",",
        "\"pending_redirect\":", quiesce.pending_redirect, ",",
        "\"live_proc_proxy\":", quiesce.live_proc_proxy, ",",
        "\"live_file_view\":", quiesce.live_file_view, ",",
        "\"control_files\":", quiesce.control_files, ",", "\"module_refs\":", quiesce.module_refs,
        ",", "\"err\":", quiesce.err, "}}");
}
}  // namespace

#endif

namespace {
const char* policy_owner_name(kasumi::PolicyOwner owner) {
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

bool parse_policy_owner(const std::string& value, kasumi::PolicyOwner& owner) {
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

std::string policy_uid_list_name(kasumi::PolicyUidList list) {
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

bool parse_policy_uid_list(const std::string& value, kasumi::PolicyUidList& list) {
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
}  // namespace

namespace {
template <typename Mutation>
bool mutate_policy_preserving_enabled(output::Context& destination,
                                      const kasumi::PolicyState& state,
                                      const std::string& operation, const Mutation& mutation) {
    if (state.enabled && !kasumi::set_enabled(false)) {
        output::err(destination,
                    "failed to disable Kasumi before policy update: ", std::strerror(errno), "\n");
        return false;
    }

    const bool changed = mutation();
    const int mutation_errno = errno;
    if (state.enabled && !kasumi::set_enabled(true)) {
        const int restore_errno = errno;
        if (!changed) {
            output::err(destination, "failed to ", operation, ": ", std::strerror(mutation_errno),
                        "\n");
        }
        output::err(destination, "failed to re-enable Kasumi after policy update: ",
                    std::strerror(restore_errno), "\n");
        errno = restore_errno;
        return false;
    }
    if (!changed) {
        errno = mutation_errno;
        output::err(destination, "failed to ", operation, ": ", std::strerror(mutation_errno),
                    "\n");
        return false;
    }
    return true;
}

std::uint32_t parse_policy_flags(Arguments args, std::size_t start, std::uint32_t fallback) {
    if (start >= args.size()) {
        return fallback;
    }
    std::uint32_t flags = 0;
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string_view value = args[i];
        if (value == "allow" || value == "allow-uids") {
            flags |= KSM_POLICY_FLAG_USE_ALLOW_UIDS;
        } else if (value == "deny" || value == "deny-uids") {
            flags |= KSM_POLICY_FLAG_USE_DENY_UIDS;
        } else if (value == "isolated" || value == "include-isolated") {
            flags |= KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS;
        } else if (value.rfind("0x", 0) == 0) {
            flags |= static_cast<std::uint32_t>(std::strtoul(args[i].c_str(), nullptr, 0));
        }
    }
    return flags;
}

bool parse_uid_values(Arguments args, std::size_t start, std::vector<std::uint32_t>& uids) {
    uids.clear();
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string_view text = args[i];
        unsigned long value;
        errno = 0;
        if (!parse_unsigned(text.substr(0, text.find('\0')), value) || value > UINT32_MAX)
            return false;
        uids.push_back(static_cast<std::uint32_t>(value));
    }
    return true;
}

bool prepare_uid_csv(std::string_view value, bounded::Array<std::uint32_t>& storage,
                     bounded::Span<std::uint32_t>& uids) noexcept {
    if (value.empty() || value == "-") {
        uids = {};
        return true;
    }
    const auto capacity = static_cast<std::size_t>(std::count(value.begin(), value.end(), ',')) + 1;
    if (!storage.prepare_array(capacity, 65536)) {
        errno = ENOMEM;
        return false;
    }
    auto remaining = value;
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const auto item = remaining.substr(0, comma);
        unsigned long uid;
        if (!parse_unsigned(item.substr(0, item.find('\0')), uid, 10) || uid == 0 ||
            uid > UINT32_MAX || !storage.append(static_cast<std::uint32_t>(uid)))
            return false;
        if (comma == std::string_view::npos)
            break;
        remaining.remove_prefix(comma + 1);
    }
    auto* first = storage.data();
    auto* last = first + storage.size();
    std::sort(first, last);
    const auto* unique = std::unique(first, last);
    uids = {first, static_cast<std::size_t>(unique - first)};
    return true;
}

void print_roots_json(output::Context& destination, std::uint32_t roots) {
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
    output::out(destination, "{\"bitmask\":", roots, ",\"names\":");
    print_string_array(destination, names);
    output::out(destination, "}");
}

void print_policy_flags_json(output::Context& destination, std::uint32_t flags) {
    output::out(destination, "{", "\"bitmask\":", flags, ",", "\"use_allow_uids\":",
                ((flags & KSM_POLICY_FLAG_USE_ALLOW_UIDS) ? "true" : "false"), ",",
                "\"use_deny_uids\":", ((flags & KSM_POLICY_FLAG_USE_DENY_UIDS) ? "true" : "false"),
                ",", "\"include_isolated_uids\":",
                ((flags & KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS) ? "true" : "false"), "}");
}

int print_policy_json(output::Context& destination) {
    const auto snapshot = kasumi::policy_snapshot();
    const auto& state = snapshot.state;

    output::out(destination, "{", "\"ok\":", (state.ok ? "true" : "false"), ",",
                "\"errno\":", state.last_errno, ",", "\"err\":", state.err, ",",
                "\"api_version\":", state.version, ",", "\"generation\":", state.generation, ",",
                "\"enabled\":", (state.enabled ? "true" : "false"), ",",
                "\"owner\":", bounded::quoted(policy_owner_name(state.owner)), ",",
                "\"effective_owner\":", bounded::quoted(policy_owner_name(state.effective_owner)),
                ",", "\"flags\":");
    print_policy_flags_json(destination, state.flags);
    output::out(destination, ",\"detected_roots\":");
    print_roots_json(destination, state.detected_roots);
    output::out(destination, ",\"allow_count\":", state.allow_count,
                ",\"deny_count\":", state.deny_count, ",\"max_uid_count\":", state.max_uid_count,
                ",\"allow_uids\":");
    print_u32_array(destination, snapshot.allow_uids);
    output::out(destination, ",\"deny_uids\":");
    print_u32_array(destination, snapshot.deny_uids);
    output::out(destination, "}\n");
    return state.ok ? 0 : 1;
}

int print_kasumi_snapshot_json(output::Context& destination) {
    const auto version = kasumi::version_info();
    const bool available = version.status == kasumi::Status::Available;
    const int bitmask = available ? kasumi::features() : 0;
    const std::string hook_text = available ? kasumi::hooks() : "";
    const auto policy_snapshot = available ? kasumi::policy_snapshot() : kasumi::PolicySnapshot{};
    const auto& policy = policy_snapshot.state;
    const auto config = current_config();

    output::out(destination, "{", "\"transport\":\"daemon\",",
                "\"available\":", (available ? "true" : "false"), ",",
                "\"status\":", static_cast<int>(version.status), ",",
                "\"expected_protocol\":", version.expected_protocol, ",",
                "\"kernel_protocol\":", version.kernel_protocol, ",",
                "\"uid\":", version.process_uid, ",", "\"euid\":", version.process_euid, ",",
                "\"last_errno\":", version.last_errno, ",", "\"last_error\":",
                bounded::quoted(version.last_errno == 0 ? "" : std::strerror(version.last_errno)),
                ",", "\"modules_visible\":", (version.modules_visible ? "true" : "false"), ",",
                "\"mount_base\":", bounded::quoted(mount::storage::current_mirror_dir(config)), ",",
                "\"features\":{\"bitmask\":", bitmask, ",\"names\":");
    print_string_array(destination, kasumi::feature_names(bitmask));
    output::out(destination, "},\"hooks\":", bounded::quoted(hook_text), ",", "\"policy\":{",
                "\"ok\":", (policy.ok ? "true" : "false"), ",", "\"errno\":", policy.last_errno,
                ",", "\"err\":", policy.err, ",", "\"api_version\":", policy.version, ",",
                "\"generation\":", policy.generation, ",",
                "\"enabled\":", (policy.enabled ? "true" : "false"), ",",
                "\"owner\":", bounded::quoted(policy_owner_name(policy.owner)), ",",
                "\"effective_owner\":", bounded::quoted(policy_owner_name(policy.effective_owner)),
                ",", "\"flags\":", policy.flags, ",", "\"detected_roots\":", policy.detected_roots,
                ",", "\"allow_count\":", policy.allow_count, ",",
                "\"deny_count\":", policy.deny_count, ",",
                "\"max_uid_count\":", policy.max_uid_count, ",", "\"allow_uids\":");
    print_u32_array(destination, policy_snapshot.allow_uids);
    output::out(destination, ",\"deny_uids\":");
    print_u32_array(destination, policy_snapshot.deny_uids);
    output::out(destination, "}}\n");
    return 0;
}

int apply_config_file(output::Context& destination, const fs::path& path,
                      bool kernel_state_lost = false, bool force_enable = false) {
#if defined(KAGAMI_EMBEDDED)
    if (path.lexically_normal() != config_file().lexically_normal()) {
        errno = EINVAL;
        output::err(destination, "Embedded configuration must stay in ", config_file(), '\n');
        return 1;
    }
#endif
    PreparedConfig config;
    std::string error;
    if (!read_config_file(path.string(), config, error)) {
        output::err(destination, error, "\n");
        return 1;
    }
    if (force_enable) {
        config.kasumi_enabled = true;
    }
    if (!config.kasumi_enabled) {
        if ((kasumi::module_loaded() || kasumi::is_available()) &&
            !mount::kasumi::deactivate(error)) {
            output::err(destination, error, "\n");
            return 1;
        }
        return 0;
    }
    if (!kasumi::is_available()) {
        output::err(destination, "Kasumi is unavailable\n");
        return 1;
    }

    // Rebuild module mappings only when this boot already established that
    // Kasumi owns them. A first post-boot load may follow an Overlay/Magic
    // fallback and must not migrate or stack backends underneath live mounts.
    if (mount::kasumi::has_replayable_mappings() &&
        (kernel_state_lost || !mount::kasumi::is_active())) {
        if (!mount::refresh_kasumi_modules(config)) {
            output::err(destination, "failed to replay persisted Kasumi configuration\n");
            return 1;
        }
        return print_policy_json(destination);
    }

    if (!mount::kasumi::apply_policy_config(config.policy, error)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        output::err(destination, error, "\n");
        return 1;
    }
    if (!mount::kasumi::restore_persisted_hide_rules(error)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        output::err(destination, error, "\n");
        return 1;
    }
    if (!mount::kasumi::apply_feature_config(config, error)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        output::err(destination, error, "\n");
        return 1;
    }
    if (!mount::fsutil::run_in_init_mount_ns(
            [&]() { return mount::overlay::restore_xattr_hiding(config); })) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        output::err(destination, "failed to restore OverlayFS xattr hiding\n");
        return 1;
    }
    if (!kasumi::set_enabled(true)) {
        std::string cleanup_error;
        (void)mount::kasumi::disable_control_state(cleanup_error);
        output::err(destination, "failed to enable Kasumi after config apply\n");
        return 1;
    }
    return print_policy_json(destination);
}

void print_backend_statuses_json(output::Context& destination,
                                 const std::vector<mount::BackendStatus>& statuses) {
    output::out(destination, "[");
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto& status = statuses[i];
        if (i > 0) {
            output::out(destination, ",");
        }
        output::out(destination, "{",
                    "\"kind\":", bounded::quoted(mount::backend_kind_name(status.kind)), ",",
                    "\"name\":", bounded::quoted(status.name), ",",
                    "\"available\":", (status.available ? "true" : "false"), ",",
                    "\"preferred\":", (status.preferred ? "true" : "false"), ",",
                    "\"detail\":", bounded::quoted(status.detail), "}");
    }
    output::out(destination, "]");
}

void print_backend_statuses_json(output::Context& destination) {
    print_backend_statuses_json(destination, mount::backend_statuses());
}

int print_system_json(output::Context& destination) {
    const auto version = kasumi::version_info();
    const int bitmask = version.status == kasumi::Status::Available ? kasumi::features() : 0;
    const std::string hook_text =
        version.status == kasumi::Status::Available ? kasumi::hooks() : "";

    // Count our live mounts (our mount source) for the stats panel.
    const auto config = current_config();
    auto owned_paths = mount::magic::active_mounts(config);
    // The legacy journal's ownership filter historically used the default
    // source in backend_statuses(). Preserve it for a non-default source.
    const auto magic_count = config.mount_source == Config{}.mount_source
                                 ? owned_paths.size()
                                 : mount::magic::active_mounts(Config{}).size();
    auto overlay_paths = mount::overlay::active_mounts(config);
    std::sort(owned_paths.begin(), owned_paths.end());
    std::sort(overlay_paths.begin(), overlay_paths.end());
    mount_table::Table mounts;
    int error = 0;
    if (!mounts.prepare_file("/proc/self/mountinfo", error)) {
        output::err(destination, "mount table read failed: ", std::strerror(error), '\n');
        return 1;
    }
    int total_mounts = 0;
    int overlay_mounts = 0;
    mounts.consume([&](const mount_table::Entry& m) noexcept {
        if (m.source == config.mount_source ||
            std::binary_search(owned_paths.begin(), owned_paths.end(), m.mount_point) ||
            std::binary_search(overlay_paths.begin(), overlay_paths.end(), m.mount_point)) {
            ++total_mounts;
            if (m.fstype == "overlay")
                ++overlay_mounts;
        }
        return true;
    });

    PreparedInput kernel;
    if (!kernel.prepare_file("/proc/version", error)) {
        output::err(destination, "kernel version read failed: ", std::strerror(error), '\n');
        return 1;
    }
    output::out(
        destination, "{", "\"kernel\":", bounded::quoted(kernel_release(kernel.view())), ",",
        "\"selinux\":", bounded::quoted(selinux_status()), ",",
        "\"mount_base\":", bounded::quoted(mount::storage::current_mirror_dir(config)), ",",
        "\"kasumi_available\":", (version.status == kasumi::Status::Available ? "true" : "false"),
        ",", "\"kasumi_status\":", static_cast<int>(version.status), ",",
        "\"hooks\":", bounded::quoted(hook_text), ",", "\"features\":");
    print_features_json(destination, bitmask);
    output::out(destination, ",\"mountStats\":{\"total_mounts\":", total_mounts,
                ",\"successful_mounts\":", total_mounts,
                ",\"failed_mounts\":0,\"tmpfs_created\":0,\"files_mounted\":0,\"dirs_mounted\":0,",
                "\"symlinks_created\":0,\"overlayfs_mounts\":", overlay_mounts,
                ",\"success_rate\":", (total_mounts > 0 ? 100 : 0), "},",
                "\"detectedPartitions\":");
    if (print_partitions_json(destination) != 0)
        return 1;
    output::out(destination, ",\"backends\":");
    print_backend_statuses_json(destination,
                                mount::backend_statuses(version, static_cast<int>(magic_count)));
    output::out(destination, "}\n");
    return 0;
}

int print_meta_json(output::Context& destination) {
    output::out(destination, "{", "\"module_id\":\"kagami\",", "\"metamodule\":true,",
                "\"version\":", bounded::quoted(KAGAMI_VERSION), ",",
                "\"data_dir\":", bounded::quoted(runtime_data_dir().string()), ",",
                "\"modules_dir\":", bounded::quoted(runtime_modules_dir().string()), ",",
                "\"config_file\":", bounded::quoted(runtime_config_file().string()), ",",
                "\"socket\":", bounded::quoted(runtime_socket_file().string()), ",",
                "\"pid_file\":", bounded::quoted(runtime_pid_file().string()), ",",
                "\"log_file\":", bounded::quoted(runtime_log_file().string()), ",",
                "\"backends\":");
    print_backend_statuses_json(destination);
    output::out(destination, "}\n");
    return 0;
}

int print_kasumi_version_json(output::Context& destination) {
    const auto version = kasumi::version_info();
    const std::string rules =
        version.status == kasumi::Status::Available ? kasumi::active_rules() : "";
    const auto modules = kasumi::active_modules_from_rules(rules);
    const bool mismatch = version.status == kasumi::Status::KernelTooOld ||
                          version.status == kasumi::Status::ClientTooOld;

    const auto config = current_config();
    output::out(
        destination, "{", "\"backend\":\"kasumi\",",
        "\"protocol_version\":", version.expected_protocol, ",",
        "\"kernel_version\":", version.kernel_protocol, ",",
        "\"kasumi_available\":", (version.status == kasumi::Status::Available ? "true" : "false"),
        ",", "\"protocol_mismatch\":", (mismatch ? "true" : "false"), ",",
        "\"mismatch_message\":", bounded::quoted(mismatch ? "Kasumi protocol mismatch" : ""), ",",
        "\"active_modules\":");
    print_string_array(destination, modules);
    output::out(destination,
                ",\"mount_base\":", bounded::quoted(mount::storage::current_mirror_dir(config)),
                "}\n");
    return 0;
}

int print_kasumi_rules_json(output::Context& destination) {
    const auto rules = kasumi::read_active_rules();
    if (!rules.ok) {
        errno = rules.last_errno != 0 ? rules.last_errno : EIO;
        output::err(destination, "failed to read complete Kasumi rules: ", std::strerror(errno),
                    '\n');
        return 1;
    }
    std::istringstream lines(rules.text);
    std::string line;
    bool first = true;

    output::out(destination, "[");
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream parts(line);
        std::string type;
        parts >> type;
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (!first) {
            output::out(destination, ",");
        }
        first = false;

        output::out(destination, "{\"type\":", bounded::quoted(type));
        if (type == "ADD" || type == "MERGE") {
            std::string target;
            std::string source;
            parts >> target >> source;
            output::out(destination, ",\"target\":", bounded::quoted(target),
                        ",\"source\":", bounded::quoted(source));
        } else if (type == "HIDE") {
            std::string path;
            parts >> path;
            output::out(destination, ",\"path\":", bounded::quoted(path));
        } else {
            std::string rest;
            std::getline(parts, rest);
            if (!rest.empty() && rest[0] == ' ') {
                rest.erase(0, 1);
            }
            output::out(destination, ",\"args\":", bounded::quoted(rest));
        }
        output::out(destination, "}");
    }
    output::out(destination, "]\n");
    return 0;
}

int handle_config(output::Context& destination, Arguments args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "show") {
        PreparedInput input;
        std::string error;
        if (!read_config_json(config_file().string(), input, error, true)) {
            output::err(destination, error, '\n');
            return 1;
        }
        // Validation preserves the original bytes. Insert probed fields only
        // while writing the final output; do not shift or duplicate the input.
        const auto config = input.view();
        const auto brace = config.rfind('}');
        output::out(destination, config.substr(0, brace), ",\"tmpfs_xattr_supported\":",
                    mount::fsutil::tmpfs_xattr_supported() ? "true" : "false",
                    ",\"kasumi_available\":", kasumi::is_available() ? "true" : "false",
                    config.substr(brace));
        return 0;
    }
    if (sub == "gen") {
        std::string output = config_file().string();
        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "-o" || args[i] == "--output") {
                output = std::string_view(args[i + 1]);
            }
        }
        std::string error;
#if defined(KAGAMI_EMBEDDED)
        if (fs::path(output).lexically_normal() != config_file().lexically_normal()) {
            errno = EINVAL;
            output::err(destination, "Embedded configuration must stay in ", config_file(), '\n');
            return 1;
        }
#endif
        if (!write_default_config(output, error)) {
            output::err(destination, error, "\n");
            return 1;
        }
        return 0;
    }
    if (sub == "merge-json") {
        if (args.size() != 3) {
            output::err(destination, "usage: kagamid config merge-json JSON\n");
            return 1;
        }
        std::string error;
        if (!merge_config_json(config_file().string(), std::string(std::string_view(args[2])),
                               error)) {
            output::err(destination, error, "\n");
            return 1;
        }
        return 0;
    }
    if (sub == "apply") {
        return apply_config_file(destination, arg_or_default(args, 2, config_file().string()));
    }
    if (sub == "policy") {
        if (arg_or_default(args, 2, "") != "set" || args.size() != 7) {
            output::err(destination,
                        "usage: kagamid config policy set <owner> <allow-csv|-> <deny-csv|-> "
                        "<isolated:on|off>\n");
            return 1;
        }
        kasumi::PolicyOwner parsed_owner = kasumi::PolicyOwner::Auto;
        if (!parse_policy_owner(std::string(std::string_view(args[3])), parsed_owner)) {
            output::err(destination, "policy owner must be auto|kernelsu|apatch|manual|disabled\n");
            return 1;
        }
        PolicyConfig policy;
        policy.owner = policy_owner_name(parsed_owner);
        bounded::Array<std::uint32_t> allow_storage;
        bounded::Array<std::uint32_t> deny_storage;
        if (!prepare_uid_csv(args[4], allow_storage, policy.allow_uids) ||
            !prepare_uid_csv(args[5], deny_storage, policy.deny_uids)) {
            output::err(
                destination,
                "policy UID values must be positive decimal integers separated by commas\n");
            return 1;
        }
        const std::string_view isolated = args[6];
        if (isolated != "on" && isolated != "off") {
            output::err(destination, "policy isolated value must be on|off\n");
            return 1;
        }
        policy.use_allow_uids = !policy.allow_uids.empty();
        policy.use_deny_uids = !policy.deny_uids.empty();
        policy.include_isolated_uids = isolated == "on";
        std::string error;
        if (!update_policy_config(config_file().string(), policy, error)) {
            output::err(destination, error, "\n");
            return 1;
        }
        output::out(destination, "{\"ok\":true,\"persisted\":true}\n");
        return 0;
    }
    if (sub == "sync-partitions") {
        std::set<std::string> partitions;
        const fs::path module_root = modules_dir();
        std::error_code scan_error;
        if (fs::is_directory(module_root, scan_error)) {
            fs::directory_iterator module(module_root, scan_error);
            const fs::directory_iterator end;
            for (; !scan_error && module != end; module.increment(scan_error)) {
                if (!module->is_directory(scan_error))
                    continue;
                fs::directory_iterator child(module->path(), scan_error);
                for (; !scan_error && child != end; child.increment(scan_error)) {
                    if (child->is_directory(scan_error)) {
                        const std::string name = child->path().filename().string();
                        if (!is_builtin_partition(name) &&
                            module_has_partition_content(module->path(), name)) {
                            partitions.insert(name);
                        }
                    }
                }
                if (scan_error)
                    break;
            }
        }
        if (scan_error && scan_error != std::errc::no_such_file_or_directory) {
            output::err(destination, "partition scan failed: ", scan_error.message(), "\n");
            return 1;
        }
        if (partitions.empty()) {
            output::out(destination, "No new partitions\n");
        } else {
            for (const auto& partition : partitions) {
                output::out(destination, "Added partition: ", partition, "\n");
            }
        }
        return 0;
    }
    print_usage(destination);
    return 1;
}

int handle_api(output::Context& destination, Arguments args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "system") {
        return print_system_json(destination);
    }
    if (sub == "storage") {
        return print_storage_json(destination);
    }
    if (sub == "lkm") {
#if defined(KAGAMI_EMBEDDED)
        output::out(destination,
                    "{\"builtin\":true,\"available\":", (kasumi::is_available() ? "true" : "false"),
                    ",\"external_management\":false}\n");
        return 0;
#else
        const auto version = kasumi::version_info();
        const bool loaded = lkm::is_loaded();
        const bool builtin = version.status == kasumi::Status::Available && !loaded;
        output::out(destination, "{\"loaded\":", (loaded ? "true" : "false"),
                    ",\"builtin\":", (builtin ? "true" : "false"),
                    ",\"autoload\":", (lkm::get_autoload() ? "true" : "false"),
                    ",\"kmi_override\":", bounded::quoted(lkm::get_kmi_override()),
                    ",\"detected_kmi\":", bounded::quoted(lkm::current_kmi()),
                    ",\"asset\":", bounded::quoted(lkm::find_asset()),
                    ",\"last_error\":", bounded::quoted(lkm::last_error()), ",\"unload\":");
        print_lkm_unload_status_json(destination);
        output::out(destination, "}\n");
        return 0;
#endif
    }
    if (sub == "kasumi") {
        return print_kasumi_snapshot_json(destination);
    }
    if (sub == "features") {
        const auto version = kasumi::version_info();
        return print_features_json(
            destination, version.status == kasumi::Status::Available ? kasumi::features() : 0);
    }
    if (sub == "hooks") {
        if (!kasumi::is_available()) {
            output::err(destination, "Kasumi not available.\n");
            return 1;
        }
        output::out(destination, kasumi::hooks(), "\n");
        return 0;
    }
    if (sub == "policy") {
        return print_policy_json(destination);
    }
    if (sub == "backends") {
        print_backend_statuses_json(destination);
        output::out(destination, "\n");
        return 0;
    }
    if (sub == "meta") {
        return print_meta_json(destination);
    }
    print_usage(destination);
    return 1;
}

int handle_module(output::Context& destination, Arguments args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "list") {
        const bool include_inactive =
            std::find(args.begin() + 2, args.end(), "--include-inactive") != args.end();
        int settings_error = 0;
        const auto modes = mount::load_module_modes(settings_error);
        if (settings_error) {
            output::err(destination, "cannot read module modes: ", std::strerror(settings_error),
                        '\n');
            return 1;
        }
        const auto rule_map = mount::load_module_rules(settings_error);
        if (settings_error) {
            output::err(destination, "cannot read module rules: ", std::strerror(settings_error),
                        '\n');
            return 1;
        }
        const auto replayable = mount::kasumi::replayable_module_ids();
        const auto cfg = current_config();
        const fs::path module_root =
            cfg.module_dir.empty() ? modules_dir() : fs::path(std::string_view(cfg.module_dir));
        const auto snapshot = mount::module_runtime::read_snapshot_observed([]() {
            mount::module_runtime::KasumiObservation observation;
            const auto rules = kasumi::read_active_rules();
            observation.ok = rules.ok;
            observation.enabled = rules.enabled;
            observation.text = rules.text;
            if (!rules.ok) {
                observation.error_code = rules.last_errno == EOVERFLOW ? "kasumi_rules_truncated"
                                                                       : "kasumi_rules_unreadable";
                observation.error = std::strerror(rules.last_errno != 0 ? rules.last_errno : EIO);
            }
            return observation;
        });
        mount::ModuleInventory candidates;
        int inventory_error = 0;
        if (!candidates.prepare_inventory(module_root.c_str(),
                                          include_inactive ? snapshot.modules.size() : 0,
                                          inventory_error) ||
            !candidates.read_directory(inventory_error)) {
            errno = inventory_error;
            output::err(destination,
                        "failed to read module directory: ", std::strerror(inventory_error), '\n');
            return 1;
        }
        if (include_inactive) {
            for (const auto& [id, runtime] : snapshot.modules) {
                if (runtime.has_contributions && valid_module_id(id) &&
                    !candidates.append_runtime(id)) {
                    errno = ENOBUFS;
                    output::err(destination, "module inventory exceeds prepared capacity\n");
                    return 1;
                }
            }
        }
        candidates.sort();
        for (const auto& id : replayable)
            candidates.mark_kasumi(id);
        output::out(destination, "{\"modules\":[");
        mount::BackendAvailability availability;
        mount::ModuleScanner scanner;
        if (candidates.begin() != candidates.end() && !scanner.prepare_scan(cfg)) {
            errno = ENOMEM;
            output::err(destination, "cannot prepare module inventory storage\n");
            return 1;
        }
        const posix::Fd hot_markers(
            open((data_dir() / "run" / "hot_unmounted").c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
        char module_path_bytes[PATH_MAX];
        bounded::Writer module_path(module_path_bytes, sizeof(module_path_bytes));
        bool first = true;
        for (const auto& candidate : candidates) {
            const std::string_view id = candidate.id;
            module_path.reset();
            if (!module_path.write(module_root.native(),
                                   module_root.native().back() == '/' ? "" : "/", id, '\0')) {
                errno = ENAMETOOLONG;
                output::err(destination, "module path exceeds filesystem capacity\n");
                return 1;
            }
            const auto mode = modes.find(id);
            const auto plan = candidate.kasumi_boot
                                  ? mount::BackendPlan{"kasumi", "kasumi", false}
                                  : mount::prepare_backend_plan(cfg, mode, availability, false);
            const auto recorded = snapshot.modules.find(id);
            const bool has_runtime = recorded != snapshot.modules.end();
            const bool retain_runtime =
                include_inactive && has_runtime && recorded->second.has_contributions;
            mount::ModuleFacts facts;
            mount::ModuleProperties props;
            int scan_error = 0;
            if (!scanner.observe(module_path_bytes, facts, props, scan_error,
                                 !candidate.kasumi_boot && plan.inspect_root_files, false)) {
                errno = scan_error;
                output::err(destination, "failed to inspect module ", id, ": ",
                            std::strerror(scan_error), '\n');
                return 1;
            }
            // fstatat initializes every field consumed, and failure never reads it.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
            struct stat marker;
            const bool inactive =
                facts.inactive ||
                (hot_markers.get() >= 0 &&
                 fstatat(hot_markers.get(), candidate.id.c_str(), &marker, 0) == 0);
            if ((!include_inactive && inactive) || (!facts.has_prop && !retain_runtime) ||
                (!facts.visible_content && !retain_runtime))
                continue;
            if (!scanner.observed_properties(props, scan_error)) {
                errno = scan_error;
                output::err(destination,
                            "failed to read module properties: ", std::strerror(scan_error), '\n');
                return 1;
            }
            const auto module_rules = rule_map.find(id);
            const std::string_view strategy = candidate.kasumi_boot ? "kasumi" : plan.select(facts);
            if (!first) {
                output::out(destination, ',');
            }
            first = false;
            output::out(destination, "{\"id\":", bounded::quoted(id), ",\"name\":",
                        bounded::quoted(props.has_name ? props.name : std::string_view(id)),
                        ",\"version\":", bounded::quoted(props.version),
                        ",\"author\":", bounded::quoted(props.author),
                        ",\"description\":", bounded::quoted(props.description),
                        ",\"mode\":", bounded::quoted(mode),
                        ",\"strategy\":", bounded::quoted(strategy), ",\"runtime\":");
            const auto& summary = has_runtime ? recorded->second : snapshot.missing;
            output::out(destination, "{\"backend\":");
            if (summary.backend.empty())
                output::out(destination, "null");
            else
                output::out(destination, bounded::json_quoted(summary.backend));
            output::out(destination, ",\"error\":");
            if (summary.error.empty() && summary.error_code.empty())
                output::out(destination, "null");
            else
                output::out(destination, "{\"code\":", bounded::json_quoted(summary.error_code),
                            ",\"message\":", bounded::json_quoted(summary.error), '}');
            output::out(destination, ",\"reason\":");
            if (summary.reason.empty())
                output::out(destination, "null");
            else
                output::out(destination, bounded::json_quoted(summary.reason));
            output::out(
                destination, ",\"state\":", bounded::json_quoted(summary.state), '}', ",\"path\":",
                bounded::quoted(module_path.view().substr(0, module_path.view().size() - 1)),
                ",\"rules\":[");
            if (!module_rules.empty()) {
                for (std::size_t i = 0; i < module_rules.size(); ++i) {
                    if (i > 0) {
                        output::out(destination, ',');
                    }
                    output::out(destination, "{\"path\":", bounded::quoted(module_rules[i].path),
                                ",\"mode\":", bounded::quoted(module_rules[i].mode), '}');
                }
            }
            output::out(destination, "]}");
        }
        output::out(destination, "]}\n");
        return 0;
    }
    if (sub == "set-mode") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string mode = arg_or_default(args, 3, "");
        if (!valid_module_id(id) || (mode != "auto" && mode != "kasumi" && mode != "overlay" &&
                                     mode != "magic" && mode != "none")) {
            output::err(destination,
                        "usage: kagamid module set-mode <id> auto|kasumi|overlay|magic|none\n");
            return 1;
        }
        int settings_error = 0;
        if (!mount::update_module_mode(id, mode, settings_error)) {
            errno = settings_error;
            output::err(destination, "failed to save module modes\n");
            return 1;
        }
        return 0;
    }
    if (sub == "add-rule") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string path = arg_or_default(args, 3, "");
        const std::string mode = arg_or_default(args, 4, "");
        if (!valid_module_id(id) || path.empty() || path.front() != '/' ||
            !valid_module_mode(mode)) {
            output::err(destination, "usage: kagamid module add-rule <id> <absolute-path> "
                                     "kasumi|overlay|magic|none|hide\n");
            return 1;
        }
        int settings_error = 0;
        if (!mount::update_module_rule(id, path, mode, false, settings_error)) {
            errno = settings_error;
            output::err(destination, "failed to save module rules\n");
            return 1;
        }
        return 0;
    }
    if (sub == "remove-rule") {
        const std::string id = arg_or_default(args, 2, "");
        const std::string path = arg_or_default(args, 3, "");
        if (!valid_module_id(id) || path.empty() || path.front() != '/') {
            output::err(destination, "usage: kagamid module remove-rule <id> <absolute-path>\n");
            return 1;
        }
        int settings_error = 0;
        if (!mount::update_module_rule(id, path, {}, true, settings_error)) {
            errno = settings_error;
            output::err(destination, "failed to save module rules\n");
            return 1;
        }
        return 0;
    }
    if (sub == "hot-mount" || sub == "hot-unmount" || sub == "add" || sub == "delete") {
        const std::string id = arg_or_default(args, 2, "");
        if (!valid_module_id(id)) {
            output::err(destination, "usage: kagamid module ", sub, " <id>\n");
            return 1;
        }
        const auto cfg = current_config();
        const fs::path module_root =
            cfg.module_dir.empty() ? modules_dir() : fs::path(std::string_view(cfg.module_dir));
        const fs::path module_path = module_root / id;
        std::error_code module_error;
        if (!fs::is_directory(module_path, module_error) ||
            !fs::exists(module_path / "module.prop", module_error)) {
            output::err(destination, "module not found: ", id, "\n");
            return 1;
        }
        const auto replayable = mount::kasumi::replayable_module_ids();
        if (std::find(replayable.begin(), replayable.end(), id) == replayable.end()) {
            output::err(
                destination,
                "module was not assigned to Kasumi this boot; reboot to change its backend\n");
            return 1;
        }
        const fs::path marker = data_dir() / "run" / "hot_unmounted" / id;
        std::error_code ec;
        const bool unmounting = sub == "hot-unmount" || sub == "delete";
        const bool existed = fs::exists(marker, ec);
        fs::create_directories(marker.parent_path(), ec);
        if (ec) {
            output::err(destination, "failed to prepare hot-mount state: ", ec.message(), "\n");
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
            output::err(destination, "failed to refresh Kasumi mappings\n");
            return 1;
        }
        output::out(destination, "{\"ok\":true,\"module\":", bounded::quoted(id),
                    ",\"action\":", bounded::quoted(sub), "}\n");
        return 0;
    }
    if (sub == "check-conflicts") {
        const auto cfg = current_config();
        int enumeration_error = 0;
        const auto modules = mount::enumerate_mountable_modules(cfg, enumeration_error);
        if (enumeration_error) {
            output::err(destination,
                        "module enumeration failed: ", std::strerror(enumeration_error), '\n');
            return 1;
        }
        const Arguments parts =
            cfg.partitions.empty() ? mount::fsutil::managed_partitions() : cfg.partitions;
        std::map<std::string, std::vector<std::string>> owners;
        for (const auto& module : modules) {
            for (const auto& part : parts) {
                const fs::path root = module.path / std::string_view(part);
                std::error_code ec;
                auto it = fs::recursive_directory_iterator(root, ec);
                const auto end = fs::recursive_directory_iterator();
                for (; it != end && !ec; it.increment(ec)) {
                    if (it->is_regular_file(ec) || it->is_symlink(ec)) {
                        owners[(fs::path("/") / std::string_view(part) /
                                it->path().lexically_relative(root))
                                   .string()]
                            .push_back(module.id);
                    }
                }
            }
        }
        output::out(destination, "[");
        bool first = true;
        for (const auto& [path, ids] : owners) {
            if (ids.size() < 2) {
                continue;
            }
            if (!first) {
                output::out(destination, ",");
            }
            first = false;
            output::out(destination, "{\"file\":", bounded::quoted(path), ",\"modules\":");
            print_string_array(destination, ids);
            output::out(destination, "}");
        }
        output::out(destination, "]\n");
        return 0;
    }
    if (sub == "mount-all") {
        // Boot-only entry (the metamodule metamount.sh hook). Refuse once the
        // device has booted: magic mount over the live system post-boot breaks
        // mount namespaces (it propagates into adbd / service namespaces).
        if (system_boot_completed() && !runtime_mount_here()) {
            output::err(destination, "refusing module mount-all: magic mount only runs at boot via "
                                     "metamount.sh; post-boot mounting breaks namespaces\n");
            return 1;
        }
        PreparedConfig config;
        std::string cfg_err;
        read_config_file(config_file().string(), config, cfg_err);  // defaults on error
        const auto report = mount::mount_all_enabled(config);
        output::out(destination, "{", "\"ok\":", (report.ok ? "true" : "false"), ",",
                    "\"backend\":", bounded::quoted(report.backend), ",",
                    "\"modules\":", report.modules, ",", "\"mounts\":", report.mounts, ",",
                    "\"detail\":", bounded::quoted(report.detail), "}\n");
        return report.ok ? 0 : 1;
    }
    if (sub == "normalize") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty()) {
            output::err(destination, "usage: kagamid module normalize <module_path>\n");
            return 1;
        }
        const bool ok = mount::magic::normalize_module(path);
        output::out(destination, "{\"ok\":", (ok ? "true" : "false"),
                    ",\"module\":", bounded::quoted(path), "}\n");
        return ok ? 0 : 1;
    }
    if (sub == "unmount") {
        // Tear down Kagami's own mounts (source-gated; never touches real partitions).
        PreparedConfig config;
        std::string cfg_err;
        read_config_file(config_file().string(), config, cfg_err);  // defaults on error
        const bool ok = mount::unmount_all(config);
        output::out(destination, "{\"ok\":", (ok ? "true" : "false"), "}\n");
        return ok ? 0 : 1;
    }
    print_usage(destination);
    return 1;
}

int handle_kasumi(output::Context& destination, Arguments args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "version") {
        return print_kasumi_version_json(destination);
    }
    if (sub == "list") {
        return print_kasumi_rules_json(destination);
    }
    if (sub == "features") {
        const auto version = kasumi::version_info();
        return print_features_json(
            destination, version.status == kasumi::Status::Available ? kasumi::features() : 0);
    }
    if (sub == "maps") {
        const std::string op = arg_or_default(args, 2, "");
        if (op == "clear") {
            if (!kasumi::clear_maps_rules()) {
                output::err(destination, "failed to clear Kasumi maps rules\n");
                return 1;
            }
            return 0;
        }
        if (op == "add") {
            if (args.size() < 8) {
                output::err(destination,
                            "usage: kagamid kasumi maps add <target-ino> <target-dev> <spoof-ino> "
                            "<spoof-dev> <spoof-path>\n");
                return 1;
            }
            unsigned long target_ino = 0;
            unsigned long target_dev = 0;
            unsigned long spoof_ino = 0;
            unsigned long spoof_dev = 0;
            if (!parse_unsigned_long(std::string(std::string_view(args[3])), target_ino) ||
                !parse_unsigned_long(std::string(std::string_view(args[4])), target_dev) ||
                !parse_unsigned_long(std::string(std::string_view(args[5])), spoof_ino) ||
                !parse_unsigned_long(std::string(std::string_view(args[6])), spoof_dev)) {
                output::err(destination, "Kasumi maps values must be unsigned integers\n");
                return 1;
            }
            if (!kasumi::add_maps_rule(target_ino, target_dev, spoof_ino, spoof_dev,
                                       std::string(std::string_view(args[7])))) {
                output::err(destination, "failed to add Kasumi maps rule\n");
                return 1;
            }
            return 0;
        }
        output::err(destination, "usage: kagamid kasumi maps clear|add ...\n");
        return 1;
    }
    if (sub == "policy") {
        const auto op = arg_or_default(args, 2, "show");
        if (op == "show" || op == "state") {
            return print_policy_json(destination);
        }
        if (op == "owner" || op == "set") {
            kasumi::PolicyOwner owner = kasumi::PolicyOwner::Auto;
            if (!parse_policy_owner(arg_or_default(args, 3, ""), owner)) {
                output::err(destination,
                            "policy owner must be auto|kernelsu|apatch|manual|disabled\n");
                return 1;
            }
            const auto current = kasumi::policy_snapshot();
            if (!current.state.ok) {
                output::err(destination, "failed to read current Kasumi policy\n");
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
                       destination, current.state, "set Kasumi policy owner",
                       [&]() {
                           return kasumi::replace_policy(owner, flags,
                                                         {allow_uids.data(), allow_uids.size()},
                                                         {deny_uids.data(), deny_uids.size()});
                       })
                       ? 0
                       : 1;
        }
        if (op == "allow" || op == "deny") {
            std::vector<std::uint32_t> uids;
            if (!parse_uid_values(args, 3, uids)) {
                output::err(destination, "policy ", op, " requires numeric uid values\n");
                return 1;
            }
            auto current = kasumi::policy_snapshot();
            if (!current.state.ok) {
                output::err(destination, "failed to read current Kasumi policy\n");
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
                       destination, current.state, "set Kasumi policy " + op + " uid list",
                       [&]() {
                           return kasumi::replace_policy(
                               current.state.owner, current.state.flags,
                               {current.allow_uids.data(), current.allow_uids.size()},
                               {current.deny_uids.data(), current.deny_uids.size()});
                       })
                       ? 0
                       : 1;
        }
        if (op == "clear") {
            kasumi::PolicyUidList list = kasumi::PolicyUidList::All;
            if (!parse_policy_uid_list(arg_or_default(args, 3, "all"), list)) {
                output::err(destination, "policy clear target must be allow|deny|all\n");
                return 1;
            }
            auto current = kasumi::policy_snapshot();
            if (!current.state.ok) {
                output::err(destination, "failed to read current Kasumi policy\n");
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
                       destination, current.state,
                       "clear Kasumi policy " + policy_uid_list_name(list) + " uid list",
                       [&]() {
                           return kasumi::replace_policy(
                               current.state.owner, current.state.flags,
                               {current.allow_uids.data(), current.allow_uids.size()},
                               {current.deny_uids.data(), current.deny_uids.size()});
                       })
                       ? 0
                       : 1;
        }
        if (op == "apply") {
            return apply_config_file(destination, arg_or_default(args, 3, config_file().string()));
        }
        if (op == "reset") {
            const auto current = kasumi::policy_snapshot();
            if (!current.state.ok) {
                output::err(destination, "failed to read current Kasumi policy\n");
                return 1;
            }
            return mutate_policy_preserving_enabled(destination, current.state,
                                                    "reset Kasumi policy",
                                                    []() { return kasumi::reset_policy(); })
                       ? 0
                       : 1;
        }
        output::err(
            destination,
            "usage: kagamid kasumi policy [show|owner OWNER [allow] [deny] [isolated]|allow "
            "UID...|deny UID...|clear allow|deny|all|reset|apply [config]]\n");
        return 1;
    }
    if (sub == "enable") {
        return apply_config_file(destination, config_file(), false, true);
    }
    if (sub == "disable") {
        if (!kasumi::set_enabled(false)) {
            output::err(destination, "failed to set Kasumi enabled state\n");
            return 1;
        }
        return 0;
    }
    if (sub == "clear") {
        if (!kasumi::clear_rules()) {
            output::err(destination, "failed to clear Kasumi rules\n");
            return 1;
        }
        mount::kasumi::invalidate_active_state();
        return 0;
    }
    if (sub == "hide-path" || sub == "delete-rule") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            output::err(destination, "Kasumi path must be absolute\n");
            return 1;
        }
        const bool ok = sub == "hide-path" ? kasumi::hide_path(path) : kasumi::delete_rule(path);
        if (!ok) {
            output::err(destination, "failed to update Kasumi path rule\n");
            return 1;
        }
        return 0;
    }
    if (sub == "fix-mounts") {
        if (!kasumi::fix_mounts()) {
            output::err(destination, "failed to reorder Kasumi mount ids\n");
            return 1;
        }
        return 0;
    }
    if (sub == "hide-overlay-xattrs") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            output::err(destination, "kasumi hide-overlay-xattrs requires an absolute path\n");
            return 1;
        }
        if (!kasumi::hide_overlay_xattrs(path)) {
            output::err(destination, "failed to hide overlay xattrs for ", path, "\n");
            return 1;
        }
        return 0;
    }
    if (sub == "mount-hide" || sub == "maps-spoof" || sub == "statfs-spoof" ||
        sub == "selinux-fix") {
        const std::string value = arg_or_default(args, 2, "off");
        const bool on = value == "on" || value == "normal" || value == "aggressive";
        bool ok = false;
        if (sub == "mount-hide") {
            if (value != "off" && value != "on" && value != "normal" && value != "aggressive") {
                output::err(destination,
                            "usage: kagamid kasumi mount-hide off|normal|aggressive\n");
                return 1;
            }
            ok =
                kasumi::set_mount_hide(on, value == "aggressive" ? kasumi::MountHideMode::Aggressive
                                                                 : kasumi::MountHideMode::Normal);
        } else if (sub == "maps-spoof") {
            ok = kasumi::set_maps_spoof(on);
        } else if (sub == "statfs-spoof") {
            ok = kasumi::set_statfs_spoof(on);
        } else {
            ok = kasumi::set_selinux_guard(on);
        }
        if (!ok) {
            output::err(destination, "failed to set Kasumi ", sub, "\n");
            return 1;
        }
        return 0;
    }
    print_usage(destination);
    return 1;
}
}  // namespace

#if defined(KAGAMI_EMBEDDED)
namespace {
int handle_lkm(output::Context& destination, Arguments) {
    errno = EOPNOTSUPP;
    output::err(destination,
                "Embedded Kasumi is owned by YukiSU; external LKM management is unavailable\n");
    return 1;
}
}  // namespace
#else
namespace {
int handle_lkm(output::Context& destination, Arguments args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "set-autoload") {
        const std::string value = arg_or_default(args, 2, "");
        if (value != "on" && value != "off" && value != "1" && value != "0" && value != "true" &&
            value != "false") {
            output::err(destination, "usage: kagamid lkm set-autoload on|off\n");
            return 1;
        }
        const bool on = value == "on" || value == "1" || value == "true";
        if (!lkm::set_autoload(on)) {
            output::err(destination, lkm::last_error(), "\n");
            return 1;
        }
        output::out(destination, "autoload=", (on ? "on" : "off"), "\n");
        return 0;
    }
    if (sub == "set-kmi") {
        const std::string kmi = arg_or_default(args, 2, "");
        if (!lkm::set_kmi_override(kmi)) {
            output::err(destination, lkm::last_error(), "\n");
            return 1;
        }
        return 0;
    }
    if (sub == "clear-kmi") {
        if (!lkm::clear_kmi_override()) {
            output::err(destination, lkm::last_error(), "\n");
            return 1;
        }
        return 0;
    }
    if (sub == "load") {
        const bool already_available = kasumi::is_available();
        if (!lkm::load()) {
            output::err(destination, "failed to load Kasumi LKM: ", lkm::last_error(), "\n");
            return 1;
        }
        return already_available ? 0 : apply_config_file(destination, config_file(), true);
    }
    if (sub == "unload" || sub == "force-unload") {
        if (lkm::unload(sub != "force-unload")) {
            return 0;
        }
        output::err(destination, "failed to unload Kasumi LKM: ", lkm::last_error(), "\n");
        return 1;
    }
    if (sub == "autoload") {
        const bool enabled = lkm::get_autoload();
        const bool already_available = kasumi::is_available();
        if (!lkm::autoload()) {
            output::err(destination, "Kasumi LKM autoload failed: ", lkm::last_error(), "\n");
            return 1;
        }
        if (!enabled) {
            return 0;
        }
        return already_available ? 0 : apply_config_file(destination, config_file(), true);
    }
    if (sub == "status") {
        output::out(destination, "{\"loaded\":", (lkm::is_loaded() ? "true" : "false"),
                    ",\"autoload\":", (lkm::get_autoload() ? "true" : "false"),
                    ",\"kmi_override\":", bounded::quoted(lkm::get_kmi_override()),
                    ",\"detected_kmi\":", bounded::quoted(lkm::current_kmi()),
                    ",\"asset\":", bounded::quoted(lkm::find_asset()),
                    ",\"last_error\":", bounded::quoted(lkm::last_error()), ",\"unload\":");
        print_lkm_unload_status_json(destination);
        output::out(destination, "}\n");
        return 0;
    }
    output::err(destination,
                "usage: kagamid lkm "
                "load|unload|force-unload|status|autoload|set-autoload|set-kmi|clear-kmi\n");
    return 1;
}
}  // namespace

#endif

namespace {
int handle_hide(output::Context& destination, Arguments args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "list") {
        const auto rules = load_user_hide_rules();
        print_string_array(destination, rules);
        output::out(destination, "\n");
        return 0;
    }
    if (sub == "add") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path[0] != '/') {
            output::err(destination, "hide path must be absolute\n");
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
    print_usage(destination);
    return 1;
}

int handle_recovery(output::Context& destination, Arguments args) {
    const auto sub = arg_or_default(args, 1, "status");
    if (sub == "boot-completed") {
        mount::recovery_boot_completed();
        output::out(destination, "{\"ok\":true,\"action\":\"boot-completed\"}\n");
        return 0;
    }
    if (sub == "reset") {
        mount::recovery_reset();
        output::out(destination, "{\"ok\":true,\"action\":\"reset\"}\n");
        return 0;
    }
    if (sub == "status") {
        output::out(destination, mount::recovery_status_json(), "\n");
        return 0;
    }
    output::err(destination, "usage: kagamid recovery status|boot-completed|reset\n");
    return 1;
}
}  // namespace

namespace {
int run_command_impl(output::Context& destination, Arguments args) {
    if (!args.empty() && args[0] == "prepare") {
        std::string error;
        const bool ok =
            prepare_runtime(error) &&
            (args.size() < 2 || prepare_package_metadata(std::string_view(args[1]), error)) &&
            logging::prepare_boot_log(error);
        logging::write(ok ? logging::Level::Info : logging::Level::Error, "startup",
                       ok ? "private runtime metadata ready" : error);
        if (!ok)
            output::err(destination, error, '\n');
        return ok ? 0 : 1;
    }
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_usage(destination);
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "version" || args[0] == "--version") {
        output::out(destination, KAGAMI_VERSION, "\n");
        return 0;
    }
    if (args[0] == "daemon") {
        return run_daemon_command(destination, args);
    }
    if (args[0] == "config") {
        return handle_config(destination, args);
    }
    if (args[0] == "api") {
        return handle_api(destination, args);
    }
    if (args[0] == "module") {
        return handle_module(destination, args);
    }
    if (args[0] == "kasumi") {
        return handle_kasumi(destination, args);
    }
    if (args[0] == "debug") {
        if (args.size() >= 2 && (args[1] == "enable" || args[1] == "disable")) {
            return kasumi::set_debug(args[1] == "enable") ? 0 : 1;
        }
        if (args.size() >= 3 && args[1] == "stealth") {
            return kasumi::set_stealth(args[2] == "enable") ? 0 : 1;
        }
        return 0;
    }
    if (args[0] == "lkm") {
        return handle_lkm(destination, args);
    }
    if (args[0] == "hide") {
        return handle_hide(destination, args);
    }
    if (args[0] == "recovery") {
        return handle_recovery(destination, args);
    }

    print_usage(destination);
    return 1;
}

}  // namespace

int run_command(output::Context& destination, Arguments args) {
    const auto sub = arg_or_default(args, 1, "");
    const auto group = args.empty() ? std::string_view{} : std::string_view(args.front());
    const auto policy_action = arg_or_default(args, 2, "show");
    const bool module_mutation = group == "module" && sub != "list" && sub != "check-conflicts" &&
                                 sub != "set-mode" && sub != "add-rule" && sub != "remove-rule";
    const bool kasumi_mutation =
        group == "kasumi" && sub != "version" && sub != "list" && sub != "features" &&
        sub != "hooks" &&
        !(sub == "policy" && (policy_action == "show" || policy_action == "state"));
    const bool lkm_mutation = group == "lkm" && (sub == "load" || sub == "unload" ||
                                                 sub == "force-unload" || sub == "autoload");
    const bool changes_runtime = module_mutation || kasumi_mutation || lkm_mutation ||
                                 (group == "config" && sub == "apply") ||
                                 (group == "hide" && (sub == "add" || sub == "remove"));
    if (!changes_runtime) {
        return run_command_impl(destination, args);
    }
    mount::module_runtime::Operation operation;
    const int result = run_command_impl(destination, args);
    const int saved_errno = errno;
    operation.finish(result == 0);
    errno = saved_errno;
    return result;
}

namespace {
bool is_query(Arguments args) {
    if (args.empty())
        return true;
    const auto sub = arg_or_default(args, 1, "");
    return args[0] == "api" || args[0] == "version" || args[0] == "help" ||
           (args[0] == "config" && sub == "show") ||
           (args[0] == "module" && (sub == "list" || sub == "check-conflicts")) ||
           (args[0] == "hide" && sub == "list") ||
           (args[0] == "kasumi" &&
            (sub == "version" || sub == "list" || sub == "features" || sub == "hooks")) ||
           (args[0] == "recovery" && sub == "status");
}

std::string operation_description(Arguments args) {
    if (args.empty())
        return "help";
    std::string result{std::string_view(args[0])};
    if (args.size() > 1) {
        result += " ";
        result += std::string_view(args[1]);
    }
    if (args[0] == "config" && args.size() == 3 && args[1] == "merge-json") {
        JsonValue patch;
        std::string error;
        if (parse_json(std::string(std::string_view(args[2])), patch, error) && patch.is_object()) {
            result += " keys=";
            const std::set<std::string> public_values = {"debug",
                                                         "verbose",
                                                         "kasumi_enabled",
                                                         "builtin_mount_enabled",
                                                         "enable_kernel_debug",
                                                         "enable_stealth",
                                                         "enable_mount_hide",
                                                         "mount_hide_mode",
                                                         "enable_maps_spoof",
                                                         "enable_statfs_spoof",
                                                         "enable_overlay_xattr_hide",
                                                         "enable_selinux_fix",
                                                         "overlayfs_enabled",
                                                         "magic_mount_enabled",
                                                         "fs_type",
                                                         "mount_backend",
                                                         "mountsource"};
            for (const auto& item : patch.object_value) {
                result += item.first;
                if (public_values.count(item.first))
                    result += "=" + stringify_json(item.second);
                result += ",";
            }
        }
    } else if ((args[0] == "module" || args[0] == "hide" || args[0] == "lkm") && args.size() > 2) {
        result += " target=" + json_quote(std::string(std::string_view(args[2])));
        if (args.size() > 3)
            result += " value=" + json_quote(std::string(std::string_view(args[3])));
        if (args.size() > 4)
            result += " mode=" + json_quote(std::string(std::string_view(args[4])));
    }
    return result;
}
}  // namespace

CommandResult run_command_capture(Arguments args, output::Context& destination) {
    destination.out.reset();
    destination.err.reset();
    destination.error = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto level = is_query(args) ? logging::Level::Debug : logging::Level::Info;
    const bool log_operation = logging::enabled(level);
    const auto operation = log_operation ? operation_description(args) : std::string{};
    if (log_operation)
        logging::write(level, "command", "begin " + operation);
    errno = 0;
    int exit_code = run_command(destination, args);
    int error_number = exit_code == 0 ? 0 : (errno != 0 ? errno : EIO);
    if (destination.error) {
        exit_code = 1;
        error_number = destination.error;
        destination.out.reset();
        destination.err.reset();
        destination.error = 0;
        output::err(destination, "command output exceeds prepared capacity\n");
    }
    if (log_operation || exit_code != 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        logging::write(exit_code == 0 ? level : logging::Level::Error, "command",
                       (log_operation ? operation : operation_description(args)) +
                           " result=" + (exit_code == 0 ? "ok" : "failed") + " exit=" +
                           std::to_string(exit_code) + " errno=" + std::to_string(error_number) +
                           " elapsed_ms=" + std::to_string(elapsed));
    }
    if (!destination.err.view().empty())
        logging::write(exit_code == 0 ? logging::Level::Warning : logging::Level::Error, "command",
                       (log_operation ? operation : operation_description(args)) + ": " +
                           std::string(destination.err.view()));
    return {exit_code, error_number, destination.out.view(), destination.err.view()};
}

}  // namespace kagami
