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
UnloadStatus g_unload_status;
std::string g_ready_unload_instance;

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
enum class HelperLoadResult {
    Unavailable,
    Success,
    Failure,
};

std::string lkm_loader_path() {
    char executable[PATH_MAX] = {};
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0) {
        return "";
    }
    executable[length] = '\0';
    const fs::path helper =
        fs::path(executable).parent_path() / "lkmloader";
    struct stat st = {};
    if (stat(helper.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        access(helper.c_str(), X_OK) != 0) {
        return "";
    }
    return helper.string();
}

HelperLoadResult try_lkm_loader(const std::string& path, const std::string& params,
                                std::string& error) {
    const std::string helper = lkm_loader_path();
    if (helper.empty()) {
        return HelperLoadResult::Unavailable;
    }

    char command[] = "lkmloader";
    std::array<char*, 4> argv = {
        command,
        const_cast<char*>(path.c_str()),
        params.empty() ? nullptr : const_cast<char*>(params.c_str()),
        nullptr,
    };
    const pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork LKM loader: ") + std::strerror(errno);
        return HelperLoadResult::Failure;
    }
    if (pid == 0) {
        execv(helper.c_str(), argv.data());
        _exit(127);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        error = std::string("waitpid LKM loader: ") + std::strerror(errno);
        return HelperLoadResult::Failure;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return HelperLoadResult::Success;
    }
    if (WIFEXITED(status)) {
        error = "LKM loader exited with status " +
                std::to_string(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        error = "LKM loader was killed by signal " +
                std::to_string(WTERMSIG(status));
    } else {
        error = "LKM loader ended with an unknown status";
    }
    return HelperLoadResult::Failure;
}

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
    const std::string finit_error = "finit_module " + path + ": " +
                                    std::strerror(saved_errno);
    std::string helper_error;
    const HelperLoadResult helper_result =
        try_lkm_loader(path, params ? params : "", helper_error);
    if (helper_result == HelperLoadResult::Success) {
        g_last_error.clear();
        return true;
    }
    if (saved_errno != ENOSYS) {
        set_error(helper_result == HelperLoadResult::Failure
                      ? finit_error + "; helper fallback: " + helper_error
                      : finit_error);
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
    const std::string init_error = "init_module " + path + ": " +
                                   std::strerror(init_errno);
    set_error(helper_result == HelperLoadResult::Failure
                  ? finit_error + "; helper fallback: " + helper_error + "; " + init_error
                  : init_error);
    return false;
}

