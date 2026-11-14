#include "kagami/kasumi_client.hpp"
#if defined(KAGAMI_TEST_EMBEDDED)
#include "uapi/kasumi.h"
#else
#include "kagami/kasumi_uapi.h"
#endif

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using kagami::kasumi::PathOperation;
using kagami::kasumi::PathRule;

struct Transaction {
    kasumi_transaction status{};
    std::vector<PathRule> rules;
    kasumi_transaction_configuration configuration{};
    std::vector<std::uint32_t> allow_uids, deny_uids;
    std::vector<kasumi_transaction_maps_rule> maps;
    std::vector<kasumi_transaction_kstat_rule> kstat;
};
struct Kernel {
    int protocol = 19;
    std::uint32_t max_operations = 8;
    std::uint64_t max_bytes = 65536;
    std::uint64_t capabilities = KSM_CAP_ATOMIC_PATH_RULES | KSM_CAP_ATOMIC_SETTINGS |
                                 KSM_CAP_ATOMIC_POLICY | KSM_CAP_ATOMIC_MAPS | KSM_CAP_ATOMIC_KSTAT;
    std::uint64_t operations = (1ULL << KSM_TX_REDIRECT) | (1ULL << KSM_TX_MERGE) |
                               (1ULL << KSM_TX_HIDE) | (1ULL << KSM_TX_DELETE) |
                               (1ULL << KSM_TX_HIDE_XATTRS);
    int next_fd = 1000;
    int opened = 0;
    int closed = 0;
    int operation_count = 0;
    int begin_count = 0;
    int stage_count = 0;
    int configuration_count = 0;
    int configuration_error = 0;
    int commit_count = 0;
    int abort_count = 0;
    int query_count = 0;
    int capability_error = 0;
    bool begin_copyout_failure = false;
    bool commit_copyout_failure = false;
    bool commit_conflict = false;
    bool begin_conflict_once = false;
    bool commit_conflict_once = false;
    int stage_failure = 0;
    Transaction published_configuration;
    std::map<int, Transaction> transactions;
    std::vector<PathRule> published{{PathOperation::Hide, "/old", "", 0}};
} kernel;

int open_control() {
    ++kernel.opened;
    return kernel.next_fd++;
}

