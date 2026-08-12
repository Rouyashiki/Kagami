#include "core/lkm.hpp"

#include "core/json_value.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kagami::lkm {

namespace fs = std::filesystem;

namespace {

std::string g_last_error;

constexpr const char* kModuleName = "kasumi_lkm";

fs::path legacy_autoload_file() { return runtime_data_dir() / "lkm_autoload"; }
fs::path kmi_override_file() { return runtime_data_dir() / "lkm_kmi_override"; }
fs::path ownership_file() { return runtime_lkm_owner_file(); }

void set_error(const std::string& message) { g_last_error = message; }

std::string read_first_line(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    return std::getline(in, line) ? line : "";
}

bool write_file(const fs::path& path, const std::string& value) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        set_error("create " + path.parent_path().string() + ": " + ec.message());
        return false;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        set_error("open " + path.string() + ": " + std::strerror(errno));
        return false;
    }
    out << value;
    return out.good();
}

std::string random_nonce() {
#if defined(__linux__)
    std::array<unsigned char, 16> bytes = {};
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return "";
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t n = read(fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            close(fd);
            return "";
        }
        offset += static_cast<std::size_t>(n);
    }
    close(fd);
    static constexpr char hex[] = "0123456789abcdef";
    std::string nonce;
    nonce.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        nonce.push_back(hex[byte >> 4]);
        nonce.push_back(hex[byte & 0x0f]);
    }
    return nonce;
#else
    return "";
#endif
}

std::string owner_token_for_nonce(const std::string& nonce) {
#if defined(__linux__)
    const std::string boot_id = read_first_line("/proc/sys/kernel/random/boot_id");
    if (boot_id.empty() || nonce.size() != 32 ||
        !std::all_of(nonce.begin(), nonce.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) {
        return "";
    }
    return "v2\n" + boot_id + "\n" + nonce + "\n";
#else
    return "";
#endif
}

std::string module_instance_token() {
#if defined(__linux__)
    return owner_token_for_nonce(
        read_first_line("/sys/module/kasumi_lkm/parameters/kasumi_owner_nonce"));
#else
    return "";
#endif
}

bool valid_kmi(const std::string& kmi) {
    return !kmi.empty() && std::all_of(kmi.begin(), kmi.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
}

std::string arch_suffix() {
#if defined(__aarch64__)
    return "_arm64";
#elif defined(__arm__)
    return "_armv7";
#elif defined(__x86_64__)
    return "_x86_64";
#else
    return "_arm64";
#endif
}

std::vector<fs::path> asset_directories() {
    std::vector<fs::path> dirs;
    if (const char* override = std::getenv("KAGAMI_LKM_DIR"); override && *override) {
        dirs.emplace_back(override);
    }

#if defined(__linux__)
    char executable[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (n > 0) {
        executable[n] = '\0';
        dirs.emplace_back(fs::path(executable).parent_path() / "kasumi");
    }
#endif
    dirs.emplace_back(runtime_modules_dir() / "kagami" / "kasumi");
    dirs.emplace_back(runtime_data_dir() / "kasumi");
    return dirs;
}

#if defined(__linux__)
bool finit_module_load(const std::string& path, const char* params) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_error("open " + path + ": " + std::strerror(errno));
        return false;
    }
    const long rc = syscall(SYS_finit_module, fd, params, 0);
    const int saved_errno = errno;
    close(fd);
    if (rc == 0) {
        return true;
    }
    if (saved_errno == EEXIST) {
        set_error("kasumi_lkm was loaded concurrently; ownership was not acquired");
        return false;
    }
    if (saved_errno != ENOSYS) {
        set_error("finit_module " + path + ": " + std::strerror(saved_errno));
        return false;
    }

    const int image_fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (image_fd < 0) {
        set_error("open " + path + ": " + std::strerror(errno));
        return false;
    }
    struct stat st = {};
    if (fstat(image_fd, &st) != 0 || st.st_size <= 0) {
        set_error("stat " + path + ": " + std::strerror(errno));
        close(image_fd);
        return false;
    }
    std::vector<char> image(static_cast<std::size_t>(st.st_size));
    std::size_t read_size = 0;
    while (read_size < image.size()) {
        const ssize_t nread = read(image_fd, image.data() + read_size, image.size() - read_size);
        if (nread < 0 && errno == EINTR) {
            continue;
        }
        if (nread <= 0) {
            set_error("read " + path + ": " + std::strerror(errno));
            close(image_fd);
            return false;
        }
        read_size += static_cast<std::size_t>(nread);
    }
    close(image_fd);
    const long init_rc = syscall(SYS_init_module, image.data(), image.size(), params);
    const int init_errno = errno;
    if (init_rc == 0) {
        return true;
    }
    if (init_errno == EEXIST) {
        set_error("kasumi_lkm was loaded concurrently; ownership was not acquired");
        return false;
    }
    set_error("init_module " + path + ": " + std::strerror(init_errno));
    return false;
}

bool delete_module_nonblocking() {
    if (syscall(SYS_delete_module, kModuleName, O_NONBLOCK) == 0) {
        return true;
    }
    set_error(std::string("delete_module ") + kModuleName + ": " + std::strerror(errno));
    return false;
}

#endif

} // namespace

