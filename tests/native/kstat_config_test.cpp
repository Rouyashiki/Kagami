#include "kagami/config.hpp"

#if defined(KAGAMI_TEST_EMBEDDED)
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/runtime.hpp"
#include "utils.hpp"
#endif

#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
namespace {
fs::path config_directory;
const std::string valid = R"({"kasumi_kstat_rules":[{
    "fields":511,"match_flags":1,"target_ino":"9007199254740993",
    "target_dev":"0","spoofed_ino":"18446744073709551615","spoofed_dev":"4294967295",
    "spoofed_nlink":"0","spoofed_size":"9007199254740995",
    "spoofed_atime_sec":"18446744073709551615","spoofed_atime_nsec":"999999999",
    "spoofed_mtime_sec":"9007199254740997","spoofed_mtime_nsec":"0",
    "spoofed_ctime_sec":"0","spoofed_ctime_nsec":"1",
    "spoofed_blksize":"4294967295","spoofed_blocks":"18446744073709551615"
},{"fields":0,"target_path":"/system/future-file"}]})";

#if defined(KAGAMI_TEST_EMBEDDED)
using TestConfig = kagami::Config;
#else
using TestConfig = kagami::PreparedConfig;
#endif

bool merge(const std::string& patch, std::string& error) {
#if defined(KAGAMI_TEST_EMBEDDED)
    return kagami::merge_config_json(patch, error);
#else
    return kagami::merge_config_json((config_directory / "config.json").string(), patch, error);
#endif
}
bool read(TestConfig& config, std::string& error) {
#if defined(KAGAMI_TEST_EMBEDDED)
    return kagami::read_config_file(config, error);
#else
    return kagami::read_config_file((config_directory / "config.json").string(), config, error);
#endif
}
std::string saved() {
    std::ifstream input(config_directory / "config.json");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void check_values(const kagami::Config& config) {
    assert(config.kasumi_kstat_rules.size() == 2);
    const auto& rule = config.kasumi_kstat_rules[0];
    assert(rule.fields == KSM_KSTAT_FIELDS_ALL && rule.match_flags == KSM_MATCH_ANY_DEVICE);
    assert(rule.target_ino == UINT64_C(9007199254740993) && rule.target_dev == 0);
    assert(rule.spoofed_ino == UINT64_MAX && rule.spoofed_dev == UINT32_MAX);
    assert(rule.spoofed_nlink == 0 && rule.spoofed_size == UINT64_C(9007199254740995));
    assert(rule.spoofed_atime_sec == UINT64_MAX && rule.spoofed_atime_nsec == 999999999);
    assert(rule.spoofed_mtime_sec == UINT64_C(9007199254740997) && rule.spoofed_mtime_nsec == 0);
    assert(rule.spoofed_ctime_sec == 0 && rule.spoofed_ctime_nsec == 1);
    assert(rule.spoofed_blksize == UINT32_MAX && rule.spoofed_blocks == UINT64_MAX);
    assert(config.kasumi_kstat_rules[1].fields == 0 &&
           std::string(config.kasumi_kstat_rules[1].target_pathname) == "/system/future-file");
}
}  // namespace

#if defined(KAGAMI_TEST_EMBEDDED)
namespace kagami {
fs::path runtime_data_dir() {
    return config_directory;
}
fs::path runtime_config_file() {
    return config_directory / "config.json";
}
bool prepare_private_directory(const fs::path& path, std::string& error) {
    std::error_code code;
    fs::create_directories(path, code);
    error = code.message();
    return !code;
}
namespace logging {
void set_debug_enabled(bool) noexcept {}
void write(Level, const std::string&, const std::string&) noexcept {}
}  // namespace logging
}  // namespace kagami
namespace ksud {
std::optional<std::string> read_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        errno = ENOENT;
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
bool write_file_atomic(const fs::path& path, const std::string& content) {
    std::ofstream output(path);
    output << content;
    return output.good();
}
}  // namespace ksud
#endif

