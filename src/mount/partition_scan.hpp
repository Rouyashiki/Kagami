#pragma once

#include "core/arguments.hpp"
#include "mount/module_runtime.hpp"

#include <filesystem>
#include <set>
#include <string>

namespace kagami::mount {

struct PartitionScan {
    std::set<std::string> discovered;
    std::set<std::string> invalid;
};

// Intersect currently mounted modules' payload directories with real block
// partitions in the supplied namespace. The roots are explicit for fixtures;
// production supplies the configured module directory and PID 1's root.
// Failure leaves the caller's result unchanged.
bool discover_module_partitions(const std::filesystem::path& modules,
                                const std::filesystem::path& system_root,
                                const module_runtime::Snapshot& runtime,
                                const std::vector<module_runtime::MountIdentity>& mounts,
                                Arguments configured, PartitionScan& result, std::string& error);

}  // namespace kagami::mount
