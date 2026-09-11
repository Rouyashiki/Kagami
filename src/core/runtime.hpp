#pragma once

#include <filesystem>
#include <string>

namespace kagami {

std::filesystem::path runtime_data_dir();
std::filesystem::path runtime_modules_dir();
std::filesystem::path runtime_config_file();
std::filesystem::path runtime_log_file();
std::filesystem::path runtime_socket_file();
std::filesystem::path runtime_pid_file();
std::filesystem::path runtime_daemon_lock_file();
std::filesystem::path runtime_lkm_owner_file();
std::string runtime_boot_id();
bool write_file_atomic(const std::string& path, const std::string& data, std::string& error);
bool runtime_mount_here();
bool prepare_private_directory(const std::filesystem::path& path, std::string& error);
bool prepare_private_file(const std::filesystem::path& path, std::string& error);
bool prepare_runtime(std::string& error);
bool prepare_package_metadata(const std::filesystem::path& path, std::string& error);

}  // namespace kagami
