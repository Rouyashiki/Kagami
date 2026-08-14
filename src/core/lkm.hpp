#pragma once

#include "kagami/kasumi_client.hpp"

#include <string>

namespace kagami::lkm {

struct UnloadStatus {
    bool attempted = false;
    bool quiesce_supported = false;
    int capability_errno = 0;
    kasumi::QuiesceSnapshot quiesce;
    int delete_errno = 0;
};

// Kasumi LKM lifecycle for a standalone metamodule. Assets are looked up from
// Kagami's module/data directories instead of ksud's embedded asset table.
bool load();
bool unload(bool require_ownership = true);
bool is_loaded();
bool owns_loaded_module();
bool retain_owned_connection();
bool autoload();
bool set_autoload(bool enabled);
bool get_autoload();
bool set_kmi_override(const std::string& kmi);
bool clear_kmi_override();
std::string get_kmi_override();
std::string current_kmi();
std::string find_asset(const std::string& kmi = "");
std::string last_error();
UnloadStatus unload_status();

} // namespace kagami::lkm
