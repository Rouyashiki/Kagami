#include "core/json.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/kasumi.hpp"
#include "mount/module_runtime.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace runtime = kagami::mount::module_runtime;
namespace backend = kagami::mount::kasumi;
namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
void file(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << "fixture\n";
    require(output.good(), "write fixture " + path.string());
}

// Replace only the kernel client boundary. The real backend compiles the
// module trees, records ownership and reads the real filesystem/sidecar.
struct Kernel {
    std::map<std::string, std::string> adds;
    std::vector<runtime::RuleEvidence> merges;
    std::set<std::string> hides;
    std::vector<std::string> add_requests;
    std::string rejected_source;
    bool enabled = true;
} kernel;

std::string resolved(const std::string& path) {
    const std::unique_ptr<char, decltype(&std::free)> value(realpath(path.c_str(), nullptr),
                                                            std::free);
    return value ? std::string(value.get()) : std::string{};
}
std::string kernel_add_key(const std::string& path) {
    // API 17 ADD follows the whole destination first. On lookup failure it
    // resolves just the parent, preserving a missing or dangling final name.
    const auto full = resolved(path);
    if (!full.empty())
        return full;
    const auto parent = resolved(fs::path(path).parent_path().string());
    return parent.empty() ? path : (fs::path(parent) / fs::path(path).filename()).string();
}
runtime::Snapshot snapshot() {
    runtime::KasumiObservation live;
    live.ok = true;
    live.enabled = kernel.enabled;
    live.text =
        "Kasumi Protocol: 17\nKasumi Enabled: " + std::string(kernel.enabled ? "1\n" : "0\n");
    for (const auto& [path, source] : kernel.adds)
        live.text += "add " + path + " " + source + " 0\n";
    for (const auto& rule : kernel.merges)
        live.text += "merge " + rule.path + " " + rule.source + "\n";
    for (const auto& path : kernel.hides)
        live.text += "hide " + path + "\n";
    return runtime::read_snapshot(live);
}
void state(const std::string& id, const std::string& expected) {
    const auto actual = snapshot().modules.at(id);
    require(actual.state == expected, id + ": " + runtime::summary_json(actual));
    require(actual.backend == "kasumi", "keep actual Kasumi backend");
}
struct TestRule {
    std::string path, mode;
};
struct Fixture {
    fs::path root;
    fs::path stock;
    std::string partition;
    kagami::Argument partition_views[1];
    kagami::Config config;
    Fixture() {
        char pattern[] = "/data/local/tmp/kagami-kasumi-runtime-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "create fixture");
        root = created;
        stock = root / "stock";
        // Keep virtual paths within the fixture without requiring chroot or
        // mounts: /<partition> is the temporary stock tree.
        partition = stock.relative_path().string();
        partition_views[0] = kagami::Argument(partition);
        config.partitions = partition_views;
        fs::create_directory(stock);
        require(setenv("KAGAMI_DATA_DIR", root.c_str(), 1) == 0, "set runtime directory");
        require(setenv("KAGAMI_MOUNT_HERE", "1", 1) == 0, "use current namespace");
        kernel = {};
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    kagami::mount::ModuleEntry module(const std::string& id) const {
        return {id, root / "modules" / id};
    }
    fs::path payload(const std::string& id, const std::string& path) const {
        return module(id).path / partition / path;
    }
    bool mount(const std::vector<std::string>& ids,
               const std::map<std::string, std::vector<TestRule>>& rules = {}) {
        std::vector<kagami::mount::ModuleEntry> modules;
        std::vector<runtime::Assignment> assignments;
        for (const auto& id : ids) {
            modules.push_back(module(id));
            assignments.push_back({id, "kasumi"});
        }
        runtime::Operation operation;
        require(runtime::assign_modules(assignments), "assign Kasumi modules");
        std::ofstream saved(root / "module_rules.json");
        saved << '{';
        bool first = true;
        for (const auto& [id, entries] : rules) {
            if (!first)
                saved << ',';
            first = false;
            saved << kagami::json_quote(id) << ":[";
            bool first_rule = true;
            for (const auto& rule : entries) {
                if (!first_rule)
                    saved << ',';
                first_rule = false;
                saved << "{\"path\":" << kagami::json_quote(rule.path)
                      << ",\"mode\":" << kagami::json_quote(rule.mode) << '}';
            }
            saved << ']';
        }
        saved << '}';
        saved.close();
        kagami::mount::ModuleRules prepared;
        int error = 0;
        require(prepared.prepare_rules((root / "module_rules.json").c_str(), error),
                "prepare module rules");
        const bool ok = backend::mount_modules(modules, config, prepared);
        operation.finish(ok);
        return ok;
    }
};

void test_leaf_alias() {
    Fixture fixture;
    file(fixture.stock / "real");
    fs::create_symlink("real", fixture.stock / "alias");
    file(fixture.payload("module", "alias"));
    require(fixture.mount({"module"}), "mount leaf alias");
    require(kernel.adds.at((fixture.stock / "real").string()) ==
                fixture.payload("module", "alias").string(),
            "kernel follows leaf alias");
    require(kernel.add_requests == std::vector<std::string>{(fixture.stock / "alias").string()},
            "keep original ADD argument");
    state("module", "mounted");

    // Queries must use the key captured during installation, not resolve the
    // alias again after the filesystem has changed.
    file(fixture.stock / "other");
    fs::remove(fixture.stock / "alias");
    fs::create_symlink("other", fixture.stock / "alias");
    state("module", "mounted");
    kernel.enabled = false;
    state("module", "unmounted");
}
void test_missing_and_dangling() {
    Fixture fixture;
    fs::create_symlink("absent", fixture.stock / "dangling");
    file(fixture.payload("module", "dangling"));
    file(fixture.payload("module", "new-file"));
    require(fixture.mount({"module"}), "mount missing and dangling names");
    require(kernel.adds.count((fixture.stock / "dangling").string()) == 1 &&
                kernel.adds.count((fixture.stock / "new-file").string()) == 1,
            "retain unresolved leaf names");
    state("module", "mounted");
}
void test_alias_winners() {
    Fixture fixture;
    file(fixture.stock / "real");
    fs::create_symlink("real", fixture.stock / "alias");
    file(fixture.payload("a", "alias"));
    file(fixture.payload("z", "real"));
    require(fixture.mount({"a", "z"}), "mount competing aliases");
    require(kernel.adds.size() == 1 &&
                kernel.adds.begin()->second == fixture.payload("a", "alias").string(),
            "higher priority alias wins");
    state("a", "mounted");
    state("z", "unmounted");
}
void test_same_owner_aliases() {
    Fixture fixture;
    file(fixture.stock / "real");
    fs::create_symlink("real", fixture.stock / "alias");
    file(fixture.payload("module", "real"));
    file(fixture.payload("module", "alias"));
    require(fixture.mount({"module"}), "mount same-owner aliases");
    require(kernel.adds.size() == 1, "one effective ADD key");
    state("module", "mounted");
}
void test_failed_alias_replacement() {
    Fixture fixture;
    file(fixture.stock / "real");
    fs::create_symlink("real", fixture.stock / "alias");
    file(fixture.payload("a", "alias"));
    file(fixture.payload("z", "real"));
    kernel.rejected_source = fixture.payload("a", "alias").string();
    require(!fixture.mount({"a", "z"}), "report replacement failure");
    require(kernel.adds.size() == 1 &&
                kernel.adds.begin()->second == fixture.payload("z", "real").string(),
            "prior rule survives failure");
    state("a", "unmounted");
    state("z", "mounted");
}
void test_merge_and_hide_aliases() {
    Fixture fixture;
    fs::create_directory(fixture.stock / "directory");
    fs::create_directory_symlink("directory", fixture.stock / "alias");
    file(fixture.payload("module", "alias/file"));
    const auto path = (fixture.stock / "alias").string();
    require(fixture.mount({"module"}, {{"module", {{path, "hide"}}}}), "hide alias");
    require(kernel.hides.count(path) == 1, "HIDE keeps final alias");
    state("module", "mounted");
    kernel = {};
    require(fixture.mount({"module"}), "merge alias");
    require(kernel.merges.size() == 1 && kernel.merges.front().path == path,
            "MERGE keeps final alias");
    state("module", "mounted");
}
}  // namespace