int main() {
    char pattern[] = "/data/local/tmp/kagami-kstat-test-XXXXXX";
    const auto* directory = mkdtemp(pattern);
    assert(directory);
    config_directory = directory;
    std::string error;
    TestConfig config;
    assert(kagami::parse_config_json(valid, config, error));
    check_values(config);
    TestConfig moved = std::move(config);
    check_values(moved);
    assert(merge(valid, error));
    assert(merge(R"({"debug":true})", error));
    TestConfig persisted;
    assert(read(persisted, error));
    check_values(persisted);
    const auto original = saved();

    for (const auto& entry : {R"({"fields":512,"target_ino":"1"})",
                              R"({"fields":1,"match_flags":2,"target_ino":"1"})",
                              R"({"fields":1.5,"target_ino":"1"})",
                              R"({"fields":-1,"target_ino":"1"})",
                              R"({"fields":4294967296,"target_ino":"1"})",
                              R"({"target_ino":"1"})",
                              R"({"fields":1,"target_ino":9007199254740993})",
                              R"({"fields":1,"target_ino":"18446744073709551616"})",
                              R"({"fields":1,"target_ino":"-1"})",
                              R"({"fields":1,"target_ino":"+1"})",
                              R"({"fields":1,"target_ino":" 1"})",
                              R"({"fields":1,"target_ino":"1e3"})",
                              R"({"fields":1,"target_ino":""})",
                              R"({"fields":1,"target_ino":"0"})",
                              R"({"fields":1,"target_ino":"1","target_dev":"4294967296"})",
                              R"({"fields":1,"target_ino":"1","spoofed_dev":"4294967296"})",
                              R"({"fields":1,"target_ino":"1","spoofed_nlink":"4294967296"})",
                              R"({"fields":1,"target_ino":"1","spoofed_blksize":"4294967296"})",
                              R"({"fields":16,"target_ino":"1","spoofed_atime_nsec":"1000000000"})",
                              R"({"fields":32,"target_ino":"1","spoofed_mtime_nsec":"1000000000"})",
                              R"({"fields":64,"target_ino":"1","spoofed_ctime_nsec":"1000000000"})",
                              R"({"fields":1,"target_path":"relative"})",
                              R"({"fields":1,"target_path":"/a\u0000b"})",
                              R"({"fields":1,"target_ino":"1","unknown":"0"})",
                              R"(null)"}) {
        const auto invalid = std::string("{\"kasumi_kstat_rules\":[") + entry + "]}";
        TestConfig rejected;
        rejected.mount_source = "unchanged";
        assert(!kagami::parse_config_json(invalid, rejected, error) && !error.empty());
        assert(rejected.mount_source == "unchanged" && rejected.kasumi_kstat_rules.empty());
        assert(!merge(invalid, error) && saved() == original);
    }
    for (const auto* invalid : {R"({"kasumi_kstat_rules":null})", R"({"kasumi_kstat_rules":{}})"}) {
        TestConfig rejected;
        assert(!kagami::parse_config_json(invalid, rejected, error));
        assert(!merge(invalid, error) && saved() == original);
    }
    TestConfig oversized_path;
    assert(!kagami::parse_config_json("{\"kasumi_kstat_rules\":[{\"fields\":0,\"target_path\":\"/" +
                                          std::string(KSM_MAX_LEN_PATHNAME - 1, 'a') + "\"}]}",
                                      oversized_path, error));
    TestConfig ignored_nanoseconds;
    assert(kagami::parse_config_json(
        R"({"kasumi_kstat_rules":[{"fields":0,"target_ino":"1","spoofed_atime_nsec":"18446744073709551615"}]})",
        ignored_nanoseconds, error));
    std::string excessive = "{\"kasumi_kstat_rules\":[";
    for (std::size_t i = 0; i <= KSM_TX_MAX_OPERATIONS; ++i) {
        if (i)
            excessive += ',';
        excessive += R"({"fields":0,"target_ino":"1"})";
    }
    excessive += "]}";
    TestConfig rejected_count;
    assert(!kagami::parse_config_json(excessive, rejected_count, error));
    assert(!merge(excessive, error) && saved() == original);
    assert(merge(R"({"kasumi_kstat_rules":[]})", error));
    TestConfig cleared;
    assert(read(cleared, error) && cleared.kasumi_kstat_rules.empty());
    TestConfig defaults;
    assert(kagami::parse_config_json("{}", defaults, error) && defaults.kasumi_kstat_rules.empty());
    fs::remove_all(config_directory);
    std::cout << "Kasumi kstat config parsing and persistence tests passed\n";
}
