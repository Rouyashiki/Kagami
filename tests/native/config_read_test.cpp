#include "core/json_value.hpp"
#include "core/prepared_input.hpp"
#include "kagami/config.hpp"

#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Fixture {
    fs::path path;
    Fixture() {
        char pattern[] = "/data/local/tmp/kagami-config-test-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "create config fixture");
        path = created;
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
void write_text(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    require(output.good(), "write " + path.string());
}
std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
bool read_config_text(const std::string& path, std::string& output, std::string& error,
                      bool allow_missing = false) {
    output.clear();
    kagami::PreparedInput input;
    if (!kagami::read_config_json(path, input, error, allow_missing))
        return false;
    output.assign(input.view());
    return true;
}
kagami::JsonValue document(const fs::path& path) {
    std::string input;
    std::string error;
    require(read_config_text(path.string(), input, error), "read valid config: " + error);
    kagami::JsonValue result;
    require(kagami::parse_json(input, result, error) && result.is_object(), "parse config object");
    return result;
}
const kagami::JsonValue& field(const kagami::JsonValue& root, const std::string& key) {
    const auto* result = root.find(key);
    require(result != nullptr, "missing config field " + key);
    return *result;
}
void test_missing() {
    Fixture fixture;
    const auto path = fixture.path / "config.json";
    std::string json = "stale";
    std::string error;
    require(!read_config_text(path.string(), json, error) && json.empty() && !error.empty(),
            "normal missing read must fail");
    require(read_config_text(path.string(), json, error, true), "allow missing defaults");
    require(!fs::exists(path), "default read must not create a file");
    kagami::PreparedConfig config;
    require(kagami::parse_config_json(json, config, error) && !config.lkm_autoload,
            "missing read returns actual defaults");
    require(kagami::merge_config_json(path.string(), "{\"debug\":true}", error),
            "merge can initialize absent config");
    auto saved = document(path);
    require(field(saved, "debug").bool_value && !field(saved, "lkm_autoload").bool_value,
            "initial merge retains default fields");
    const auto autoload_path = fixture.path / "autoload.json";
    require(kagami::update_lkm_autoload_config(autoload_path.string(), true, error),
            "dedicated autoload initializes absent config");
    require(field(document(autoload_path), "lkm_autoload").bool_value,
            "autoload initialization persisted");
    kagami::PolicyConfig policy;
    policy.owner = "kagami";
    policy.use_allow_uids = true;
    const std::uint32_t allow[] = {10001};
    policy.allow_uids = allow;
    const auto policy_path = fixture.path / "policy.json";
    require(kagami::update_policy_config(policy_path.string(), policy, error),
            "policy initializes absent config");
    saved = document(policy_path);
    require(field(field(saved, "policy"), "owner").string_value == "kagami" &&
                field(saved, "mount_backend").string_value == "auto",
            "policy initialization retains defaults");
}
void expect_all_updates_rejected(const fs::path& path) {
    std::string json = "stale";
    std::string error;
    require(!read_config_text(path.string(), json, error) && json.empty() && !error.empty(),
            "strict read rejects unreadable/invalid config");
    require(!read_config_text(path.string(), json, error, true) && json.empty() && !error.empty(),
            "allow_missing cannot mask an existing bad config");
    kagami::PreparedConfig config;
    config.mount_source = "sentinel";
    require(!kagami::read_config_file(path.string(), config, error) &&
                config.mount_source == "sentinel",
            "typed read failure preserves caller state");
    require(!kagami::merge_config_json(path.string(), "{\"debug\":true}", error) && !error.empty(),
            "merge cannot overwrite unreadable config");
    require(!kagami::update_lkm_autoload_config(path.string(), true, error) && !error.empty(),
            "autoload cannot overwrite unreadable config");
    require(!kagami::update_policy_config(path.string(), {}, error) && !error.empty(),
            "policy cannot overwrite unreadable config");
}
void test_invalid_documents() {
    Fixture fixture;
    const auto path = fixture.path / "config.json";
    for (const auto& input : {std::string{}, std::string(" \n"), std::string("{"),
                              std::string("{\"debug\":false}\n{\"other\":1}\n"), std::string("[]"),
                              std::string("null"), std::string("42"), std::string("\"string\"")}) {
        write_text(path, input);
        expect_all_updates_rejected(path);
        require(read_text(path) == input, "invalid saved bytes must remain unchanged");
    }
    fs::remove(path);
    fs::create_directory(path);
    write_text(path / "sentinel", "directory content");
    expect_all_updates_rejected(path);
    require(fs::is_directory(path) && read_text(path / "sentinel") == "directory content",
            "directory read error cannot replace its contents");
}
void test_permission_denied() {
    Fixture fixture;
    const auto path = fixture.path / "config.json";
    const std::string original = "{\"unknown\":{\"keep\":true},\"debug\":false}\n";
    write_text(path, original);
    require(chmod(fixture.path.c_str(), 0777) == 0, "make parent writable to fixture child");
    require(chmod(path.c_str(), 0000) == 0, "deny config reads independently of parent writes");
    const auto child = fork();
    require(child >= 0, "fork permission fixture");
    if (child == 0) {
        try {
            if (geteuid() == 0) {
                require(setgroups(0, nullptr) == 0 && setgid(65534) == 0 && setuid(65534) == 0,
                        "drop child to nobody");
            }
            require(access(fixture.path.c_str(), W_OK | X_OK) == 0,
                    "parent directory remains writable");
            write_text(fixture.path / "child-can-write", "yes");
            std::string json;
            std::string error;
            require(!read_config_text(path.string(), json, error, true),
                    "permission denial is not missing config");
            require(error.rfind("open " + path.string() + ":", 0) == 0,
                    "failure belongs to config read, not the directory/lock");
            expect_all_updates_rejected(path);
            require(fs::exists(path.string() + ".lock"), "update acquired writable lock path");
            _exit(0);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            _exit(1);
        }
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "permission fixture child passed");
    require(chmod(path.c_str(), 0600) == 0, "restore fixture readability");
    require(read_text(path) == original, "unreadable config was never replaced with defaults");
}
void test_removed_mount_hide_mode() {
    Fixture fixture;
    const auto path = fixture.path / "config.json";
    const std::string original = "{\"kasumi_feature_config_version\":3,\"enable_mount_hide\":true,"
                                 "\"mount_hide_mode\":\"aggressive\",\"unknown\":42}\n";
    write_text(path, original);
    std::string error;
    kagami::PreparedConfig config;
    require(kagami::read_config_file(path.string(), config, error) && config.enable_mount_hide,
            "legacy mode does not disable ordinary mount hiding");
    require(!kagami::merge_config_json(path.string(), "{\"mount_hide_mode\":\"normal\"}", error),
            "removed mode rejects new writes");
    require(read_text(path) == original, "rejected mode update preserves original bytes");
    require(kagami::merge_config_json(path.string(), "{\"debug\":true}", error),
            "unrelated merge removes obsolete mode");
    const auto saved = document(path);
    require(saved.find("mount_hide_mode") == nullptr &&
                field(saved, "enable_mount_hide").bool_value &&
                field(saved, "unknown").number_value == 42,
            "obsolete mode removal preserves ordinary hiding and unrelated fields");
}
void test_preservation_and_migration() {
    Fixture fixture;
    const auto path = fixture.path / "config.json";
    const std::string future_value(9000, 'x');
    write_text(
        path,
        "{\"enable_hidexattr\":true,\"enable_stealth\":false,\"lkm_autoload\":true,"
        "\"overlay_dir\":\"/legacy/mirror\",\"overlay_img\":\"/legacy/image\","
        "\"overlay_img_size_mb\":256,\"future\":{\"keep\":[1,{\"key\":\"value\"}],\"large\":\"" +
            future_value + "\"}}\n");
    const auto original_future = kagami::stringify_json(field(document(path), "future"));
    std::string error;
    require(kagami::merge_config_json(path.string(), "{\"debug\":true}", error), "unrelated merge");
    require(field(document(path), "enable_hidexattr").bool_value,
            "unrelated save retains legacy state");
    require(kagami::merge_config_json(path.string(), "{\"enable_mount_hide\":false}", error),
            "migrate feature configuration");
    auto saved = document(path);
    require(saved.find("enable_hidexattr") == nullptr &&
                field(saved, "kasumi_feature_config_version").number_value == 3,
            "split migration replaces legacy feature switch");
    kagami::PreparedConfig config;
    require(kagami::read_config_file(path.string(), config, error), "read migrated config");
    require(config.debug && !config.enable_mount_hide && config.enable_overlay_xattr_hide &&
                config.enable_maps_spoof && config.enable_statfs_spoof &&
                config.enable_selinux_fix && config.enable_stealth,
            "migration preserves all unrelated enabled projections");
    require(config.mirror_dir == "/legacy/mirror" && config.mirror_img == "/legacy/image" &&
                config.mirror_img_size_mb == 256,
            "legacy storage compatibility remains intact");
    require(kagami::update_lkm_autoload_config(path.string(), false, error),
            "dedicated autoload update");
    saved = document(path);
    require(!field(saved, "lkm_autoload").bool_value && field(saved, "debug").bool_value,
            "autoload update preserves other saved settings");
    const auto before_rejected_merge = read_text(path);
    require(!kagami::merge_config_json(path.string(), "{\"lkm_autoload\":true}", error),
            "generic merge cannot bypass dedicated autoload path");
    require(read_text(path) == before_rejected_merge,
            "rejected autoload merge leaves bytes unchanged");
    kagami::PolicyConfig policy;
    policy.owner = "kagami";
    policy.use_deny_uids = true;
    const std::uint32_t deny[] = {10020, 10021};
    policy.deny_uids = deny;
    require(kagami::update_policy_config(path.string(), policy, error), "dedicated policy update");
    saved = document(path);
    require(kagami::stringify_json(field(saved, "future")) == original_future,
            "unknown nested fields and multi-buffer strings survive all updates");
    require(
        !field(saved, "lkm_autoload").bool_value && field(saved, "enable_maps_spoof").bool_value,
        "policy update retains autoload and migrated feature fields");
}
}  // namespace

int main() {
    try {
        test_missing();
        test_invalid_documents();
        test_permission_denied();
        test_removed_mount_hide_mode();
        test_preservation_and_migration();
        std::cout << "config read fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