int request(int fd, unsigned long command, void* argument) {
    assert(fd >= 1000 || fd == 99);
    if (command == KSM_IOC_GET_VERSION) {
        *static_cast<int*>(argument) = kernel.protocol;
        return 0;
    }
    ++kernel.operation_count;
    if (command == KSM_IOC_SET_ENABLED)
        return 0;
    if (command == KSM_IOC_GET_CAPABILITIES) {
        assert(kernel.protocol == 19);
        if (kernel.capability_error) {
            errno = kernel.capability_error;
            return -1;
        }
        auto& result = *static_cast<kasumi_capabilities*>(argument);
        assert(result.size == sizeof(result));
        result = {};
        result.size = sizeof(result);
        result.version = KSM_CAPABILITIES_VERSION;
        result.capabilities = kernel.capabilities;
        result.generation = 7;
        result.operations = kernel.operations;
        result.max_stage = 2;
        result.max_operations = kernel.max_operations;
        result.max_bytes = kernel.max_bytes;
        return 0;
    }
    if (command == KSM_IOC_BEGIN_TRANSACTION) {
        ++kernel.begin_count;
        auto& input = *static_cast<kasumi_transaction*>(argument);
        assert(input.size == sizeof(input));
        assert(!(input.flags & ~(KSM_TX_REPLACE_PATH_RULES | KSM_TX_REPLACE_XATTR_RULES)));
        assert(input.expected_generation == 7);
        assert(kernel.transactions.count(fd) == 0);
        if (kernel.begin_conflict_once && kernel.begin_count == 1) {
            errno = EAGAIN;
            return -1;
        }
        auto& transaction = kernel.transactions[fd];
        transaction.status = input;
        transaction.status.id = 83;
        transaction.status.state = KSM_TX_STATE_STAGING;
        if (kernel.begin_copyout_failure) {
            input.id = 0xdead;
            errno = EFAULT;
            return -1;
        }
        input = transaction.status;
        return 0;
    }
    auto found = kernel.transactions.find(fd);
    if (found == kernel.transactions.end() && command == KSM_IOC_QUERY_TRANSACTION) {
        ++kernel.query_count;
        errno = ENOENT;
        return -1;
    }
    assert(found != kernel.transactions.end());
    auto& transaction = found->second;
    if (command == KSM_IOC_STAGE_CONFIGURATION) {
        ++kernel.configuration_count;
        const auto& input = *static_cast<kasumi_transaction_configuration*>(argument);
        assert(input.size == sizeof(input) && input.id == transaction.status.id);
        assert(!input.reserved1 && !input.reserved2 && !input.reserved[0] && !input.reserved[1]);
        if (kernel.configuration_error) {
            errno = kernel.configuration_error;
            return -1;
        }
        transaction.configuration = input;
        if (input.allow_count) {
            const auto* uids = reinterpret_cast<const std::uint32_t*>(input.allow_uids);
            transaction.allow_uids.assign(uids, uids + input.allow_count);
        }
        if (input.deny_count) {
            const auto* uids = reinterpret_cast<const std::uint32_t*>(input.deny_uids);
            transaction.deny_uids.assign(uids, uids + input.deny_count);
        }
        if (input.maps_count) {
            const auto* maps =
                reinterpret_cast<const kasumi_transaction_maps_rule*>(input.maps_rules);
            transaction.maps.assign(maps, maps + input.maps_count);
        }
        if (input.kstat_count) {
            const auto* kstat =
                reinterpret_cast<const kasumi_transaction_kstat_rule*>(input.kstat_rules);
            transaction.kstat.assign(kstat, kstat + input.kstat_count);
        } else {
            assert(!input.kstat_rules);
        }
        return 0;
    }
    if (command == KSM_IOC_STAGE_TRANSACTION) {
        ++kernel.stage_count;
        auto& stage = *static_cast<kasumi_transaction_stage*>(argument);
        assert(stage.size == sizeof(stage) && stage.id == transaction.status.id);
        assert(stage.count && stage.count <= 2);
        assert(transaction.status.state == KSM_TX_STATE_STAGING);
        if (kernel.stage_count == kernel.stage_failure) {
            errno = ENOMEM;
            return -1;
        }
        const auto* rules = reinterpret_cast<const kasumi_transaction_rule*>(stage.rules);
        for (__u32 i = 0; i < stage.count; ++i) {
            transaction.rules.push_back(
                {static_cast<PathOperation>(rules[i].operation),
                 reinterpret_cast<const char*>(rules[i].visible_path),
                 rules[i].source_path ? reinterpret_cast<const char*>(rules[i].source_path) : "",
                 rules[i].flags});
        }
        return 0;
    }
    auto& status = *static_cast<kasumi_transaction*>(argument);
    assert(status.size == sizeof(status));
    assert(status.id == transaction.status.id ||
           (!status.id && command == KSM_IOC_QUERY_TRANSACTION));
    if (command == KSM_IOC_COMMIT_TRANSACTION) {
        ++kernel.commit_count;
        if (kernel.commit_conflict || (kernel.commit_conflict_once && kernel.commit_count == 1)) {
            transaction.status.state = KSM_TX_STATE_FAILED;
            transaction.status.error = -EAGAIN;
            status = transaction.status;
            errno = EAGAIN;
            return -1;
        }
        if (transaction.status.flags & KSM_TX_REPLACE_PATH_RULES)
            kernel.published = transaction.rules;
        else
            kernel.published.insert(kernel.published.end(), transaction.rules.begin(),
                                    transaction.rules.end());
        if (transaction.configuration.fields)
            kernel.published_configuration = transaction;
        transaction.status.state = KSM_TX_STATE_COMMITTED;
        transaction.status.generation = 8;
        if (kernel.commit_copyout_failure) {
            status.id = 0xbeef;
            errno = EFAULT;
            return -1;
        }
    } else if (command == KSM_IOC_QUERY_TRANSACTION) {
        ++kernel.query_count;
    } else if (command == KSM_IOC_ABORT_TRANSACTION) {
        ++kernel.abort_count;
        assert(transaction.status.state != KSM_TX_STATE_COMMITTED);
        transaction.status.state = KSM_TX_STATE_ABORTED;
    } else {
        assert(false);
    }
    status = transaction.status;
    return 0;
}

void reset() {
#if !defined(KAGAMI_TEST_EMBEDDED)
    kagami::kasumi::release_connection();
#endif
    kernel = {};
}

void balanced() {
    assert(kernel.opened == kernel.closed);
    assert(kernel.transactions.empty());
}
}  // namespace

