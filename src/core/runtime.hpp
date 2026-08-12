#pragma once

#include <filesystem>

namespace kagami {

std::filesystem::path runtime_data_dir();
std::filesystem::path runtime_modules_dir();
std::filesystem::path runtime_config_file();
std::filesystem::path runtime_log_file();
std::filesystem::path runtime_socket_file();
std::filesystem::path runtime_pid_file();
std::filesystem::path runtime_daemon_lock_file();
std::filesystem::path runtime_lkm_owner_file();

} // namespace kagami
