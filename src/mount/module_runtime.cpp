#include "mount/module_runtime.hpp"

#include "core/json_value.hpp"
#include "core/runtime.hpp"

#include <fcntl.h>
#include <linux/stat.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

namespace kagami::mount::module_runtime {
namespace {
namespace fs = std::filesystem;
struct Record {
    std::string backend;
    bool complete = false;
    std::vector<MountContribution> mounts;
    std::vector<RuleEvidence> rules;
};
struct Database {
    std::string boot;
    std::uint64_t generation = 0;
    std::string namespace_id;
    std::map<std::string, Record> modules;
};
struct Writer {
    unsigned depth = 0;
    int fd = -1;
};
thread_local Writer writer;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0)
            close(fd_);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&&) = delete;
    FileDescriptor& operator=(FileDescriptor&&) = delete;
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};

fs::path directory() {
    return runtime_data_dir() / "run" / "module_runtime";
}
fs::path data_file() {
    return directory() / "state";
}
fs::path pending_file() {
    return directory() / "pending";
}
fs::path invalid_file() {
    return directory() / "invalid";
}
fs::path lock_file() {
    return directory() / "lock";
}

bool safe_text(const std::string& value) {
    return value.find_first_of("\r\n") == std::string::npos &&
           value.find('\0') == std::string::npos;
}
bool backend_valid(const std::string& value) {
    return value == "overlay" || value == "magic" || value == "kasumi" || value == "none";
}
bool rule_valid(const RuleEvidence& value) {
    return (value.kind == "ADD" || value.kind == "MERGE" || value.kind == "HIDE") &&
           !value.path.empty() && value.path.front() == '/' && safe_text(value.path) &&
           safe_text(value.source);
}
std::string ns_identity(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
        return {};
    return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino);
}
bool read_text(const fs::path& path, std::string& text, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    if (!read_complete(in, text)) {
        error = "incomplete read: " + path.string();
        return false;
    }
    return true;
}
void invalidate() {
    std::string ignored;
    (void)write_file_atomic(invalid_file().string(), "observation_write_failed\n", ignored);
}
bool store(const Database& database) {
    std::ostringstream out;
    out << "KAGAMI_MODULE_RUNTIME_V2 " << std::quoted(database.boot) << ' ' << database.generation
        << ' ' << std::quoted(database.namespace_id) << '\n';
    for (const auto& [id, record] : database.modules) {
        if (!safe_text(id) || !backend_valid(record.backend)) {
            invalidate();
            return false;
        }
        out << "module " << std::quoted(id) << ' ' << record.backend << ' ' << record.complete
            << '\n';
        for (const auto& mount : record.mounts) {
            const auto& m = mount.identity;
            out << "mount " << std::quoted(id) << ' ' << std::quoted(mount.path) << ' '
                << mount.committed << ' ' << mount.captured << ' ' << m.mount_id << ' ' << m.device
                << ' ' << m.inode << ' ' << std::quoted(m.root) << ' ' << std::quoted(m.path) << ' '
                << std::quoted(m.filesystem) << ' ' << std::quoted(m.source) << ' '
                << std::quoted(m.namespace_id) << '\n';
        }
        for (const auto& rule : record.rules) {
            if (!rule_valid(rule)) {
                invalidate();
                return false;
            }
            out << "rule " << std::quoted(id) << ' ' << rule.kind << ' ' << std::quoted(rule.path)
                << ' ' << std::quoted(rule.source) << '\n';
        }
    }
    std::string error;
    if (!write_file_atomic(data_file().string(), out.str(), error)) {
        invalidate();
        return false;
    }
    return true;
}
bool load(Database& database, std::string& reason, std::string& error) {
    std::string text;
    if (!read_text(data_file(), text, error)) {
        reason = "record_unreadable";
        std::error_code ec;
        if (!fs::exists(data_file(), ec) && !ec) {
            reason = "no_record";
            error.clear();
        }
        return false;
    }
    std::istringstream input(text);
    std::string line;
    std::string version;
    if (!std::getline(input, line)) {
        reason = "invalid_record";
        error = "empty module runtime record";
        return false;
    }
    std::istringstream header(line);
    if (!(header >> version) || version != "KAGAMI_MODULE_RUNTIME_V2") {
        const bool old_schema = version.rfind("KAGAMI_MODULE_RUNTIME_V", 0) == 0;
        reason = old_schema ? "unsupported_record" : "invalid_record";
        error = old_schema ? "" : "invalid module runtime record header";
        return false;
    }
    if (!(header >> std::quoted(database.boot) >> database.generation >>
          std::quoted(database.namespace_id)) ||
        !(header >> std::ws).eof() || database.generation == 0 || database.namespace_id.empty()) {
        reason = "invalid_record";
        error = "invalid module runtime record header";
        return false;
    }
    const auto boot = runtime_boot_id();
    if (boot.empty() || database.boot != boot) {
        reason = boot.empty() ? "boot_unreadable" : "stale_boot";
        error = boot.empty() ? "cannot read current boot identity" : "";
        return false;
    }
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string type;
        std::string id;
        if (!(row >> type >> std::quoted(id)) || id.empty() || !safe_text(id))
            break;
        if (type == "module") {
            Record record;
            if (!(row >> record.backend >> record.complete) || !backend_valid(record.backend) ||
                database.modules.count(id) != 0)
                break;
            database.modules.emplace(id, std::move(record));
        } else {
            const auto found = database.modules.find(id);
            if (found == database.modules.end())
                break;
            if (type == "mount") {
                MountContribution mount;
                auto& m = mount.identity;
                if (!(row >> std::quoted(mount.path) >> mount.committed >> mount.captured >>
                      m.mount_id >> m.device >> m.inode >> std::quoted(m.root) >>
                      std::quoted(m.path) >> std::quoted(m.filesystem) >> std::quoted(m.source) >>
                      std::quoted(m.namespace_id)) ||
                    mount.path.empty() || mount.path.front() != '/' || !safe_text(mount.path) ||
                    (mount.captured && (!mount.committed || m.mount_id == 0 ||
                                        m.namespace_id.empty() || m.path != mount.path)))
                    break;
                found->second.mounts.push_back(std::move(mount));
            } else if (type == "rule") {
                RuleEvidence rule;
                if (!(row >> rule.kind >> std::quoted(rule.path) >> std::quoted(rule.source)) ||
                    !rule_valid(rule))
                    break;
                found->second.rules.push_back(std::move(rule));
            } else {
                break;
            }
        }
        if (!(row >> std::ws).eof())
            break;
    }
    if (!input.eof() || (!text.empty() && text.back() != '\n')) {
        reason = "invalid_record";
        error = "invalid or incomplete module runtime record";
        return false;
    }
    return true;
}
template <typename Function>
bool update(Function fn) {
    if (writer.depth == 0 || writer.fd < 0) {
        invalidate();
        return false;
    }
    Database database;
    std::string reason;
    std::string error;
    if (!load(database, reason, error)) {
        invalidate();
        return false;
    }
    fn(database);
    return store(database);
}
std::string decode(const std::string& value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 3 < value.size() && value[i + 1] >= '0' &&
            value[i + 1] <= '3' && value[i + 2] >= '0' && value[i + 2] <= '7' &&
            value[i + 3] >= '0' && value[i + 3] <= '7') {
            result += static_cast<char>(((value[i + 1] - '0') * 64) + ((value[i + 2] - '0') * 8) +
                                        (value[i + 3] - '0'));
            i += 3;
        } else {
            result += value[i];
        }
    }
    return result;
}
bool stat_identity(const std::string& path, MountIdentity& identity) {
#if defined(__linux__) && defined(__NR_statx)
    struct statx st{};
    constexpr unsigned mask = STATX_INO | STATX_MNT_ID;
    if (syscall(__NR_statx, AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, mask,
                &st) != 0 ||
        (st.stx_mask & mask) != mask)
        return false;
    identity.mount_id = st.stx_mnt_id;
    identity.device = (static_cast<std::uint64_t>(st.stx_dev_major) << 32) | st.stx_dev_minor;
    identity.inode = st.stx_ino;
    return true;
#else
    (void)path;
    (void)identity;
    return false;
#endif
}
bool resolve_mount_identity(const std::string& path, std::string& resolved,
                            MountIdentity& identity) {
    std::error_code ec;
    resolved = fs::canonical(path, ec).string();
    if (ec || !stat_identity(path, identity))
        return false;
    if (resolved != path) {
        // Resolve intermediate aliases while retaining the no-follow check on
        // the final component. Both paths must identify the same mounted inode.
        MountIdentity canonical;
        if (!stat_identity(resolved, canonical) || canonical.mount_id != identity.mount_id ||
            canonical.device != identity.device || canonical.inode != identity.inode)
            return false;
    }
    return true;
}
bool read_mounts(std::vector<MountIdentity>& mounts, std::string& error, bool self) {
    const std::string proc = self ? "/proc/self" : "/proc/1";
    std::string text;
    if (!read_text(proc + "/mountinfo", text, error) || !parse_mountinfo(text, mounts, error))
        return false;
    const auto ns = ns_identity(proc + "/ns/mnt");
    if (ns.empty()) {
        error = "cannot read mount namespace identity";
        return false;
    }
    for (auto& mount : mounts)
        mount.namespace_id = ns;
    return true;
}
bool hidden_by_ancestor(const MountIdentity& current, const std::vector<MountIdentity>& mounts) {
    std::set<std::uint64_t> ancestors;
    const MountIdentity* node = &current;
    while (ancestors.insert(node->mount_id).second) {
        const auto parent = std::find_if(mounts.begin(), mounts.end(), [&](const auto& entry) {
            return entry.mount_id == node->parent_id;
        });
        if (parent == mounts.end())
            break;
        node = &*parent;
    }
    auto path = fs::path(current.path).parent_path();
    while (!path.empty()) {
        MountIdentity visible;
        const auto probe = runtime_mount_here() ? path.string() : "/proc/1/root" + path.string();
        if (stat_identity(probe, visible)) {
            // A reachable ancestor belongs to another attachment branch: an
            // upper mount is hiding this still-attached lower subtree.
            return ancestors.count(visible.mount_id) == 0 &&
                   std::any_of(mounts.begin(), mounts.end(), [&](const auto& entry) {
                       return entry.mount_id == visible.mount_id;
                   });
        }
        if (path == path.root_path())
            break;
        path = path.parent_path();
    }
    return false;
}
bool rules_from_text(const std::string& text, std::vector<RuleEvidence>& rules) {
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string kind;
        row >> kind;
        if (kind != "add" && kind != "merge" && kind != "hide")
            continue;
        RuleEvidence rule;
        if (kind == "add")
            rule.kind = "ADD";
        else
            rule.kind = kind == "merge" ? "MERGE" : "HIDE";
        if (!(row >> rule.path))
            return false;
        if (kind != "hide" && !(row >> rule.source))
            return false;
        if (kind == "add") {
            unsigned type = 0;
            if (!(row >> type) || type != 0)
                return false;
        }
        if (!(row >> std::ws).eof() || !rule_valid(rule))
            return false;
        rules.push_back(std::move(rule));
    }
    return true;
}
Summary unknown(const std::string& backend, const std::string& reason,
                const std::string& error = {}, const std::string& code = {}) {
    return {"unknown", backend, reason, code, error};
}
Summary summarize(const Record& record, const std::vector<MountIdentity>& live, bool mounts_ok,
                  const std::string& mount_error, const KasumiObservation& kasumi) {
    std::size_t expected = 0;
    std::size_t present = 0;
    if (record.backend == "kasumi" && !record.rules.empty()) {
        if (!kasumi.ok)
            return unknown(record.backend, "read_failed", kasumi.error, kasumi.error_code);
        for (const auto& rule : record.rules) {
            if (rule.path.find_first_of(" \t\r\n") != std::string::npos ||
                rule.source.find_first_of(" \t\r\n") != std::string::npos)
                return unknown(record.backend, "ambiguous_rule_encoding");
            ++expected;
            if (kasumi.enabled &&
                std::any_of(kasumi.rules.begin(), kasumi.rules.end(), [&](const auto& current) {
                    return current.kind == rule.kind && current.path == rule.path &&
                           current.source == rule.source;
                }))
                ++present;
        }
    }
    if (!record.mounts.empty() && !mounts_ok)
        return unknown(record.backend, "read_failed", mount_error, "mountinfo_unreadable");
    for (const auto& contribution : record.mounts) {
        ++expected;
        if (!contribution.committed)
            continue;
        if (!contribution.captured)
            return unknown(record.backend, "capture_failed");
        const auto& stored = contribution.identity;
        if (!live.empty() && live.front().namespace_id != stored.namespace_id)
            return unknown(record.backend, "namespace_changed");
        const auto current = std::find_if(live.begin(), live.end(), [&](const auto& mount) {
            return mount.mount_id == stored.mount_id;
        });
        if (current == live.end())
            continue;
        if (!same_mount(stored, *current))
            return unknown(record.backend, "mount_identity_changed");
        MountIdentity visible;
        const auto path = runtime_mount_here() ? stored.path : "/proc/1/root" + stored.path;
        // An upper overlay may cover this mount. mountinfo still identifies the
        // lower attachment; only compare inode when this exact mount is visible.
        if (!stat_identity(path, visible)) {
            if (hidden_by_ancestor(*current, live)) {
                ++present;
                continue;
            }
            return unknown(record.backend, "read_failed", "cannot inspect " + stored.path,
                           "mount_identity_unreadable");
        }
        if (visible.mount_id == stored.mount_id &&
            (visible.device != stored.device || visible.inode != stored.inode))
            return unknown(record.backend, "mount_identity_changed");
        ++present;
    }
    if (present != 0)
        return {record.complete && present == expected ? "mounted" : "partial",
                record.backend,
                present == expected && record.complete ? "" : "incomplete",
                {},
                {}};
    if (!record.complete)
        return unknown(record.backend, "incomplete");
    return {"unmounted",
            record.backend,
            expected == 0 ? "no_effective_payload" : "not_present",
            {},
            {}};
}
}  // namespace

