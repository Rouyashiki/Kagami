#include "core/runtime.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#if defined(__ANDROID__)
#include <sys/xattr.h>
#endif
#if defined(KAGAMI_EMBEDDED)
#include "kagami/embedded_paths.hpp"
#endif

namespace kagami {

namespace fs = std::filesystem;

fs::path runtime_data_dir() {
#if defined(KAGAMI_EMBEDDED)
#if !defined(__ANDROID__)
    if (const char* path = std::getenv("KSU_KAGAMI_TEST_DIR"); path && *path)
        return path;
#endif
    return embedded_data_dir;
#else
    const char* path = std::getenv("KAGAMI_DATA_DIR");
    return path && *path ? fs::path(path) : fs::path("/data/adb/kagami");
#endif
}

fs::path runtime_modules_dir() {
#if !defined(KAGAMI_EMBEDDED)
    if (const char* path = std::getenv("KAGAMI_MODULES_DIR"); path && *path)
        return path;
#elif !defined(__ANDROID__)
    if (const char* path = std::getenv("KSU_KAGAMI_TEST_MODULES"); path && *path)
        return path;
#endif
    return "/data/adb/modules";
}

fs::path runtime_config_file() {
    return runtime_data_dir() / "config.json";
}

fs::path runtime_log_file() {
#if defined(KAGAMI_EMBEDDED)
#if !defined(__ANDROID__)
    if (std::getenv("KSU_KAGAMI_TEST_DIR"))
        return runtime_data_dir() / "kagami.log";
#endif
    return embedded_log_file;
#else
    return runtime_data_dir() / "daemon.log";
#endif
}

fs::path runtime_socket_file() {
    return runtime_data_dir() / "kagamid.sock";
}
fs::path runtime_pid_file() {
    return runtime_data_dir() / "kagamid.pid";
}
fs::path runtime_daemon_lock_file() {
#if defined(KAGAMI_EMBEDDED)
    return runtime_data_dir() / "kagamid.lock";
#else
    return "/data/adb/kagami/kagamid.lock";
#endif
}
fs::path runtime_lkm_owner_file() {
#if defined(KAGAMI_EMBEDDED)
    return runtime_data_dir() / "run" / "kasumi_lkm.owner";
#else
    return "/data/adb/kagami/run/kasumi_lkm.owner";
#endif
}

std::string runtime_boot_id() {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string id;
    std::getline(input, id);
    return id;
}

bool runtime_mount_here() {
#if defined(KAGAMI_EMBEDDED) && defined(__ANDROID__)
    return false;
#elif defined(KAGAMI_EMBEDDED)
    return std::getenv("KSU_KAGAMI_TEST_MOUNT_HERE") != nullptr;
#else
    return std::getenv("KAGAMI_MOUNT_HERE") != nullptr;
#endif
}

namespace {
bool metadata(const fs::path& path, mode_t mode, std::string& error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        error = "lstat " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        error = "refusing metadata symlink: " + path.string();
        return false;
    }
    if (((st.st_mode & 07777) != mode && chmod(path.c_str(), mode) != 0) ||
        (geteuid() == 0 && (st.st_uid != 0 || st.st_gid != 0) && chown(path.c_str(), 0, 0) != 0)) {
        error = "secure " + path.string() + ": " + std::strerror(errno);
        return false;
    }
#if defined(__ANDROID__)
    constexpr char context[] = "u:object_r:adb_data_file:s0";
    char current[256]{};
    const ssize_t length =
        lgetxattr(path.c_str(), "security.selinux", current, sizeof(current) - 1);
    if (length < 0 || std::strcmp(current, context) != 0) {
        if (lsetxattr(path.c_str(), "security.selinux", context, sizeof(context), 0) != 0) {
            error = "label " + path.string() + ": " + std::strerror(errno);
            return false;
        }
    }
#endif
    return true;
}
}  // namespace

