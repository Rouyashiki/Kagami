#include "core/json.hpp"
#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/kasumi.hpp"
#include "mount/module_runtime.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
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

extern "C" int __wrap_mount(const char* source, const char* target, const char* type,
                            unsigned long flags, const void* data) {
    if (source || std::string(target) != "/" || type || flags != (MS_REC | MS_PRIVATE) || data)
        std::abort();
    return 0;
}

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
void whiteout(const fs::path& path) {
    fs::create_directories(path.parent_path());
    require(mknod(path.c_str(), S_IFCHR | 0600, makedev(0, 0)) == 0,
            "create whiteout " + path.string());
}

// Replace only the kernel client boundary. The real backend compiles the
// module trees, records ownership and reads the real filesystem/sidecar.
struct Kernel {
    std::map<std::string, std::string> adds;
    std::vector<runtime::RuleEvidence> merges;
    std::set<std::string> hides;
    std::map<std::string, unsigned> add_flags, hide_flags;
    std::vector<std::string> add_requests;
    std::string rejected_source;
    bool enabled = true;
    bool atomic = true;
    int transactions = 0;
    std::vector<std::string> xattrs;
    kagami::kasumi::TransactionConfiguration configuration;
    int available_features =
        KSM_FEATURE_MOUNT_HIDE | KSM_FEATURE_MAPS_SPOOF | KSM_FEATURE_STATFS_SPOOF;
    bool configuration_failure = false;
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
bool has_payload_mapping(const fs::path& path) {
    const auto supplies = [&](const std::string& target, const std::string& source) {
        const auto relative = path.lexically_relative(kernel_add_key(target));
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            return false;
        std::error_code ec;
        const auto payload = relative == "." ? fs::path(source) : fs::path(source) / relative;
        return fs::exists(fs::symlink_status(payload, ec));
    };
    return std::any_of(kernel.adds.begin(), kernel.adds.end(),
                       [&](const auto& add) { return supplies(add.first, add.second); }) ||
           std::any_of(kernel.merges.begin(), kernel.merges.end(),
                       [&](const auto& merge) { return supplies(merge.path, merge.source); });
}
runtime::Snapshot snapshot() {
    runtime::KasumiObservation live;
    live.ok = true;
    live.enabled = kernel.enabled;
    live.text = "Kasumi Protocol: " + std::to_string(KSM_PROTOCOL_VERSION) +
                "\nKasumi Enabled: " + std::string(kernel.enabled ? "1\n" : "0\n");
    for (const auto& [path, source] : kernel.adds)
        live.text +=
            "add " + path + " " + source + " " + std::to_string(kernel.add_flags[path]) + "\n";
    for (const auto& rule : kernel.merges)
        live.text +=
            "merge " + rule.path + " " + rule.source + " " + std::to_string(rule.flags) + "\n";
    for (const auto& path : kernel.hides)
        live.text += "hide " + path + " " + std::to_string(kernel.hide_flags[path]) + "\n";
    return runtime::read_snapshot(live);
}
void state(const std::string& id, const std::string& expected) {
    const auto actual = snapshot().modules.at(id);
    require(actual.state == expected, id + ": " + runtime::summary_json(actual));
    require(actual.backend == "kasumi", "keep actual Kasumi backend");
}
struct TestRule {
    std::string path, mode;
    bool bypass = false;
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
                      << ",\"mode\":" << kagami::json_quote(rule.mode)
                      << ",\"bypass\":" << (rule.bypass ? "true}" : "false}");
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
    require(fixture.mount({"z"}), "publish original generation");
    kernel.rejected_source = fixture.payload("a", "alias").string();
    require(!fixture.mount({"a", "z"}), "report replacement failure");
    require(kernel.adds.size() == 1 &&
                kernel.adds.begin()->second == fixture.payload("z", "real").string(),
            "prior rule survives failure");
    require(kernel.transactions == 1, "failed batch does not publish a generation");
    state("a", "unknown");
    state("z", "unknown");
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
void test_bypass_scope() {
    Fixture fixture;
    file(fixture.payload("module", "normal"));
    file(fixture.payload("module", "global"));
    file(fixture.payload("module", "merged/file"));
    fs::create_directories(fixture.stock / "merged");
    const auto path = [&](const char* leaf) { return (fixture.stock / leaf).string(); };
    require(fixture.mount({"module"}, {{"module",
                                        {{path("global"), "kasumi", true},
                                         {path("merged"), "kasumi", true},
                                         {path("hidden"), "hide", true}}}}),
            "mixed bypass mount");
    require(kernel.add_flags.at(path("normal")) == 0, "ordinary sibling remains policy-following");
    require(kernel.add_flags.at(path("global")) == KSM_RULE_BYPASS_POLICY, "explicit ADD bypass");
    require(kernel.merges.size() == 1 && kernel.merges[0].flags == KSM_RULE_BYPASS_POLICY,
            "MERGE bypass reaches kernel");
    require(kernel.hide_flags.at(path("hidden")) == KSM_RULE_BYPASS_POLICY,
            "HIDE bypass reaches kernel");
    state("module", "mounted");
    kernel.add_flags[path("global")] = 0;
    state("module", "partial");
    kernel = {};
    std::ofstream(fixture.root / "module_bypass.json") << "{\"module\":\"on\"}";
    require(fixture.mount({"module"}), "module bypass mount");
    require(kernel.add_flags.at(path("normal")) == KSM_RULE_BYPASS_POLICY,
            "module bypass applies to default rule");
    require(kernel.merges[0].flags == KSM_RULE_BYPASS_POLICY, "module bypass applies to merge");
    state("module", "mounted");
}

void test_shared_user_hides() {
    Fixture fixture;
    const auto path = (fixture.stock / "hidden").string();
    file(fixture.payload("module", "payload"));
    require(fixture.mount({"module"}, {{"module", {{path, "hide", true}}}}),
            "mount global module hide");
    require(backend::record_replayable_mappings({fixture.module("module")}), "record boot owner");
    const auto edit = [&](bool remove, int flags = 0) {
        runtime::Operation operation;
        const bool ok = backend::update_legacy_user_hide(path, remove, flags);
        operation.finish(ok);
        return ok;
    };
    require(edit(false), "add ordinary user hide over global module hide");
    require(kernel.hide_flags.at(path) == KSM_RULE_BYPASS_POLICY, "keep global module flag");
    kernel.hide_flags[path] = 0;
    state("module", "partial");
    kernel.hide_flags[path] = KSM_RULE_BYPASS_POLICY;
    require(edit(false, KSM_RULE_BYPASS_POLICY) && edit(false), "toggle user bypass off");
    require(kernel.hide_flags.at(path) == KSM_RULE_BYPASS_POLICY,
            "user toggle retains module flag");
    file(fixture.module("module").path / "disable");
    std::ofstream(fixture.root / "config.json") << "{\"kasumi_enabled\":false}";
    std::ofstream(fixture.root / "module_rules.json") << "{}";
    kernel.enabled = false;
    require(edit(true), "remove user hide with pending module changes");
    require(kernel.hides.count(path) == 1 && kernel.hide_flags.at(path) == KSM_RULE_BYPASS_POLICY &&
                !kernel.enabled,
            "retain installed module hide without applying pending changes or enabling Kasumi");
    kernel.enabled = true;
    state("module", "mounted");
    std::ofstream(fixture.root / "user_hide_rules.json") << '[' << kagami::json_quote(path) << ']';
    {
        runtime::Operation operation;
        std::string error;
        require(backend::restore_persisted_hide_rules(error), error);
        operation.finish();
    }
    require(kernel.hide_flags.at(path) == KSM_RULE_BYPASS_POLICY, "restore retains module bypass");

    kernel = {};
    std::ofstream(fixture.root / "hide_bypass.json")
        << '{' << kagami::json_quote(path) << ":\"on\"}";
    require(fixture.mount({"module"}, {{"module", {{path, "hide"}}}}),
            "mount ordinary module hide with global user hide");
    state("module", "mounted");
    require(edit(false), "disable user bypass over ordinary module hide");
    require(kernel.hide_flags.at(path) == 0, "user bypass does not become module bypass");
    state("module", "mounted");
    require(edit(false, KSM_RULE_BYPASS_POLICY), "enable user bypass over ordinary module hide");
    state("module", "mounted");
    require(edit(true) && kernel.hides.count(path) == 1 && kernel.hide_flags.at(path) == 0,
            "removing global user hide retains ordinary module hide");
    state("module", "mounted");

    {
        runtime::Operation operation;
        require(runtime::assign_modules({}, "kasumi"), "withdraw hot-unmounted module evidence");
        kernel = {};
        kagami::mount::ModuleRules rules;
        require(backend::mount_modules({}, fixture.config, rules),
                "restore user hides without modules");
        operation.finish();
    }
    require(edit(true) && kernel.hides.count(path) == 0,
            "hot-unmounted module no longer retains the user hide");
    require(fixture.mount({"module"}, {{"module", {{path, "hide"}}}}), "restore module ownership");

    std::ofstream(fixture.root / "run/module_runtime/state") << "invalid";
    require(!edit(true) && kernel.hides.count(path) == 1,
            "unreadable module ownership must not delete the live rule");
    backend::clear_replayable_mappings();
    require(
        edit(false, KSM_RULE_BYPASS_POLICY) && kernel.hide_flags.at(path) == KSM_RULE_BYPASS_POLICY,
        "unowned hide retains direct updates");
    require(edit(true) && kernel.hides.count(path) == 0, "unowned hide retains direct removal");
}
void test_merged_whiteouts() {
    Fixture fixture;
    const auto browser = fixture.stock / "priv-app/Browser";
    const auto nested = fixture.stock / "priv-app/Nested/Removed";
    file(nested);
    file(fixture.stock / "priv-app/Keep");
    fs::create_symlink("Keep", browser);
    for (const auto* id : {"first", "second"}) {
        const auto whiteout = fixture.payload(id, "priv-app/Browser");
        fs::create_directories(whiteout.parent_path());
        require(mknod(whiteout.c_str(), S_IFCHR | 0600, makedev(0, 0)) == 0,
                "create whiteout fixture");
    }
    const auto nested_whiteout = fixture.payload("first", "priv-app/Nested/Removed");
    fs::create_directories(nested_whiteout.parent_path());
    require(mknod(nested_whiteout.c_str(), S_IFCHR | 0600, makedev(0, 0)) == 0,
            "create nested whiteout");
    file(fixture.payload("first", "priv-app/Added"));
    const auto added_whiteout = fixture.payload("first", "priv-app/New/Removed");
    fs::create_directories(added_whiteout.parent_path());
    require(mknod(added_whiteout.c_str(), S_IFCHR | 0600, makedev(0, 0)) == 0,
            "create whiteout under a module-only directory");
    file(fixture.payload("first", "priv-app/New/Keep"));
    require(fixture.mount({"first", "second"},
                          {{"first", {{(fixture.stock / "priv-app").string(), "kasumi", true}}}}),
            "mount merged whiteouts");
    require(kernel.merges.empty() && kernel.adds.size() == 2,
            "expand whiteout subtrees before selecting the final hides");
    require(kernel.hides ==
                std::set<std::string>{browser.string(), nested.string(), added_whiteout.string()},
            "hide existing names and module-only backing entries without following the leaf");
    require(kernel.hide_flags.at(browser.string()) == KSM_RULE_BYPASS_POLICY &&
                kernel.hide_flags.at(nested.string()) == KSM_RULE_BYPASS_POLICY &&
                kernel.hide_flags.at(added_whiteout.string()) == KSM_RULE_BYPASS_POLICY,
            "retain inherited whiteout bypass flags");
    state("first", "mounted");
    state("second", "mounted");
    require(
        backend::record_replayable_mappings({fixture.module("first"), fixture.module("second")}),
        "record whiteout owners");
    {
        runtime::Operation operation;
        const bool ok = backend::update_legacy_user_hide(browser.string(), false) &&
                        backend::update_legacy_user_hide(browser.string(), true);
        operation.finish(ok);
        require(ok, "edit a user hide over the module whiteout");
    }
    require(kernel.hides.count(browser.string()) == 1 &&
                kernel.hide_flags.at(browser.string()) == KSM_RULE_BYPASS_POLICY,
            "removing a user hide retains the module-owned whiteout");
}
void test_new_directory_whiteout() {
    Fixture fixture;
    const auto whiteout = fixture.payload("module", "New/Removed");
    fs::create_directories(whiteout.parent_path());
    require(mknod(whiteout.c_str(), S_IFCHR | 0600, makedev(0, 0)) == 0,
            "create unmerged whiteout");
    file(fixture.payload("module", "New/Keep"));
    require(fixture.mount({"module"}), "mount an entirely new directory with whiteout");
    require(kernel.merges.empty() && kernel.adds.size() == 1 &&
                kernel.hides == std::set<std::string>{whiteout.string()},
            "bind whiteout to its backing parent when no merge exists");
    state("module", "mounted");
}
void test_whiteout_priority() {
    for (const auto* leaf : {"entry", "merged/entry", "new/entry"}) {
        for (const bool higher_whiteout : {false, true}) {
            Fixture fixture;
            const auto path = fixture.stock / leaf;
            if (std::string_view(leaf) != "new/entry")
                file(path);
            whiteout(fixture.payload(higher_whiteout ? "a" : "z", leaf));
            file(fixture.payload(higher_whiteout ? "z" : "a", leaf));
            require(fixture.mount({"a", "z"}, {{"z", {{path.string(), "kasumi", true}}}}),
                    "mount competing whiteout and file");
            if (higher_whiteout) {
                const auto key = fs::exists(path.parent_path())
                                     ? path.string()
                                     : fixture.payload("a", leaf).string();
                require(
                    kernel.hides == std::set<std::string>{key} && kernel.hide_flags.at(key) == 0,
                    "higher whiteout wins without inheriting lower file bypass");
                if (!fs::exists(path.parent_path())) {
                    require(!has_payload_mapping(path),
                            "whiteout removes lower payload from the virtual directory");
                    state("z", "unmounted");
                }
            } else {
                require(kernel.hides.empty(), "higher file cancels the lower whiteout");
                require(kernel.hide_flags.empty(), "overridden whiteout retains no bypass");
                state("a", "mounted");
                state("z", "unmounted");
            }
        }
        Fixture fixture;
        const auto path = fixture.stock / leaf;
        if (std::string_view(leaf) != "new/entry")
            file(path);
        file(fixture.payload("z", leaf));
        whiteout(fixture.payload("m", leaf));
        file(fixture.payload("a", leaf));
        require(fixture.mount({"a", "m", "z"}), "mount file-whiteout-file sequence");
        require(kernel.hides.empty(), "last file cancels an intermediate whiteout");
        if (kernel.merges.empty())
            require(kernel.adds.at(path.string()) == fixture.payload("a", leaf).string(),
                    "highest file owns the effective ADD");
        else
            require(kernel.merges.back().source == fixture.payload("a", "merged").string(),
                    "highest directory is merged last");
        state("a", "mounted");
        state("m", "unmounted");
    }
}
void test_whiteout_explicit_hides() {
    Fixture fixture;
    const auto path = (fixture.stock / "entry").string();
    file(path);
    whiteout(fixture.payload("z", "entry"));
    file(fixture.payload("a", "entry"));
    require(fixture.mount({"a", "z"}, {{"z", {{path, "hide", true}}}}),
            "mount explicit hide with a higher file");
    require(kernel.hides.count(path) == 1 && kernel.hide_flags.at(path) == KSM_RULE_BYPASS_POLICY,
            "explicit module hides remain independent of payload priority");
    kernel = {};
    std::ofstream(fixture.root / "user_hide_rules.json") << '[' << kagami::json_quote(path) << ']';
    require(fixture.mount({"a", "z"}), "mount higher file with persisted user hide");
    require(kernel.hides.count(path) == 1, "persisted user hides survive whiteout replacement");
    state("z", "unmounted");
}
void test_whiteout_alias_priority() {
    for (const bool same_link : {false, true}) {
        Fixture fixture;
        const auto alias = fixture.stock / "alias";
        const auto real = fixture.stock / "real";
        file(real);
        fs::create_symlink("real", alias);
        whiteout(fixture.payload("z", same_link ? "alias" : "real"));
        if (same_link) {
            const auto payload = fixture.payload("a", "alias");
            fs::create_directories(payload.parent_path());
            fs::create_symlink("real", payload);
        } else {
            file(fixture.payload("a", "alias"));
        }
        require(fixture.mount({"a", "z"}), "mount higher alias over whiteout");
        require(kernel.hides.empty(), "alias replacement cancels the effective lower whiteout");
        if (same_link)
            require(kernel.adds.empty(), "retain the existing matching stock alias");
        else
            require(kernel.adds.at(real.string()) == fixture.payload("a", "alias").string(),
                    "replacement uses the kernel's resolved ADD key");
    }
}
void test_whiteout_special_siblings() {
    Fixture fixture;
    fs::create_directory(fixture.stock / "merged");
    whiteout(fixture.payload("module", "merged/removed"));
    const auto fifo = fixture.payload("module", "merged/fifo");
    const auto device = fixture.payload("module", "merged/null");
    require(mkfifo(fifo.c_str(), 0600) == 0 &&
                mknod(device.c_str(), S_IFCHR | 0600, makedev(1, 3)) == 0,
            "create special siblings");
    require(fixture.mount({"module"}), "expand mixed whiteout and special sources");
    require(kernel.merges.empty() && kernel.adds.size() == 2 &&
                kernel.adds.at((fixture.stock / "merged/fifo").string()) == fifo.string() &&
                kernel.adds.at((fixture.stock / "merged/null").string()) == device.string(),
            "expansion retains non-whiteout special sources");
}
void test_virtual_whiteouts_in_lower_merge() {
    for (const bool alias : {false, true}) {
        Fixture fixture;
        fs::create_directories(fixture.stock / "existing/unrelated");
        if (alias)
            fs::create_directory_symlink("existing", fixture.stock / "alias");
        const std::string lower_root = alias ? "alias" : "existing";
        const auto lower = [&](const std::string& path) {
            std::string relative = lower_root;
            relative += '/';
            relative += path;
            return fixture.payload("z", relative);
        };
        for (int i = 0; i < 10; ++i) {
            const auto leaf = "new/entry" + std::to_string(i);
            file(lower(leaf));
            whiteout(fixture.payload("a", "existing/" + leaf));
        }
        file(lower("new/removed/child"));
        whiteout(fixture.payload("a", "existing/new/removed"));
        file(lower("new/keep"));
        file(lower("unrelated/keep"));
        file(fixture.payload("a", "existing/new/higher"));
        require(fixture.mount({"a", "z"},
                              {{"z", {{(fixture.stock / lower_root).string(), "kasumi", true}}}}),
                "mount whiteouts over a lower directory merge");
        for (int i = 0; i < 10; ++i)
            require(!has_payload_mapping(fixture.stock / "existing/new" /
                                         ("entry" + std::to_string(i))),
                    "lower merge cannot reintroduce a whiteout target");
        require(!has_payload_mapping(fixture.stock / "existing/new/removed/child"),
                "directory whiteout removes lower descendants");
        require(has_payload_mapping(fixture.stock / "existing/new/keep") &&
                    has_payload_mapping(fixture.stock / "existing/unrelated/keep") &&
                    has_payload_mapping(fixture.stock / "existing/new/higher"),
                "preserve unrelated lower and higher payloads");
        require(kernel.add_flags.at((fixture.stock / "existing/new/keep").string()) ==
                        KSM_RULE_BYPASS_POLICY &&
                    kernel.add_flags.at((fixture.stock / "existing/new/higher").string()) == 0,
                "expanded mappings retain each module's policy flags");
        require(kernel.merges.size() == 1 &&
                    kernel.merges.front().path == (fixture.stock / "existing/unrelated").string(),
                "unaffected subtrees retain bulk merging");
        state("a", "mounted");
        state("z", "mounted");
    }
}
}  // namespace

namespace kagami::mount {
ModuleModes load_module_modes(int& error, const char* name) {
    ModuleModes modes;
    (void)modes.prepare_modes((runtime_data_dir() / name).c_str(), error);
    return modes;
}
}  // namespace kagami::mount

namespace kagami::mount::overlay {
bool xattr_hiding_paths(const Config& config, std::vector<std::string>& paths) {
    paths = config.enable_overlay_xattr_hide ? std::vector<std::string>{"/fixture-overlay"}
                                             : std::vector<std::string>{};
    return true;
}
}  // namespace kagami::mount::overlay

namespace kagami::kasumi {
ControlCapabilities control_capabilities() {
    ControlCapabilities result;
    result.ok = kernel.atomic;
    result.last_errno = kernel.atomic ? 0 : EPROTO;
    result.protocol = kernel.atomic ? KSM_PROTOCOL_VERSION : 18;
    result.capabilities = kernel.atomic ? KSM_CAP_ATOMIC_PATH_RULES | KSM_CAP_ATOMIC_SETTINGS |
                                              KSM_CAP_ATOMIC_POLICY | KSM_CAP_ATOMIC_MAPS |
                                              KSM_CAP_ATOMIC_KSTAT
                                        : 0;
    return result;
}

FeatureCapabilities feature_capabilities() {
    FeatureCapabilities result;
    result.ok = true;
    result.bitmask = kernel.available_features;
    return result;
}
std::vector<std::string> feature_names(int) {
    return {"unsupported-fixture-feature"};
}
bool commit_transaction(const std::vector<PathRule>& rules,
                        const TransactionConfiguration& configuration, std::uint32_t flags) {
    const auto previous = kernel;
    if (kernel.configuration_failure) {
        errno = ENOMEM;
        return false;
    }
    if (flags & KSM_TX_REPLACE_PATH_RULES)
        clear_rules();
    if (flags & (KSM_TX_REPLACE_PATH_RULES | KSM_TX_REPLACE_XATTR_RULES))
        kernel.xattrs.clear();
    for (const auto& rule : rules) {
        bool ok = false;
        switch (rule.operation) {
        case PathOperation::Redirect:
            ok = add_rule(rule.path, rule.source, rule.flags);
            break;
        case PathOperation::Merge:
            ok = add_merge_rule(rule.path, rule.source, rule.flags);
            break;
        case PathOperation::Hide:
            ok = hide_path(rule.path, rule.flags);
            break;
        case PathOperation::Delete:
            ok = delete_rule(rule.path);
            break;
        case PathOperation::HideXattrs:
            kernel.xattrs.push_back(rule.path);
            ok = true;
            break;
        }
        if (!ok) {
            kernel = previous;
            return false;
        }
    }
    kernel.configuration = configuration;
    if (configuration.fields & KSM_TX_CONFIG_SETTINGS)
        kernel.enabled = configuration.enabled;
    ++kernel.transactions;
    return true;
}

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
bool set_mount_hide(bool) {
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
    require((type & ~KSM_RULE_FLAGS_MASK) == 0, "backend ADD flags");
    kernel.add_requests.push_back(target);
    if (source == kernel.rejected_source) {
        errno = EIO;
        return false;
    }
    kernel.adds[kernel_add_key(target)] = source;
    kernel.add_flags[kernel_add_key(target)] = type;
    return true;
}
bool add_merge_rule(const std::string& target, const std::string& source, int flags) {
    kernel.merges.push_back({"MERGE", target, source, {}, static_cast<unsigned>(flags)});
    return true;
}
bool hide_path(const std::string& path, int flags) {
    const auto parent = resolved(fs::path(path).parent_path().string());
    if (parent.empty()) {
        errno = ENOENT;
        return false;
    }
    const auto key = (fs::path(parent) / fs::path(path).filename()).string();
    kernel.hides.insert(key);
    kernel.hide_flags[key] = flags;
    return true;
}
bool delete_rule(const std::string& path) {
    kernel.hides.erase(path);
    kernel.hide_flags.erase(path);
    return true;
}
}  // namespace kagami::kasumi

namespace {
void test_atomic_rebuild() {
    Fixture fixture;
    const auto target = fixture.stock / "entry";
    const auto payload = fixture.payload("module", "entry");
    file(target);
    file(payload);
    require(fixture.mount({"module"}), "publish initial API 19 mapping");
    bool atomic = true;
    std::string error;
    require(backend::prepare_rebuild(fixture.config, error, atomic) && atomic,
            "negotiate atomic rebuild");
    require(kernel.enabled && kernel.adds.size() == 1, "prepare retains current view");
    kernel.rejected_source = payload.string();
    require(!fixture.mount({"module"}), "reject failed atomic replacement");
    require(kernel.enabled && kernel.adds.at(target.string()) == payload.string() &&
                backend::is_active() && kernel.transactions == 1,
            "failed transaction retains old rules and active marker");

    kernel.rejected_source.clear();
    fixture.config.enable_overlay_xattr_hide = true;
    std::ofstream(fixture.root / "user_hide_rules.json")
        << '[' << kagami::json_quote(target.string()) << ']';
    require(fixture.mount({"module"}), "commit mappings, hides and xattrs together");
    require(kernel.transactions == 2 && kernel.hides.count(target.string()) == 1 &&
                kernel.xattrs == std::vector<std::string>{"/fixture-overlay"},
            "atomic publication includes persisted hides and overlay xattrs");
    require(kernel.configuration.fields == (KSM_TX_CONFIG_SETTINGS | KSM_TX_CONFIG_POLICY |
                                            KSM_TX_CONFIG_MAPS | KSM_TX_CONFIG_KSTAT),
            "full rebuild publishes settings, policy and clears maps");
    require(kernel.configuration.enabled == fixture.config.kasumi_enabled &&
                !(kernel.configuration.features & KSM_FEATURE_SELINUX_FIX),
            "settings include desired enable state and supported feature mask");
    kasumi_transaction_kstat_rule kstat{};
    kstat.fields = KSM_KSTAT_FIELDS_ALL;
    kstat.match_flags = KSM_MATCH_ANY_DEVICE;
    kstat.target_ino = UINT64_C(9007199254740993);
    kstat.spoofed_ino = UINT64_MAX;
    fixture.config.kasumi_kstat_rules = {&kstat, 1};
    bool applied = false;
    fixture.config.enable_overlay_xattr_hide = false;
    fixture.config.enable_kernel_debug = true;
    require(backend::apply_atomic_config(fixture.config, error, applied) && applied,
            "configuration-only atomic publication");
    require(kernel.adds.at(target.string()) == payload.string() && kernel.xattrs.empty() &&
                kernel.configuration.debug && !(kernel.configuration.fields & KSM_TX_CONFIG_MAPS) &&
                (kernel.configuration.fields & KSM_TX_CONFIG_KSTAT),
            "configuration-only keeps module mappings and maps while clearing xattrs");
    require(kernel.configuration.kstat.size() == 1 &&
                kernel.configuration.kstat[0].fields == KSM_KSTAT_FIELDS_ALL &&
                kernel.configuration.kstat[0].match_flags == KSM_MATCH_ANY_DEVICE &&
                kernel.configuration.kstat[0].target_ino == kstat.target_ino &&
                kernel.configuration.kstat[0].spoofed_ino == UINT64_MAX &&
                kernel.configuration.kstat[0].spoofed_size == 0,
            "configuration apply carries nonempty exact kstat fields and explicit zeros");
    kernel.configuration_failure = true;
    fixture.config.enable_kernel_debug = false;
    fixture.config.kasumi_kstat_rules = {};
    require(!backend::apply_atomic_config(fixture.config, error, applied) && applied,
            "configuration failure propagates");
    require(kernel.configuration.debug && kernel.configuration.kstat.size() == 1 &&
                kernel.adds.at(target.string()) == payload.string(),
            "configuration failure keeps complete previous generation");
    kernel.configuration_failure = false;
    require(fixture.mount({}), "commit empty module set");
    require(kernel.transactions == 4 && kernel.adds.empty() && kernel.merges.empty() &&
                kernel.hides.count(target.string()) == 1,
            "empty replacement retires mappings and preserves persisted hides");

    require(kernel.configuration.kstat.empty(),
            "successful full rebuild clears configured kstat table");
    fixture.config.kasumi_kstat_rules = {&kstat, 1};
    require(fixture.mount({}), "full rebuild installs nonempty kstat table");
    require(kernel.configuration.kstat.size() == 1 &&
                kernel.configuration.kstat[0].target_ino == kstat.target_ino,
            "full rebuild preserves nonempty kstat values");
    kernel.atomic = false;
    const auto transactions = kernel.transactions;
    require(!backend::prepare_rebuild(fixture.config, error, atomic) && errno == EPROTO &&
                !atomic && kernel.enabled && kernel.transactions == transactions,
            "API 18 preflight rejects nonempty kstat before reset");
    require(!backend::apply_atomic_config(fixture.config, error, applied) && errno == EPROTO &&
                !applied && kernel.enabled,
            "API 18 config apply rejects nonempty kstat before legacy changes");
    require(!backend::mount_modules({}, fixture.config, {}) && errno == EPROTO && kernel.enabled,
            "API 18 direct rebuild rejects nonempty kstat before legacy changes");
    fixture.config.kasumi_kstat_rules = {};
    require(!backend::prepare_rebuild(fixture.config, error, atomic) && !atomic && errno == EPROTO,
            "API 18 rejects empty configuration too");
    require(kernel.enabled && kernel.transactions == transactions,
            "protocol mismatch preserves state");
}
}  // namespace

int main() {
    try {
        test_bypass_scope();
        test_shared_user_hides();
        test_leaf_alias();
        test_missing_and_dangling();
        test_alias_winners();
        test_same_owner_aliases();
        test_failed_alias_replacement();
        test_merge_and_hide_aliases();
        test_merged_whiteouts();
        test_new_directory_whiteout();
        test_whiteout_priority();
        test_whiteout_explicit_hides();
        test_whiteout_alias_priority();
        test_whiteout_special_siblings();
        test_virtual_whiteouts_in_lower_merge();
        test_atomic_rebuild();
        std::cout << "Kasumi runtime alias fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