int delete_module_nonblocking() {
    errno = 0;
    if (syscall(SYS_delete_module, kModuleName, O_NONBLOCK) == 0) {
        return 0;
    }
    const int saved_errno = errno != 0 ? errno : EIO;
    set_error(std::string("delete_module ") + kModuleName + ": " +
              std::strerror(saved_errno));
    return saved_errno;
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

UnloadStatus unload_status() { return g_unload_status; }

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
    g_unload_status = {};
    g_ready_unload_instance.clear();
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
    const UnloadStatus previous_unload = g_unload_status;
    const std::string previous_ready_instance = g_ready_unload_instance;
    g_unload_status = {};
    g_unload_status.attempted = true;
    if (!is_loaded()) {
        g_ready_unload_instance.clear();
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
    const std::string current_instance = module_instance_token();
    const bool retry_ready_delete =
        !previous_ready_instance.empty() &&
        previous_ready_instance == current_instance &&
        previous_unload.attempted && previous_unload.quiesce_supported &&
        previous_unload.quiesce.ok &&
        previous_unload.quiesce.state == kasumi::QuiesceState::Ready;
    if (!retry_ready_delete) {
        g_ready_unload_instance.clear();
    }

    if (require_ownership) {
        if (!owns_loaded_module()) {
            set_error("refusing to unload a Kasumi LKM not loaded by Kagami");
            return false;
        }
    }

    if (retry_ready_delete) {
        // PREPARE is terminal and GET_FD has already stopped. Preserve the
        // READY snapshot and retry only delete_module for this exact instance.
        g_unload_status.quiesce_supported = true;
        g_unload_status.quiesce = previous_unload.quiesce;
        kasumi::set_connection_persistent(false);
    } else {
        kasumi::set_connection_persistent(true);
        const auto capabilities = kasumi::feature_capabilities();
        if (!capabilities.ok) {
            g_unload_status.capability_errno = capabilities.last_errno;
            const int saved_errno = capabilities.last_errno != 0
                                        ? capabilities.last_errno
                                        : EIO;
            set_error(std::string("query Kasumi unload capability: ") +
                      std::strerror(saved_errno));
            errno = saved_errno;
            return false;
        }
        if (require_ownership && !owns_loaded_module()) {
            kasumi::set_connection_persistent(false);
            set_error("loaded kasumi_lkm instance changed before unload capability was acquired");
            errno = ESTALE;
            return false;
        }

        g_unload_status.quiesce_supported = capabilities.quiesce;
        if (capabilities.quiesce) {
            constexpr int kQuiescePollAttempts = 30;
            bool ready = false;
            for (int attempt = 0; attempt < kQuiescePollAttempts; ++attempt) {
                g_unload_status.quiesce = kasumi::prepare_unload();
                const auto& snapshot = g_unload_status.quiesce;
                if (!snapshot.ok) {
                    const int saved_errno = snapshot.last_errno != 0
                                                ? snapshot.last_errno
                                                : EIO;
                    std::ostringstream message;
                    message << "prepare Kasumi unload: " << std::strerror(saved_errno)
                            << " (state=" << static_cast<std::uint32_t>(snapshot.state)
                            << ", err=" << snapshot.err << ")";
                    set_error(message.str());
                    errno = saved_errno;
                    return false;
                }
                if (snapshot.state == kasumi::QuiesceState::Ready) {
                    ready = true;
                    break;
                }
                if (snapshot.state != kasumi::QuiesceState::Active &&
                    snapshot.state != kasumi::QuiesceState::Draining) {
                    set_error("prepare Kasumi unload returned an unknown state");
                    errno = EPROTO;
                    return false;
                }
                if (attempt + 1 < kQuiescePollAttempts) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            if (!ready) {
                const auto& snapshot = g_unload_status.quiesce;
                std::ostringstream message;
                message << "Kasumi unload quiesce timed out (state="
                        << static_cast<std::uint32_t>(snapshot.state)
                        << ", busy_mask=0x" << std::hex << snapshot.busy_mask
                        << std::dec << ", control_files=" << snapshot.control_files
                        << ", module_refs=" << snapshot.module_refs << ")";
                set_error(message.str());
                errno = EBUSY;
                return false;
            }

            // READY proves callback teardown and leaves one control-file module
            // reference. delete_module remains authoritative if an fd alias keeps
            // that same file alive after this descriptor is closed.
            g_ready_unload_instance = module_instance_token();
            kasumi::set_connection_persistent(false);
        } else {
            // API 17 without KSM_FEATURE_QUIESCE keeps the legacy best-effort path.
            (void)kasumi::set_enabled(false);
            (void)kasumi::clear_rules();
            kasumi::set_connection_persistent(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    }

    for (int attempt = 0; attempt < 5; ++attempt) {
        const int delete_errno = delete_module_nonblocking();
        g_unload_status.delete_errno = delete_errno;
        if (delete_errno == 0) {
            g_ready_unload_instance.clear();
            std::error_code ec;
            fs::remove(ownership_file(), ec);
            g_last_error.clear();
            return true;
        }
        if (delete_errno != EAGAIN && delete_errno != EBUSY) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    const int delete_errno = g_unload_status.delete_errno;
    if (!g_unload_status.quiesce_supported && require_ownership && owns_loaded_module()) {
        (void)retain_owned_connection();
    }
    errno = delete_errno;
    return false;
#else
    set_error("Kasumi LKM unloading requires Android/Linux");
    return false;
#endif
}

} // namespace kagami::lkm
