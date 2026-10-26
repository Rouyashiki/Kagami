#include "core/command.hpp"
#include "core/json_value.hpp"
#include "mount/module_runtime.hpp"
#include "mount/partition_scan.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace runtime = kagami::mount::module_runtime;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
void file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << contents;
    require(stream.good(), "write " + path.string());
}
struct Fixture {
    fs::path path;
    Fixture() {
        char pattern[] = "/data/local/tmp/kagami-module-list-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "create list fixture");
        path = created;
        require(setenv("KAGAMI_DATA_DIR", path.c_str(), 1) == 0, "runtime directory");
        require(setenv("KAGAMI_MOUNT_HERE", "1", 1) == 0, "current namespace");
        file(path / "config.json", "{\"moduledir\":\"" + (path / "custom-modules").string() +
                                       "\",\"mount_backend\":\"none\"}");
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    void module(const std::string& id, const std::string& partition = "",
                const std::string& marker = "") const {
        const auto directory = path / "custom-modules" / id;
        file(directory / "module.prop", "id=" + id + "\nname=" + id + "\n");
        if (!partition.empty())
            file(directory / partition / "etc/payload", "module content");
        if (!marker.empty())
            file(directory / marker, "");
    }
};

std::set<std::string> listed(bool inactive) {
    const kagami::Argument args[] = {"module", "list", "--include-inactive"};
    std::vector<char> out(1024 * 1024), err(4096);
    kagami::output::Context destination{{out.data(), out.size()}, {err.data(), err.size()}};
    const auto result = kagami::run_command_capture({args, inactive ? 3U : 2U}, destination);
    require(result.exit_code == 0, "module list failed: " + std::string(result.stderr_text));
    kagami::JsonValue json;
    std::string error;
    require(kagami::parse_json(std::string(result.stdout_text), json, error), error);
    std::set<std::string> ids;
    for (const auto& module : json.object_value.at("modules").array_value)
        ids.insert(module.object_value.at("id").string_value);
    return ids;
}
void test_inventory() {
    Fixture fixture;
    fixture.module("script");
    fixture.module("old_empty_record");
    fixture.module("payload", "system");
    fixture.module("disabled", "system", "disable");
    fixture.module("removed", "vendor", "remove");
    fixture.module("skipped", "product", "skip_mount");
    fixture.module("hot_unmounted", "system");
    file(fixture.path / "run/hot_unmounted/hot_unmounted", "");
    {
        runtime::Operation operation;
        require(runtime::assign_modules(
                    {{"script", "none"}, {"old_empty_record", "magic"}, {"retained", "magic"}}),
                "assign inventory");
        const auto attachment = runtime::capture_mount("/proc", {"retained"});
        require(attachment.captured, "capture existing attachment without mounting anything");
        require(runtime::record_mounts("magic", {attachment}, true), "publish evidence");
        operation.finish();
    }
    require(listed(false) == std::set<std::string>{"payload"}, "default CLI list compatibility");
    const auto extended = listed(true);
    require(extended == std::set<std::string>{"payload", "disabled", "removed", "skipped",
                                              "hot_unmounted", "retained"},
            "extended list must exclude scripts and empty bookkeeping records");
    // Missing runtime evidence must not hide installed mount payloads or add scripts.
    fs::remove(fixture.path / "run/module_runtime/state");
    require(listed(true) ==
                std::set<std::string>{"payload", "disabled", "removed", "skipped", "hot_unmounted"},
            "payload inventory remains usable with a missing runtime record");
}

void test_partition_scan() {
    Fixture fixture;
    const auto modules = fixture.path / "custom-modules";
    const auto system = fixture.path / "system-root";
    runtime::Snapshot snapshot;
    snapshot.modules["active"] = {"mounted", "magic", {}, {}, {}};
    snapshot.modules["partial"] = {"partial", "overlay", "incomplete", {}, {}};
    snapshot.modules["inactive"] = {"unmounted", "magic", {}, {}, {}};
    snapshot.modules["unknown"] = {};
    snapshot.modules["removed"] = {"mounted", "magic", {}, {}, {}};
    fixture.module("active", "vendor_dlkm");
    fixture.module("partial", "my_product");
    fixture.module("inactive", "inactive_partition", "disable");
    fixture.module("unknown", "unknown_partition");
    fixture.module("script", "zygisk");
    file(modules / "removed/removed_partition/payload", "no module metadata");
    file(modules / "active/system/system_dlkm/lib/payload", "nested partition");
    file(modules / "active/odm_dlkm/lib/payload", "partition alias");

    std::vector<runtime::MountIdentity> mounts;
    std::string table;
    unsigned mount_id = 100;
    const auto partition = [&](const std::string& name) {
        fs::create_directories(system / name);
        table +=
            std::to_string(mount_id++) + " 1 253:0 / /" + name + " ro - erofs /dev/block/dm-0 ro\n";
    };
    for (const auto* name :
         {"vendor_dlkm", "system_dlkm", "vendor/odm_dlkm", "my_product", "inactive_partition",
          "unknown_partition", "removed_partition", "empty_partition", "manual_partition"})
        partition(name);
    fs::create_directory_symlink(system / "vendor/odm_dlkm", system / "odm_dlkm");
    fs::create_directories(modules / "active/empty_partition");
    table += "200 1 253:1 / /absent_partition ro - ext4 /dev/block/dm-1 ro\n"
             "201 1 0:40 / /tmpfs_dir rw - tmpfs tmpfs rw\n"
             "202 1 253:2 /directory /bind_dir ro - ext4 /dev/block/dm-2 ro\n"
             "203 1 0:41 / /overlay_only ro - overlay KSU ro\n"
             "204 1 0:42 / /vendor_dlkm ro - overlay KSU ro\n";
    for (const auto* name : {"zygisk", "lkm", "webroot", "META-INF", "ordinary_dir", "tmpfs_dir",
                             "bind_dir", "overlay_only"}) {
        file(modules / "active" / name / "payload", "not a partition");
        fs::create_directories(system / name);
    }
    file(modules / "active/absent_partition/payload", "no system target");
    std::string error;
    require(runtime::parse_mountinfo(table, mounts, error), error);
    const kagami::Argument configured[] = {
        "vendor_dlkm", "manual_partition", "zygisk",          "lkm",
        "webroot",     "ordinary_dir",     "absent_partition"};
    kagami::mount::PartitionScan found;
    require(kagami::mount::discover_module_partitions(modules, system, snapshot, mounts, configured,
                                                      found, error),
            error);
    require(found.discovered ==
                std::set<std::string>{"vendor_dlkm", "odm_dlkm", "system_dlkm", "my_product"},
            "only mounted/partial payloads with real system partitions are discovered");
    require(found.invalid == std::set<std::string>{"zygisk", "lkm", "webroot", "ordinary_dir",
                                                   "absent_partition"},
            "invalid old entries are removable while valid manual partitions survive");

    // A failed directory read must not publish the successfully scanned subset.
    file(modules / "active/bad_alias/payload", "unreadable system target");
    fs::create_directory_symlink("bad_alias", system / "bad_alias");
    const auto previous = found;
    require(!kagami::mount::discover_module_partitions(modules, system, snapshot, mounts,
                                                       configured, found, error) &&
                !error.empty() && found.discovered == previous.discovered &&
                found.invalid == previous.invalid,
            "failed partition reads preserve the caller's previous result");
}

void test_partition_command() {
    Fixture fixture;
    const auto decoy = fixture.path / "default-modules";
    file(decoy / "unrelated/lkm/payload", "default path must not be scanned");
    require(setenv("KAGAMI_MODULES_DIR", decoy.c_str(), 1) == 0, "default module directory");
    fixture.module("script", "zygisk");
    const kagami::Argument args[] = {"config", "sync-partitions"};
    std::vector<char> out(4096), err(4096);
    kagami::output::Context destination{{out.data(), out.size()}, {err.data(), err.size()}};
    auto result = kagami::run_command_capture(args, destination);
    require(result.exit_code == 0 && result.stdout_text == "No new partitions\n",
            "auto scan does not enumerate unrelated directories or unmounted modules");
    file(fixture.path / "config.json", "{\"moduledir\":\"" +
                                           (fixture.path / "custom-modules").string() +
                                           "\",\"partitions\":[\"system\",\"zygisk\",\"lkm\"]}");
    const kagami::Argument json_args[] = {"config", "sync-partitions", "--json"};
    result = kagami::run_command_capture(json_args, destination);
    kagami::JsonValue scanned;
    std::string error;
    require(result.exit_code == 0 &&
                kagami::parse_json(std::string(result.stdout_text), scanned, error),
            "read structured partition scan");
    require(scanned.object_value.at("partitions").array_value.empty() &&
                scanned.object_value.at("invalid").array_value.size() == 2,
            "retain built-in targets and report obsolete resource entries");
    file(fixture.path / "config.json", "{");
    result = kagami::run_command_capture(args, destination);
    require(result.exit_code != 0 && result.stdout_text.empty(),
            "invalid config must not silently scan the default module directory");
    require(unsetenv("KAGAMI_MODULES_DIR") == 0, "restore default module directory");
}
void test_module_apply() {
    Fixture fixture;
    fixture.module("payload", "system");
    const kagami::Argument args[] = {"module", "apply", "payload"};
    std::vector<char> out(4096), err(4096);
    kagami::output::Context destination{{out.data(), out.size()}, {err.data(), err.size()}};
    auto result = kagami::run_command_capture(args, destination);
    require(result.exit_code == 0 && result.stdout_text == "{\"applied\":false}\n",
            "unassigned modules must wait for boot instead of migrating backends");
    {
        runtime::Operation operation;
        require(runtime::assign_modules({{"payload", "magic"}}), "assign non-Kasumi backend");
        operation.finish();
    }
    result = kagami::run_command_capture(args, destination);
    require(result.exit_code == 0 && result.stdout_text == "{\"applied\":false}\n",
            "Magic ownership must not become live Kasumi ownership");
    file(fixture.path / "config.json", "{");
    result = kagami::run_command_capture(args, destination);
    require(result.exit_code != 0 && result.stdout_text.empty(), "reject corrupt configuration");
    file(fixture.path / "not-a-directory", "file");
    file(fixture.path / "config.json",
         "{\"moduledir\":\"" + (fixture.path / "not-a-directory").string() + "\"}");
    result = kagami::run_command_capture(args, destination);
    require(result.exit_code != 0 && result.stdout_text.empty(), "report module scan failure");
    const kagami::Argument invalid[] = {"module", "apply", "../payload"};
    result = kagami::run_command_capture(invalid, destination);
    require(result.exit_code != 0, "reject invalid module identifier");
}
}  // namespace
int main() {
    try {
        test_inventory();
        test_partition_scan();
        test_partition_command();
        test_module_apply();
        std::cout << "module list fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
