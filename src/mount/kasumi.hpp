#pragma once

#include <map>
#include <string>
#include <vector>

#include "mount/backend.hpp"

namespace kagami::mount::kasumi {

// Apply the API17 policy owner, UID lists, and flags before installing Kasumi
// rules. Policy is kernel state and is lost when an LKM reloads, so this must
// be part of the mount path rather than a manager-only side action.
bool apply_policy_config(const PolicyConfig& policy, std::string& error);

// Mirrors module content off /data, compiles it into ADD/MERGE/HIDE Kasumi
// rules, and applies the configured kernel controls. Must run in PID 1's mount
// namespace because the mirror filesystem is mounted there.
bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                   const ModuleRuleMap& rules);

// Removes Kagami-owned Kasumi rules and detaches its mirror storage. Must run
// in the init mount namespace.
bool unmount_all(const Config& config);

bool is_active();

} // namespace kagami::mount::kasumi