bool is_loaded() { return kasumi::module_loaded(); }

bool owns_loaded_module() {
    if (!is_loaded()) {
        return false;
    }
    const std::string current = module_instance_token();
    if (current.empty() || read_first_line(ownership_file()) != "v2") {
        return false;
    }
    std::ifstream in(ownership_file());
    std::ostringstream content;
    content << in.rdbuf();
    return (in.good() || in.eof()) && content.str() == current;
}

bool retain_owned_connection() {
    kasumi::set_connection_persistent(false);
    if (is_loaded() && !owns_loaded_module()) {
        return false;
    }

    kasumi::set_connection_persistent(true);
    if (!kasumi::is_available()) {
        kasumi::set_connection_persistent(false);
        return false;
    }

    // The capability FD now pins the exact module instance. Recheck ownership
    // after acquiring it so an unload/reload between the first check and GET_FD
    // cannot make the daemon retain somebody else's LKM.
    if (is_loaded() && !owns_loaded_module()) {
        kasumi::set_connection_persistent(false);
        return false;
    }
    return true;
}

std::string last_error() { return g_last_error; }

std::string get_kmi_override() { return read_first_line(kmi_override_file()); }

bool set_kmi_override(const std::string& kmi) {
    if (!valid_kmi(kmi)) {
        set_error("KMI may contain only letters, digits, '.', '_' and '-'");
        return false;
    }
    return write_file(kmi_override_file(), kmi + "\n");
}

bool clear_kmi_override() {
    std::error_code ec;
    fs::remove(kmi_override_file(), ec);
    if (ec) {
        set_error("remove KMI override: " + ec.message());
    }
    return !ec;
}

bool get_autoload() {
    std::ifstream config_file(runtime_config_file(), std::ios::binary);
    if (config_file) {
        std::ostringstream data;
        data << config_file.rdbuf();
        JsonValue root;
        std::string error;
        const JsonValue* value = parse_json(data.str(), root, error) && root.is_object()
                                     ? root.find("lkm_autoload")
                                     : nullptr;
        if (value && value->is_bool()) {
            return value->bool_value;
        }
    }

    // Migration for the short-lived pre-config implementation. New installs
    // default to disabled; an explicit legacy setting remains honored once.
    const std::string legacy = read_first_line(legacy_autoload_file());
    return legacy == "1" || legacy == "on" || legacy == "true";
}

bool set_autoload(bool enabled) {
    g_last_error.clear();
    if (!update_lkm_autoload_config(runtime_config_file().string(), enabled, g_last_error)) {
        return false;
    }
    std::error_code ec;
    fs::remove(legacy_autoload_file(), ec);
    return true;
}

std::string current_kmi() {
    std::string release = read_first_line("/proc/sys/kernel/osrelease");
#if defined(__linux__)
    if (release.empty()) {
        utsname uts = {};
        if (uname(&uts) == 0) {
            release = uts.release;
        }
    }
#endif
    const std::size_t first_dot = release.find('.');
    const std::size_t second_dot = release.find('.', first_dot == std::string::npos ? 0 : first_dot + 1);
    const std::size_t android = release.find("-android");
    if (first_dot == std::string::npos || android == std::string::npos) {
        return "";
    }
    const std::string major_minor = release.substr(0, second_dot == std::string::npos ? release.size() : second_dot);
    const std::size_t version_start = android + std::strlen("-android");
    const std::size_t version_end = release.find('-', version_start);
    const std::string android_version = release.substr(version_start, version_end - version_start);
    return android_version.empty() ? "" : "android" + android_version + "-" + major_minor;
}

