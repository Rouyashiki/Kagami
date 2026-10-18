#include "mount/partition_scan.hpp"

#include <algorithm>
#include <cctype>

namespace kagami::mount {
namespace fs = std::filesystem;
namespace {
bool component(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '_' || c == '-' || c == '.';
           });
}
bool missing(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
}
bool resolve_directory(const fs::path& path, fs::path& resolved, std::error_code& error) {
    resolved.clear();
    if (fs::is_directory(path, error))
        resolved = fs::canonical(path, error);
    if (missing(error))
        error.clear();
    return !error;
}
bool scan_payload(const fs::path& payload, const fs::path& system_root,
                  const std::set<fs::path>& targets, std::set<std::string>& partitions,
                  std::error_code& error) {
    fs::directory_iterator entry(payload, error);
    if (missing(error)) {
        error.clear();
        return true;
    }
    const fs::directory_iterator end;
    for (; !error && entry != end; entry.increment(error)) {
        const auto name = entry->path().filename().string();
        if (!component(name))
            continue;
        fs::path target;
        if (!resolve_directory(system_root / name, target, error))
            break;
        if (target.empty() || targets.count(target) == 0)
            continue;
        if (entry->is_directory(error) && !fs::is_empty(entry->path(), error) && !error)
            partitions.insert(name);
        if (error)
            break;
    }
    return !error;
}
}  // namespace

bool discover_module_partitions(const fs::path& modules, const fs::path& system_root,
                                const module_runtime::Snapshot& runtime,
                                const std::vector<module_runtime::MountIdentity>& mounts,
                                Arguments configured, PartitionScan& result, std::string& error) {
    std::set<fs::path> targets;
    std::error_code ec;
    for (const auto& mount : mounts) {
        // Subtree binds, tmpfs/overlay work directories and ordinary root
        // directories are not evidence of a system partition. Keep underlying
        // block mounts even when an overlay covers them.
        if (mount.root != "/" || mount.path == "/" ||
            (mount.source.rfind("/dev/block/", 0) != 0 && mount.source != "/dev/root"))
            continue;
        fs::path target;
        if (!resolve_directory(system_root / fs::path(mount.path).relative_path(), target, ec))
            break;
        if (!target.empty())
            targets.insert(std::move(target));
    }
    PartitionScan scanned;
    for (const auto& configured_name : configured) {
        if (ec)
            break;
        const std::string name{std::string_view(configured_name)};
        fs::path target;
        if (component(name) && !resolve_directory(system_root / name, target, ec))
            break;
        if (target.empty() || targets.count(target) == 0)
            scanned.invalid.insert(name);
    }
    for (const auto& [id, summary] : runtime.modules) {
        if (ec)
            break;
        if ((summary.state != "mounted" && summary.state != "partial") || !component(id))
            continue;
        const auto module = modules / id;
        if (!fs::is_regular_file(module / "module.prop", ec)) {
            if (missing(ec))
                ec.clear();
            continue;
        }
        // Android modules support both <partition>/ and system/<partition>/.
        if (!scan_payload(module, system_root, targets, scanned.discovered, ec) ||
            !scan_payload(module / "system", system_root, targets, scanned.discovered, ec))
            break;
    }
    if (ec) {
        error = "partition scan failed: " + ec.message();
        return false;
    }
    result = std::move(scanned);
    return true;
}

}  // namespace kagami::mount
