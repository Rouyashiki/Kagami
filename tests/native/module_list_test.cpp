#include "core/command.hpp"
#include "core/json_value.hpp"
#include "mount/module_runtime.hpp"

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
}  // namespace
int main() {
    try {
        test_inventory();
        std::cout << "module list fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
