#include "core/runtime.hpp"

#include <cstdlib>

namespace kagami {

static constexpr const char* kDataDir = "/data/adb/kagami";
static constexpr const char* kModulesDir = "/data/adb/modules";

std::filesystem::path runtime_data_dir() {
    const char* env = std::getenv("KAGAMI_DATA_DIR");
    return (env && *env) ? std::filesystem::path(env) : std::filesystem::path(kDataDir);
}

std::filesystem::path runtime_modules_dir() {
    const char* env = std::getenv("KAGAMI_MODULES_DIR");
    return (env && *env) ? std::filesystem::path(env) : std::filesystem::path(kModulesDir);
}

std::filesystem::path runtime_config_file() {
    return runtime_data_dir() / "config.json";
}

std::filesystem::path runtime_log_file() {
    return runtime_data_dir() / "daemon.log";
}

std::filesystem::path runtime_socket_file() {
    return runtime_data_dir() / "kagamid.sock";
}

std::filesystem::path runtime_pid_file() {
    return runtime_data_dir() / "kagamid.pid";
}

std::filesystem::path runtime_daemon_lock_file() {
    return std::filesystem::path(kDataDir) / "kagamid.lock";
}

std::filesystem::path runtime_lkm_owner_file() {
    return std::filesystem::path(kDataDir) / "run" / "kasumi_lkm.owner";
}

} // namespace kagami