namespace kagami::kasumi {
bool is_available() {
    return true;
}
bool module_loaded() {
    return true;
}
bool set_enabled(bool enabled) {
    kernel.enabled = enabled;
    return true;
}
bool set_debug(bool) {
    return true;
}
bool set_stealth(bool) {
    return true;
}
bool set_mount_hide(bool, MountHideMode) {
    return true;
}
bool set_maps_spoof(bool) {
    return true;
}
bool set_statfs_spoof(bool) {
    return true;
}
bool set_selinux_guard(bool) {
    return true;
}
bool replace_policy(PolicyOwner, std::uint32_t, bounded::Span<std::uint32_t>,
                    bounded::Span<std::uint32_t>) {
    return true;
}
bool clear_rules() {
    kernel.adds.clear();
    kernel.merges.clear();
    kernel.hides.clear();
    return true;
}
bool add_rule(const std::string& target, const std::string& source, int type) {
    require(type == 0, "backend ADD type");
    kernel.add_requests.push_back(target);
    if (source == kernel.rejected_source) {
        errno = EIO;
        return false;
    }
    kernel.adds[kernel_add_key(target)] = source;
    return true;
}
bool add_merge_rule(const std::string& target, const std::string& source) {
    kernel.merges.push_back({"MERGE", target, source, {}});
    return true;
}
bool hide_path(const std::string& path) {
    kernel.hides.insert(path);
    return true;
}
}  // namespace kagami::kasumi

int main() {
    try {
        test_leaf_alias();
        test_missing_and_dangling();
        test_alias_winners();
        test_same_owner_aliases();
        test_failed_alias_replacement();
        test_merge_and_hide_aliases();
        std::cout << "Kasumi runtime alias fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