bool prepare_private_directory(const fs::path& path, std::string& error) {
    error.clear();
    const auto normalized = path.lexically_normal();
    if (normalized.empty() || !normalized.is_absolute() || normalized == normalized.root_path() ||
        normalized == "/data" || normalized == "/data/adb" || normalized == "/data/adb/ksu" ||
        normalized == "/data/adb/modules" || normalized == "/tmp") {
        error = "refusing a shared runtime directory: " + path.string();
        return false;
    }
    std::error_code ec;
    const bool created = fs::create_directories(path, ec);
    if (ec) {
        error = "mkdir " + path.string() + ": " + ec.message();
        return false;
    }
    if (!created && normalized == runtime_data_dir().lexically_normal() &&
        normalized != "/data/adb/kagami" && normalized != "/data/adb/ksu/kagami") {
        struct stat st{};
        if (lstat(path.c_str(), &st) != 0 || st.st_uid != geteuid() ||
            (st.st_mode & 0777) != 0700) {
            error = "custom runtime directory must be private and owned by the controller: " +
                    path.string();
            return false;
        }
    }
    return metadata(path, 0700, error);
}

bool prepare_private_file(const fs::path& path, std::string& error) {
    error.clear();
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT)
            return true;
        error = "lstat " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    if ((!S_ISREG(st.st_mode) && !S_ISSOCK(st.st_mode)) || st.st_nlink != 1) {
        error = "not a private controller file: " + path.string();
        return false;
    }
    return metadata(path, 0600, error);
}

bool prepare_runtime(std::string& error) {
    error.clear();
    if (!prepare_private_directory(runtime_data_dir(), error) ||
        !prepare_private_directory(runtime_data_dir() / "run", error) ||
        !prepare_private_directory(runtime_log_file().parent_path(), error))
        return false;
    // These are controller records, not mounted module payloads or LKM assets.
    for (const auto* name : {"config.json", "config.json.lock", "module_mode.json",
                             "module_rules.json", "user_hide_rules.json", "kagamid.pid",
                             "kagamid.sock", "kagamid.lock", "mirror.img", "mirror.erofs"}) {
        if (!prepare_private_file(runtime_data_dir() / name, error))
            return false;
    }
    return true;
}

bool prepare_package_metadata(const fs::path& path, std::string& error) {
    error.clear();
#if defined(KAGAMI_EMBEDDED)
    (void)path;
    error = "the embedded controller has no standalone package";
    errno = EOPNOTSUPP;
    return false;
#else
    const auto normalized = path.lexically_normal();
    if (!normalized.is_absolute() || normalized == normalized.root_path()) {
        error = "invalid Kagami package directory";
        return false;
    }
#if defined(__ANDROID__)
    const auto modules = runtime_modules_dir().lexically_normal();
    if (normalized != modules / "kagami" &&
        normalized != modules.parent_path() / "modules_update" / "kagami") {
        error = "Kagami metadata preparation is restricted to its package directory";
        return false;
    }
#endif
    std::ifstream prop(path / "module.prop");
    std::string line;
    bool kagami = false;
    while (std::getline(prop, line)) {
        if (line == "id=kagami" || line == "id=kagami\r")
            kagami = true;
    }
    if (!kagami) {
        error = "not a Kagami package: " + path.string();
        return false;
    }
    if (!metadata(path, 0755, error))
        return false;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(path, ec), end; it != end && !ec; it.increment(ec)) {
        const auto relative = it->path().lexically_relative(path);
        const auto first = relative.begin()->string();
        if (first == "system" || first == "vendor" || first == "product" || first == "system_ext" ||
            first == "odm" || first == "oem") {
            it.disable_recursion_pending();
            continue;
        }
        const auto status = it->symlink_status(ec);
        if (ec)
            break;
        if (fs::is_symlink(status))
            continue;
        const bool executable = (status.permissions() & fs::perms::owner_exec) != fs::perms::none;
        if (!metadata(it->path(), fs::is_directory(status) || executable ? 0755 : 0644, error))
            return false;
    }
    if (ec) {
        error = "package metadata: " + ec.message();
        return false;
    }
    return true;
#endif
}

}  // namespace kagami