extern "C" long __wrap_syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    (void)va_arg(args, int);
    (void)va_arg(args, int);
    (void)va_arg(args, int);
    *va_arg(args, int*) = open_control();
    va_end(args);
    return 0;
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...) {
    va_list args;
    va_start(args, command);
    void* argument = va_arg(args, void*);
    va_end(args);
    return request(fd, command, argument);
}

extern "C" int __wrap_close(int fd) {
    assert(fd >= 1000);
    ++kernel.closed;
    kernel.transactions.erase(fd);
    return 0;
}

#if defined(KAGAMI_TEST_EMBEDDED)
extern "C" int ksu_kasumi_ioctl(unsigned long command, void* argument) {
    return request(99, command, argument);
}
extern "C" int ksu_kasumi_open_fd() {
    return open_control();
}
#endif

int main() {
    using namespace kagami::kasumi;
    const auto names = feature_names(KSM_FEATURE_SAFE_RULE_RETIRE | KSM_FEATURE_MANAGED_HIDE |
                                     KSM_FEATURE_OVERLAY_XATTR_HIDE);
    assert((names ==
            std::vector<std::string>{"safe_rule_retire", "managed_hide", "overlay_xattr_hide"}));
    const std::vector<PathRule> rules = {
        {PathOperation::Redirect, "/visible", "/source", KSM_RULE_BYPASS_POLICY},
        {PathOperation::Merge, "/directory", "/module", 0},
        {PathOperation::Hide, "/hidden", "", 0},
        {PathOperation::HideXattrs, "/overlay", "", 0},
        {PathOperation::Delete, "/retired", "", 0},
    };
    for (int version : {17, 18, 19, 20}) {
        reset();
        kernel.protocol = version;
        const auto capabilities = control_capabilities();
        assert(capabilities.ok == (version == KSM_PROTOCOL_VERSION));
        assert(version_info().status == (version < KSM_PROTOCOL_VERSION   ? Status::KernelTooOld
                                         : version > KSM_PROTOCOL_VERSION ? Status::ClientTooOld
                                                                          : Status::Available));
        if (version != KSM_PROTOCOL_VERSION) {
            assert(capabilities.last_errno == EPROTO);
            assert(!set_enabled(true) && errno == EPROTO);
            assert(!replace_path_rules(rules) && errno == EPROTO);
            assert(kernel.operation_count == 0 && kernel.begin_count == 0);
        } else {
            assert(set_enabled(true));
        }
#if !defined(KAGAMI_TEST_EMBEDDED)
        release_connection();
#endif
        balanced();
    }
    for (bool begin_fault : {false, true}) {
        for (bool commit_fault : {false, true}) {
            reset();
            kernel.begin_copyout_failure = begin_fault;
            kernel.commit_copyout_failure = commit_fault;
            assert(replace_path_rules(rules));
            assert(kernel.begin_count == 1 && kernel.stage_count == 3 && kernel.commit_count == 1);
            assert(kernel.abort_count == 0);
            assert(kernel.query_count == int(begin_fault) + int(commit_fault));
            assert(kernel.published.size() == rules.size());
            for (std::size_t i = 0; i < rules.size(); ++i) {
                assert(kernel.published[i].operation == rules[i].operation);
                assert(kernel.published[i].path == rules[i].path);
                assert(kernel.published[i].source == rules[i].source);
                assert(kernel.published[i].flags == rules[i].flags);
            }
            balanced();
        }
    }
    reset();
    kernel.stage_failure = 2;
    assert(!replace_path_rules(rules) && errno == ENOMEM);
    assert(kernel.commit_count == 0 && kernel.abort_count == 1);
    assert(kernel.published.size() == 1 && kernel.published[0].path == "/old");
    balanced();

    reset();
    kernel.commit_conflict = true;
    assert(!replace_path_rules(rules) && errno == EAGAIN);
    assert(kernel.begin_count == 3 && kernel.commit_count == 3);
    assert(kernel.abort_count == 3 && kernel.published[0].path == "/old");
    balanced();

    for (bool conflict_at_begin : {false, true}) {
        reset();
        kernel.begin_conflict_once = conflict_at_begin;
        kernel.commit_conflict_once = !conflict_at_begin;
        TransactionConfiguration retry_configuration;
        retry_configuration.fields = KSM_TX_CONFIG_SETTINGS;
        retry_configuration.enabled = true;
        assert(commit_transaction(rules, retry_configuration, KSM_TX_REPLACE_PATH_RULES));
        assert(kernel.begin_count == 2 && kernel.opened == 2);
        assert(kernel.configuration_count == (conflict_at_begin ? 1 : 2));
        assert(kernel.stage_count == (conflict_at_begin ? 3 : 6));
        assert(kernel.commit_count == (conflict_at_begin ? 1 : 2));
        assert(kernel.published.size() == rules.size());
        balanced();
    }

    reset();
    assert(replace_path_rules({}));
    assert(kernel.stage_count == 0 && kernel.commit_count == 1 && kernel.published.empty());
    balanced();

    reset();
    kernel.protocol = 18;
    assert(!replace_path_rules(rules) && errno == EPROTO && kernel.begin_count == 0);
    balanced();

    reset();
    kernel.capability_error = EIO;
    assert(!replace_path_rules(rules) && errno == EIO && kernel.begin_count == 0);
    balanced();

    reset();
    kernel.operations &= ~(1ULL << KSM_TX_MERGE);
    assert(!replace_path_rules(rules) && errno == EOPNOTSUPP && kernel.begin_count == 0);
    balanced();

    reset();
    auto excessive = rules;
    excessive.insert(excessive.end(), rules.begin(), rules.end());
    assert(!replace_path_rules(excessive) && errno == E2BIG && kernel.begin_count == 0);
    balanced();

    reset();
    assert(!replace_path_rules({{PathOperation::Hide, std::string("/a\0/b", 5), "", 0}}));
    assert(errno == EINVAL && kernel.begin_count == 0);
    balanced();
    TransactionConfiguration configuration;
    configuration.fields =
        KSM_TX_CONFIG_SETTINGS | KSM_TX_CONFIG_POLICY | KSM_TX_CONFIG_MAPS | KSM_TX_CONFIG_KSTAT;
    configuration.enabled = true;
    configuration.debug = true;
    configuration.stealth = true;
    configuration.features = KSM_FEATURE_MAPS_SPOOF;
    configuration.policy_owner = 4;
    configuration.policy_flags = 3;
    configuration.allow_uids = {10001, 10002};
    configuration.deny_uids = {10003};
    configuration.maps = {{1, 2, 3, 4, "/visible-mapping"}};
    reset();
    kernel.commit_copyout_failure = true;
    assert(commit_transaction(rules, configuration, KSM_TX_REPLACE_PATH_RULES));
    assert(kernel.configuration_count == 1 && kernel.stage_count == 3);
    const auto& published = kernel.published_configuration;
    assert(published.configuration.fields == configuration.fields);
    assert(published.configuration.enabled && published.configuration.debug &&
           published.configuration.stealth);
    assert(published.configuration.features == KSM_FEATURE_MAPS_SPOOF);
    assert(published.configuration.policy_owner == 4 && published.configuration.policy_flags == 3);
    assert(published.allow_uids == configuration.allow_uids &&
           published.deny_uids == configuration.deny_uids);
    assert(!published.configuration.kstat_count && !published.configuration.kstat_rules);
    assert(published.maps.size() == 1 && published.maps[0].fields == KSM_MAPS_FIELDS_ALL &&
           !published.maps[0].match_flags && published.maps[0].target_ino == 1 &&
           published.maps[0].target_dev == 2 && published.maps[0].spoofed_ino == 3 &&
           published.maps[0].spoofed_dev == 4 &&
           std::string(published.maps[0].spoofed_pathname) == "/visible-mapping");
    balanced();

    reset();
    kernel.configuration_error = ENOMEM;
    assert(!commit_transaction(rules, configuration, KSM_TX_REPLACE_PATH_RULES));
    assert(errno == ENOMEM && !kernel.stage_count && !kernel.commit_count &&
           kernel.abort_count == 1);
    assert(kernel.published[0].path == "/old" &&
           !kernel.published_configuration.configuration.fields);
    balanced();

    reset();
    kernel.stage_failure = 2;
    assert(!commit_transaction(rules, configuration, KSM_TX_REPLACE_PATH_RULES));
    assert(errno == ENOMEM && !kernel.commit_count && kernel.abort_count == 1);
    assert(kernel.published[0].path == "/old" &&
           !kernel.published_configuration.configuration.fields);
    balanced();

    for (const auto capability : {KSM_CAP_ATOMIC_SETTINGS, KSM_CAP_ATOMIC_POLICY,
                                  KSM_CAP_ATOMIC_MAPS, KSM_CAP_ATOMIC_KSTAT}) {
        reset();
        kernel.capabilities &= ~capability;
        assert(!commit_transaction({}, configuration, 0) && errno == EOPNOTSUPP);
        assert(!kernel.begin_count);
        balanced();
    }
    reset();
    configuration.maps[0].spoofed_path.resize(KSM_MAX_LEN_PATHNAME, 'a');
    assert(!commit_transaction({}, configuration, 0) && errno == EINVAL && !kernel.begin_count);
    balanced();

    reset();
    configuration = {};
    configuration.fields = KSM_TX_CONFIG_MAPS;
    assert(commit_transaction({}, configuration, KSM_TX_REPLACE_XATTR_RULES));
    assert(kernel.published[0].path == "/old" && kernel.published_configuration.maps.empty());
    assert(kernel.published_configuration.status.flags == KSM_TX_REPLACE_XATTR_RULES);
    assert(kernel.configuration_count == 1 && !kernel.stage_count);
    balanced();

    reset();
    configuration = {};
    configuration.fields = KSM_TX_CONFIG_SETTINGS;
    assert(commit_transaction({}, configuration, 0));
    assert(!kernel.published_configuration.configuration.enabled &&
           kernel.published[0].path == "/old");
    balanced();
    reset();
    configuration = {};
    configuration.fields = KSM_TX_CONFIG_KSTAT;
    TransactionKstatRule kstat;
    kstat.fields = KSM_KSTAT_FIELDS_ALL;
    kstat.match_flags = KSM_MATCH_ANY_DEVICE;
    kstat.target_ino = UINT64_C(9007199254740993);
    kstat.target_dev = UINT32_MAX;
    kstat.spoofed_ino = UINT64_MAX;
    kstat.spoofed_dev = UINT32_MAX;
    kstat.spoofed_nlink = 0;
    kstat.spoofed_size = UINT64_C(9007199254740995);
    kstat.spoofed_atime_sec = UINT64_MAX;
    kstat.spoofed_atime_nsec = 999999999;
    kstat.spoofed_mtime_sec = UINT64_C(9007199254740997);
    kstat.spoofed_mtime_nsec = 0;
    kstat.spoofed_ctime_sec = 0;
    kstat.spoofed_ctime_nsec = 1;
    kstat.spoofed_blksize = UINT32_MAX;
    kstat.spoofed_blocks = UINT64_MAX;
    kstat.target_path = "/exact-kstat";
    configuration.kstat = {kstat};
    kernel.commit_conflict_once = true;
    kernel.commit_copyout_failure = true;
    assert(commit_transaction({}, configuration, 0));
    assert(kernel.begin_count == 2 && kernel.commit_count == 2 && kernel.abort_count == 1);
    const auto wire = kernel.published_configuration.kstat.at(0);
    assert(kernel.published_configuration.configuration.kstat_count == 1);
    assert(wire.fields == kstat.fields);
    assert(wire.match_flags == kstat.match_flags);
    assert(wire.target_ino == kstat.target_ino);
    assert(wire.target_dev == kstat.target_dev);
    assert(wire.spoofed_ino == kstat.spoofed_ino);
    assert(wire.spoofed_dev == kstat.spoofed_dev);
    assert(wire.spoofed_nlink == kstat.spoofed_nlink);
    assert(wire.spoofed_size == kstat.spoofed_size);
    assert(wire.spoofed_atime_sec == kstat.spoofed_atime_sec);
    assert(wire.spoofed_atime_nsec == kstat.spoofed_atime_nsec);
    assert(wire.spoofed_mtime_sec == kstat.spoofed_mtime_sec);
    assert(wire.spoofed_mtime_nsec == kstat.spoofed_mtime_nsec);
    assert(wire.spoofed_ctime_sec == kstat.spoofed_ctime_sec);
    assert(wire.spoofed_ctime_nsec == kstat.spoofed_ctime_nsec);
    assert(wire.spoofed_blksize == kstat.spoofed_blksize);
    assert(wire.spoofed_blocks == kstat.spoofed_blocks);
    assert(std::string(wire.target_pathname) == kstat.target_path);
    balanced();
    kernel.commit_copyout_failure = false;
    kernel.configuration_error = ENOMEM;
    configuration.kstat.clear();
    assert(!commit_transaction({}, configuration, 0) && errno == ENOMEM);
    assert(kernel.published_configuration.kstat.size() == 1);
    balanced();
    kernel.configuration_error = 0;
    assert(commit_transaction({}, configuration, 0));
    assert(kernel.published_configuration.kstat.empty());
    assert(!kernel.published_configuration.configuration.kstat_rules);
    balanced();

    const auto reject_kstat = [&](const TransactionKstatRule& invalid) {
        reset();
        configuration.fields = KSM_TX_CONFIG_KSTAT;
        configuration.kstat = {invalid};
        assert(!commit_transaction({}, configuration, 0) && errno == EINVAL && !kernel.begin_count);
        balanced();
    };
    auto invalid = kstat;
    invalid.fields = KSM_KSTAT_FIELDS_ALL + 1;
    reject_kstat(invalid);
    invalid = kstat;
    invalid.match_flags = 2;
    reject_kstat(invalid);
    invalid = kstat;
    invalid.target_path = "relative";
    reject_kstat(invalid);
    invalid.target_path = std::string("/a\0b", 4);
    reject_kstat(invalid);
    invalid.target_path = std::string(KSM_MAX_LEN_PATHNAME, '/');
    reject_kstat(invalid);
    invalid.target_path.clear();
    invalid.target_ino = 0;
    reject_kstat(invalid);
    for (auto member :
         {&TransactionKstatRule::target_dev, &TransactionKstatRule::spoofed_dev,
          &TransactionKstatRule::spoofed_nlink, &TransactionKstatRule::spoofed_blksize}) {
        invalid = kstat;
        invalid.*member = UINT64_C(4294967296);
        reject_kstat(invalid);
    }
    for (auto member :
         {&TransactionKstatRule::spoofed_atime_nsec, &TransactionKstatRule::spoofed_mtime_nsec,
          &TransactionKstatRule::spoofed_ctime_nsec}) {
        invalid = kstat;
        invalid.*member = 1000000000;
        reject_kstat(invalid);
    }
    reset();
    configuration.kstat = {kstat};
    configuration.fields = KSM_TX_CONFIG_SETTINGS;
    assert(!commit_transaction({}, configuration, 0) && errno == EINVAL && !kernel.begin_count);
    balanced();
    reset();
    configuration.fields = KSM_TX_CONFIG_KSTAT;
    kernel.protocol = 18;
    assert(!commit_transaction({}, configuration, 0) && errno == EPROTO && !kernel.begin_count);
    balanced();
    reset();
    configuration.kstat.resize(9, kstat);
    assert(!commit_transaction({}, configuration, 0) && errno == E2BIG && !kernel.begin_count);
    balanced();
    reset();
    configuration.kstat = {kstat};
    kernel.max_bytes = sizeof(kasumi_transaction_kstat_rule) - 1;
    assert(!commit_transaction({}, configuration, 0) && errno == E2BIG && !kernel.begin_count);
    balanced();
    reset();
    kernel.max_bytes = sizeof(kasumi_transaction_kstat_rule);
    assert(commit_transaction({}, configuration, 0));
    balanced();
    reset();
    configuration.kstat[0].fields = 0;
    configuration.kstat[0].target_path.clear();
    configuration.kstat[0].target_dev = 0;
    configuration.kstat[0].spoofed_atime_nsec = UINT64_MAX;
    assert(commit_transaction({}, configuration, 0));
    assert(kernel.published_configuration.kstat[0].fields == 0 &&
           kernel.published_configuration.kstat[0].target_dev == 0 &&
           kernel.published_configuration.kstat[0].match_flags == KSM_MATCH_ANY_DEVICE);
    balanced();
#if !defined(KAGAMI_TEST_EMBEDDED)
    reset();
    std::uint32_t allow[] = {10001};
    assert(replace_policy(PolicyOwner::Manual, KSM_POLICY_FLAG_USE_ALLOW_UIDS, allow, {}));
    assert(kernel.configuration_count == 1 && !kernel.stage_count && kernel.commit_count == 1);
    assert(kernel.published_configuration.configuration.fields == KSM_TX_CONFIG_POLICY &&
           kernel.published[0].path == "/old");
    balanced();
    reset();
    assert(reset_policy());
    assert(kernel.configuration_count == 1 && !kernel.stage_count && kernel.commit_count == 1);
    assert(kernel.published_configuration.configuration.fields == KSM_TX_CONFIG_RESET_POLICY &&
           kernel.published[0].path == "/old");
    balanced();
#endif
    std::cout << "Kasumi capability and fd-owned transaction tests passed\n";
}