Operation::Operation() : outer_(writer.depth++ == 0) {
    if (!outer_)
        return;
    std::string error;
    if (!prepare_private_directory(directory(), error))
        return;
    writer.fd = open(lock_file().c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (writer.fd < 0)
        return;
    if (flock(writer.fd, LOCK_EX) != 0) {
        close(writer.fd);
        writer.fd = -1;
        return;
    }
    std::error_code previous_error;
    if (fs::exists(pending_file(), previous_error) || previous_error)
        invalidate();
    if (!write_file_atomic(pending_file().string(), runtime_boot_id() + "\n", error)) {
        invalidate();
        return;
    }
    Database database;
    std::string reason;
    if (!load(database, reason, error)) {
        if (reason != "no_record" && reason != "stale_boot") {
            invalidate();
            return;
        }
        database = {};
        database.boot = runtime_boot_id();
    }
    database.namespace_id =
        ns_identity(runtime_mount_here() ? "/proc/self/ns/mnt" : "/proc/1/ns/mnt");
    if (database.generation == UINT64_MAX) {
        invalidate();
        return;
    }
    ++database.generation;
    // A later successful metadata write cannot repair an earlier missing
    // observation. Only an explicit full boot inventory may clear invalid.
    if (database.boot.empty() || database.namespace_id.empty() || !store(database))
        invalidate();
}
Operation::~Operation() {
    if (writer.depth != 0)
        --writer.depth;
    if (outer_ && writer.fd >= 0) {
        // The boot-completed watcher also forks while this operation exists.
        // It must not keep our flock alive after all mount children are joined.
        (void)flock(writer.fd, LOCK_UN);
        close(writer.fd);
        writer.fd = -1;
    }
}
void Operation::finish(bool success) {
    (void)success;  // Native failure is represented by the retained live evidence.
    if (finished_)
        return;
    finished_ = true;
    if (!outer_ || writer.fd < 0)
        return;
    std::error_code ec;
    if (fs::exists(invalid_file(), ec) || ec)
        return;
    fs::remove(pending_file(), ec);
}

bool assign_modules(const std::vector<Assignment>& assignments, const std::string& scope) {
    const auto assign = [&](Database& database) {
        for (auto& [id, record] : database.modules) {
            (void)id;
            if (scope.empty() || record.backend == scope) {
                record.mounts.clear();
                record.rules.clear();
                record.complete = true;
            }
        }
        for (const auto& assignment : assignments)
            database.modules[assignment.id] = {
                assignment.backend, assignment.backend == "none", {}, {}};
    };
    if (!scope.empty())
        return update(assign);
    // Only the boot orchestrator calls a full assignment, after it has claimed
    // this boot. This explicit inventory can recover a corrupt/old schema;
    // unrelated control operations deliberately cannot erase those errors.
    if (writer.depth == 0 || writer.fd < 0)
        return false;
    Database database;
    std::string reason;
    std::string error;
    if (!load(database, reason, error)) {
        database = {};
        database.boot = runtime_boot_id();
        database.generation = 1;
        database.namespace_id =
            ns_identity(runtime_mount_here() ? "/proc/self/ns/mnt" : "/proc/1/ns/mnt");
    }
    if (database.boot.empty() || database.namespace_id.empty()) {
        invalidate();
        return false;
    }
    assign(database);
    if (!store(database))
        return false;
    std::error_code ec;
    fs::remove(invalid_file(), ec);
    return !ec;
}
MountContribution capture_mount(const std::string& path, const std::vector<std::string>& owners) {
    MountContribution result{path, owners, true, false, {}};
    std::vector<MountIdentity> mounts;
    std::string error;
    std::string resolved;
    MountIdentity visible;
    if (!resolve_mount_identity(path, resolved, visible) || !read_mounts(mounts, error, true))
        return result;
    const auto match = std::find_if(mounts.begin(), mounts.end(), [&](const auto& mount) {
        return mount.mount_id == visible.mount_id && mount.path == resolved &&
               mount.device == visible.device;
    });
    if (match == mounts.end())
        return result;
    result.identity = *match;
    result.identity.inode = visible.inode;
    result.path = resolved;
    result.captured = true;
    return result;
}
void capture_mounts(std::vector<MountContribution>& contributions) {
    bool has_committed = false;
    for (auto& contribution : contributions) {
        if (!contribution.committed)
            continue;
        has_committed = true;
        contribution.captured = false;
        contribution.identity = {};
    }
    if (!has_committed)
        return;
    std::vector<MountIdentity> mounts;
    std::string error;
    if (!read_mounts(mounts, error, true))
        return;
    std::map<std::uint64_t, const MountIdentity*> by_id;
    for (const auto& mount : mounts)
        by_id.emplace(mount.mount_id, &mount);
    for (auto& contribution : contributions) {
        if (!contribution.committed)
            continue;
        std::string resolved;
        MountIdentity visible;
        if (!resolve_mount_identity(contribution.path, resolved, visible))
            continue;
        const auto match = by_id.find(visible.mount_id);
        if (match == by_id.end() || match->second->path != resolved ||
            match->second->device != visible.device)
            continue;
        contribution.identity = *match->second;
        contribution.identity.inode = visible.inode;
        contribution.path = resolved;
        contribution.captured = true;
    }
}
bool record_mounts(const std::string& backend, const std::vector<MountContribution>& contributions,
                   bool complete) {
    return update([&](Database& database) {
        for (auto& [id, record] : database.modules) {
            if (record.backend != backend)
                continue;
            record.complete = complete;
            record.mounts.clear();
            for (const auto& contribution : contributions) {
                if (std::find(contribution.owners.begin(), contribution.owners.end(), id) !=
                    contribution.owners.end())
                    record.mounts.push_back(contribution);
            }
        }
    });
}
bool record_rules(const std::vector<RuleEvidence>& rules, bool complete) {
    return update([&](Database& database) {
        for (auto& [id, record] : database.modules) {
            if (record.backend != "kasumi")
                continue;
            record.complete = complete;
            record.rules.clear();
            for (const auto& rule : rules) {
                if (std::find(rule.owners.begin(), rule.owners.end(), id) != rule.owners.end())
                    record.rules.push_back(rule);
            }
        }
    });
}

Snapshot read_snapshot(const std::function<KasumiObservation()>& observe) {
    Snapshot result;
    Database database;
    std::string reason;
    std::string error;
    const bool loaded = load(database, reason, error);
    const FileDescriptor fd(open(lock_file().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    const bool locked = fd.get() >= 0 && flock(fd.get(), LOCK_SH | LOCK_NB) == 0;
    const int lock_error = locked ? 0 : errno;
    std::error_code pending_error;
    std::error_code invalid_error;
    const bool pending = fs::exists(pending_file(), pending_error);
    const bool invalid = fs::exists(invalid_file(), invalid_error);
    if (!loaded || !locked || pending || invalid || pending_error || invalid_error) {
        if (loaded) {
            if (!locked && lock_error != EWOULDBLOCK && lock_error != EAGAIN) {
                reason = "record_unreadable";
                error = "cannot lock runtime snapshot: " + std::string(std::strerror(lock_error));
            } else if (pending_error || invalid_error) {
                reason = "record_unreadable";
                error = "cannot inspect runtime publication state";
            } else {
                if (!locked)
                    reason = "updating";
                else
                    reason = invalid ? "record_write_failed" : "interrupted";
                error = invalid ? "failed to publish runtime observation" : "";
            }
        }
        result.missing = unknown({}, reason, error, error.empty() ? "" : reason);
        for (const auto& [id, record] : database.modules) {
            auto value = result.missing;
            value.backend = record.backend;
            result.modules.emplace(id, std::move(value));
        }
        return result;
    }
    // Re-read after acquiring the lock: a writer may have published between
    // the first read (needed for pending IDs) and this shared-lock acquisition.
    database = {};
    if (!load(database, reason, error)) {
        result.missing = unknown({}, reason, error, reason);
        return result;
    }
    const auto live_namespace =
        ns_identity(runtime_mount_here() ? "/proc/self/ns/mnt" : "/proc/1/ns/mnt");
    if (live_namespace.empty() || live_namespace != database.namespace_id) {
        result.missing = unknown({}, live_namespace.empty() ? "read_failed" : "namespace_changed",
                                 live_namespace.empty() ? "cannot inspect runtime namespace" : "",
                                 live_namespace.empty() ? "namespace_unreadable" : "");
        for (const auto& [id, record] : database.modules) {
            auto value = result.missing;
            value.backend = record.backend;
            result.modules.emplace(id, std::move(value));
        }
        return result;
    }
    std::vector<MountIdentity> mounts;
    const bool mounts_ok = read_mounts(mounts, error, runtime_mount_here());
    KasumiObservation kasumi;
    const bool needs_kasumi =
        std::any_of(database.modules.begin(), database.modules.end(),
                    [](const auto& entry) { return !entry.second.rules.empty(); });
    if (needs_kasumi)
        kasumi = observe();
    if (kasumi.ok && !kasumi.text.empty() && !rules_from_text(kasumi.text, kasumi.rules)) {
        kasumi.ok = false;
        kasumi.error_code = "ambiguous_rule_encoding";
        kasumi.error = "cannot parse complete Kasumi rule snapshot";
    }
    for (const auto& [id, record] : database.modules)
        result.modules.emplace(id, summarize(record, mounts, mounts_ok, error, kasumi));
    return result;
}

Snapshot read_snapshot(const KasumiObservation& observation) {
    return read_snapshot(std::function<KasumiObservation()>([&]() { return observation; }));
}

std::string summary_json(const Summary& summary) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    const auto string = [](const std::string& value) {
        JsonValue out;
        out.type = JsonValue::Type::String;
        out.string_value = value;
        return out;
    };
    root.object_value["state"] = string(summary.state);
    root.object_value["backend"] = summary.backend.empty() ? JsonValue{} : string(summary.backend);
    root.object_value["reason"] = summary.reason.empty() ? JsonValue{} : string(summary.reason);
    JsonValue error;
    if (!summary.error.empty() || !summary.error_code.empty()) {
        error.type = JsonValue::Type::Object;
        error.object_value["code"] = string(summary.error_code);
        error.object_value["message"] = string(summary.error);
    }
    root.object_value["error"] = std::move(error);
    return stringify_json(root);
}

bool parse_mountinfo(const std::string& text, std::vector<MountIdentity>& mounts,
                     std::string& error) {
    mounts.clear();
    std::istringstream input(text);
    std::string line;
    std::set<std::uint64_t> ids;
    while (std::getline(input, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos)
            break;
        std::istringstream before(line.substr(0, sep));
        std::istringstream after(line.substr(sep + 3));
        MountIdentity mount;
        std::uint64_t major = 0;
        std::uint64_t minor = 0;
        std::string device;
        std::string options;
        std::string super_options;
        if (!(before >> mount.mount_id >> mount.parent_id >> device >> mount.root >> mount.path >>
              options) ||
            !(after >> mount.filesystem >> mount.source >> super_options) ||
            !(after >> std::ws).eof() || mount.mount_id == 0 || !ids.insert(mount.mount_id).second)
            break;
        std::istringstream dev(device);
        char colon = 0;
        if (!(dev >> major >> colon >> minor) || colon != ':' || !(dev >> std::ws).eof() ||
            major > UINT32_MAX || minor > UINT32_MAX)
            break;
        mount.device = (major << 32) | minor;
        mount.path = decode(mount.path);
        mount.root = decode(mount.root);
        mount.source = decode(mount.source);
        if (mount.path.empty() || mount.path.front() != '/' || mount.root.empty() ||
            mount.root.front() != '/')
            break;
        mounts.push_back(std::move(mount));
    }
    if (!input.eof() || mounts.empty() || text.empty() || text.back() != '\n') {
        error = "invalid or incomplete mountinfo snapshot";
        mounts.clear();
        return false;
    }
    return true;
}
bool same_mount(const MountIdentity& recorded, const MountIdentity& live) {
    return recorded.mount_id == live.mount_id && recorded.device == live.device &&
           recorded.root == live.root && recorded.path == live.path &&
           recorded.filesystem == live.filesystem && recorded.source == live.source &&
           recorded.namespace_id == live.namespace_id;
}

bool read_complete(std::istream& input, std::string& output) {
    output.clear();
    std::array<char, 4096> buffer{};
    try {
        while (input.read(buffer.data(), buffer.size()))
            output.append(buffer.data(), buffer.size());
        output.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
    } catch (const std::ios_base::failure&) {
        return false;
    }
    return input.eof() && !input.bad();
}
}  // namespace kagami::mount::module_runtime