std::string find_asset(const std::string& requested_kmi) {
    const std::string kmi = requested_kmi.empty() ? current_kmi() : requested_kmi;
    std::vector<std::string> names;
    if (valid_kmi(kmi)) {
        names.push_back(kmi + arch_suffix() + "_kasumi_lkm.ko");
    }
    names.push_back(arch_suffix() + "_kasumi_lkm.ko");
    names.push_back("kasumi_lkm.ko");
    for (const auto& dir : asset_directories()) {
        for (const auto& name : names) {
            const fs::path candidate = dir / name;
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec) && !ec) {
                return candidate.string();
            }
        }
    }
    return "";
}

bool load() {
    g_last_error.clear();
    if (kasumi::is_available()) {
        return true; // Kasumi may be kernel-built-in rather than an LKM.
    }
    if (is_loaded()) {
        set_error("kasumi_lkm is present but its protocol is unavailable or incompatible");
        return false;
    }
    const std::string kmi = get_kmi_override().empty() ? current_kmi() : get_kmi_override();
    const std::string asset = find_asset(kmi);
    if (asset.empty()) {
        set_error("no Kasumi module asset matches " + (kmi.empty() ? std::string("this kernel") : kmi));
        return false;
    }
#if defined(__linux__)
    // Kasumi API 17 keeps the GET_FD syscall number as an internal default and
    // no longer exports hymo's kasumi_syscall_nr module parameter. Passing the
    // old option succeeds but pollutes dmesg with an unknown-parameter warning.
    const std::string nonce = random_nonce();
    if (nonce.empty()) {
        set_error("cannot generate a Kasumi LKM ownership token");
        return false;
    }
    const std::string params = "kasumi_owner_nonce=" + nonce;
    if (!finit_module_load(asset, params.c_str())) {
        return false;
    }
    const std::string expected_instance = owner_token_for_nonce(nonce);
    kasumi::set_connection_persistent(false);
    kasumi::set_connection_persistent(true);
    if (!kasumi::is_available()) {
        kasumi::set_connection_persistent(false);
        set_error("loaded Kasumi LKM protocol is unavailable or incompatible");
        return false;
    }
    const std::string instance = module_instance_token();
    if (expected_instance.empty() || instance != expected_instance) {
        kasumi::set_connection_persistent(false);
        set_error("loaded kasumi_lkm instance changed before ownership was acquired");
        return false;
    }
    if (!write_file(ownership_file(), instance)) {
        kasumi::set_connection_persistent(false);
        return false;
    }
    return true;
#else
    set_error("Kasumi LKM loading requires Android/Linux");
    return false;
#endif
}

bool autoload() {
    return !get_autoload() || is_loaded() || kasumi::is_available() || load();
}

bool unload(bool require_ownership) {
    g_last_error.clear();
    if (!is_loaded()) {
        kasumi::set_connection_persistent(false);
        std::error_code stale_ec;
        fs::remove(ownership_file(), stale_ec);
        if (kasumi::is_available()) {
            (void)retain_owned_connection();
            set_error("Kasumi is available without kasumi_lkm; it cannot be unloaded by Kagami");
            return false;
        }
        return true;
    }
#if defined(__linux__)
    if (require_ownership) {
        if (!owns_loaded_module()) {
            set_error("refusing to unload a Kasumi LKM not loaded by Kagami");
            return false;
        }
    }
    (void)kasumi::set_enabled(false);
    (void)kasumi::clear_rules();
    kasumi::set_connection_persistent(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (delete_module_nonblocking()) {
            std::error_code ec;
            fs::remove(ownership_file(), ec);
            return true;
        }
        if (errno != EAGAIN && errno != EBUSY) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    if (require_ownership && owns_loaded_module()) {
        (void)retain_owned_connection();
    }
    return false;
#else
    set_error("Kasumi LKM unloading requires Android/Linux");
    return false;
#endif
}

} // namespace kagami::lkm
