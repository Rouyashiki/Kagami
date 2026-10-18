#include "mount/module_runtime.hpp"

#include "core/json_value.hpp"
#include "core/output.hpp"
#include "core/runtime.hpp"
#include "kagami/fixed_array.hpp"

#include <fcntl.h>
#include <linux/stat.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace kagami::mount::module_runtime {
namespace {
namespace fs = std::filesystem;
using detail::Database;
using detail::Record;

// These indexes borrow immutable snapshot storage and never outlive it. Keep
// duplicate IDs in input order so lookup has the same first-match semantics as
// find_if (the strict mountinfo parser normally rejects duplicate IDs).
class MountIndex {
public:
    [[nodiscard]] bool prepare_index(const std::vector<MountIdentity>& mounts) noexcept {
        constexpr std::size_t maximum = 1024UL * 1024;
        if (mounts.size() > maximum || !entries_.prepare_array(mounts.size(), maximum)) {
            errno = mounts.size() > maximum ? EOVERFLOW : ENOMEM;
            return false;
        }
        for (const auto& mount : mounts)
            if (!entries_.append({mount.mount_id, &mount})) {
                errno = ENOBUFS;
                return false;
            }
        if (!mounts.empty())
            std::sort(entries_.data(), entries_.data() + entries_.size(),
                      [](const auto& lhs, const auto& rhs) noexcept {
                          return lhs.id < rhs.id || (lhs.id == rhs.id && lhs.mount < rhs.mount);
                      });
        return true;
    }

    [[nodiscard]] const MountIdentity* find(std::uint64_t id) const noexcept {
        const auto entries = entries_.view();
        if (entries.empty())
            return nullptr;
        const auto* found = std::lower_bound(
            entries.begin(), entries.end(), id,
            [](const auto& entry, auto value) noexcept { return entry.id < value; });
        return found != entries.end() && found->id == id ? found->mount : nullptr;
    }

    [[nodiscard]] bool has_ancestor(const MountIdentity& current, std::uint64_t id) const noexcept {
        const auto* node = &current;
        // Every parent comes from this index. A longer chain must contain a
        // cycle, and by then every reachable mount ID has been inspected.
        for (auto remaining = entries_.size(); node && remaining != 0; --remaining) {
            if (node->mount_id == id)
                return true;
            const auto* parent = find(node->parent_id);
            if (parent == node)
                break;
            node = parent;
        }
        return false;
    }

private:
    struct Entry {
        std::uint64_t id;
        const MountIdentity* mount;
    };
    bounded::Array<Entry> entries_;
};

// An immutable ordinal table is prepared before inspecting any record. This
// removes the former eighth-scan lazy hash allocation from contains().
class RuleIndex {
public:
    [[nodiscard]] bool prepare_index(const KasumiObservation& observation) noexcept {
        if (!observation.ok || !observation.enabled)
            return true;
        constexpr std::size_t maximum = 1024UL * 1024;
        const auto& rules = observation.rules;
        if (rules.size() > maximum || !entries_.prepare_array(rules.size(), maximum)) {
            errno = rules.size() > maximum ? EOVERFLOW : ENOMEM;
            return false;
        }
        for (const auto& rule : rules)
            if (!entries_.append({&rule})) {
                errno = ENOBUFS;
                return false;
            }
        if (!rules.empty())
            std::sort(entries_.data(), entries_.data() + entries_.size(), Less{});
        return true;
    }
    [[nodiscard]] bool contains(const RuleEvidence& rule) const noexcept {
        const auto entries = entries_.view();
        return !entries.empty() &&
               std::binary_search(entries.begin(), entries.end(), Entry{&rule}, Less{});
    }

private:
    struct Entry {
        const RuleEvidence* rule;
    };
    struct Less {
        bool operator()(Entry lhs, Entry rhs) const noexcept {
            return std::tie(lhs.rule->kind, lhs.rule->path, lhs.rule->source) <
                   std::tie(rhs.rule->kind, rhs.rule->path, rhs.rule->source);
        }
    };
    bounded::Array<Entry> entries_;
};

template <typename Contribution>
void distribute(Database& database, const std::string& backend,
                const std::vector<Contribution>& contributions, bool complete,
                std::vector<Contribution> Record::* destination) {
    struct Target {
        const std::string* id = nullptr;
        Record* record = nullptr;
        const Contribution* previous = nullptr;
    };
    std::vector<Target> targets;
    targets.reserve(database.modules.size());
    for (auto& [id, record] : database.modules) {
        if (record.backend != backend)
            continue;
        record.complete = complete;
        (record.*destination).clear();
        targets.push_back({&id, &record});
    }
    // Database keys already put targets in lookup order. Iterate contributions
    // in their original order, including distinct equal-valued contributions.
    for (const auto& contribution : contributions) {
        for (const auto& owner : contribution.owners) {
            const auto target =
                std::lower_bound(targets.begin(), targets.end(), owner,
                                 [](const auto& entry, const auto& id) { return *entry.id < id; });
            if (target == targets.end() || *target->id != owner ||
                target->previous == &contribution)
                continue;
            (target->record->*destination).push_back(contribution);
            target->previous = &contribution;
        }
    }
}
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
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

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
    const FileDescriptor in(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (in.get() < 0) {
        error = "cannot read " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    if (!read_complete(in.get(), text)) {
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
std::string decode(std::string_view value) {
    if (value.find('\\') == std::string_view::npos)
        return std::string(value);
    std::string result;
    result.reserve(value.size());
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

std::string_view mount_field(std::string_view& remaining) {
    const auto space = remaining.find(' ');
    const auto field = remaining.substr(0, space);
    remaining.remove_prefix(space == std::string_view::npos ? remaining.size() : space + 1);
    return field;
}

bool mount_number(std::string_view field, std::uint64_t& value) {
    if (field.empty())
        return false;
    const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == field.data() + field.size();
}

// Accept only the ordinary proc spelling here. Everything else is retried by
// the original stream parser, including its numeric and whitespace semantics.
// Views stay within text; every published identity owns its strings.
bool parse_proc_mountinfo(std::string_view text, std::vector<MountIdentity>& mounts) {
    if (text.empty() || text.back() != '\n')
        return false;
    std::vector<std::uint64_t> ids;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        text.remove_prefix(newline + 1);
        const auto separator = line.find(" - ");
        if (separator == std::string_view::npos || line.front() == ' ' || line.back() == ' ' ||
            std::any_of(line.begin(), line.end(), [](char c) { return c >= '\t' && c <= '\r'; }) ||
            line.find("  ") != std::string_view::npos)
            return false;
        auto before = line.substr(0, separator);
        auto after = line.substr(separator + 3);
        std::array<std::string_view, 6> fields;
        for (auto& field : fields) {
            field = mount_field(before);
            if (field.empty())
                return false;
        }
        MountIdentity mount;
        std::uint64_t major = 0;
        std::uint64_t minor = 0;
        const auto colon = fields[2].find(':');
        if (!mount_number(fields[0], mount.mount_id) || mount.mount_id == 0 ||
            !mount_number(fields[1], mount.parent_id) || colon == std::string_view::npos ||
            !mount_number(fields[2].substr(0, colon), major) ||
            !mount_number(fields[2].substr(colon + 1), minor) || major > UINT32_MAX ||
            minor > UINT32_MAX)
            return false;
        const auto filesystem = mount_field(after);
        const auto source = mount_field(after);
        const auto options = mount_field(after);
        if (filesystem.empty() || source.empty() || options.empty() || !after.empty())
            return false;
        mount.device = (major << 32) | minor;
        mount.root = decode(fields[3]);
        mount.path = decode(fields[4]);
        mount.filesystem = filesystem;
        mount.source = decode(source);
        if (mount.path.empty() || mount.path.front() != '/' || mount.root.empty() ||
            mount.root.front() != '/')
            return false;
        ids.push_back(mount.mount_id);
        mounts.push_back(std::move(mount));
    }
    std::sort(ids.begin(), ids.end());
    return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}
bool stat_identity_at(int fd, const char* path, int flags, MountIdentity& identity) {
#if defined(__linux__) && defined(__NR_statx)
    struct statx st{};
    constexpr unsigned mask = STATX_INO | STATX_MNT_ID;
    if (syscall(__NR_statx, fd, path, flags, mask, &st) != 0 || (st.stx_mask & mask) != mask)
        return false;
    identity.mount_id = st.stx_mnt_id;
    identity.device = (static_cast<std::uint64_t>(st.stx_dev_major) << 32) | st.stx_dev_minor;
    identity.inode = st.stx_ino;
    return true;
#else
    (void)fd;
    (void)path;
    (void)flags;
    (void)identity;
    return false;
#endif
}
bool stat_identity(const std::string& path, MountIdentity& identity) {
    return stat_identity_at(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
                            identity);
}
bool same_inode(const MountIdentity& lhs, const MountIdentity& rhs) {
    return lhs.mount_id == rhs.mount_id && lhs.device == rhs.device && lhs.inode == rhs.inode;
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
    if (!detail::read_namespace_mountinfo(proc, text, error) ||
        !parse_mountinfo(text, mounts, error))
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
bool hidden_by_ancestor(const MountIdentity& current, const MountIndex& mounts) {
    auto path = fs::path(current.path).parent_path();
    while (!path.empty()) {
        MountIdentity visible;
        const auto probe = runtime_mount_here() ? path.string() : "/proc/1/root" + path.string();
        if (stat_identity(probe, visible)) {
            // A reachable ancestor belongs to another attachment branch: an
            // upper mount is hiding this still-attached lower subtree.
            return mounts.find(visible.mount_id) && !mounts.has_ancestor(current, visible.mount_id);
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
                  const std::string& mount_error, const KasumiObservation& kasumi,
                  const MountIndex& mount_index, RuleIndex& rule_index) {
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
            if (kasumi.enabled && rule_index.contains(rule))
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
        const auto* current = mount_index.find(stored.mount_id);
        MountIdentity visible;
        const auto path = runtime_mount_here() ? stored.path : "/proc/1/root" + stored.path;
        const bool inspected = stat_identity(path, visible);
        const int inspect_error = inspected ? 0 : errno;
        if (!current) {
            // A filtered mountinfo table is not absence evidence when the path
            // still has the captured mount ID, device and inode. Conversely, a
            // failed probe or a visible mount absent from that table is unknown.
            if (inspected && same_inode(stored, visible)) {
                ++present;
                continue;
            }
            if (!inspected && inspect_error != ENOENT && inspect_error != ENOTDIR)
                return unknown(record.backend, "read_failed", "cannot inspect " + stored.path,
                               "mount_identity_unreadable");
            if (inspected && !mount_index.find(visible.mount_id))
                return unknown(record.backend, "mount_table_incomplete");
            continue;
        }
        if (!same_mount(stored, *current))
            return unknown(record.backend, "mount_identity_changed");
        // An upper overlay may cover this mount. mountinfo still identifies the
        // lower attachment; only compare inode when this exact mount is visible.
        if (!inspected) {
            if (hidden_by_ancestor(*current, mount_index)) {
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

bool detail::read_namespace_mountinfo(const std::string& proc, std::string& text,
                                      std::string& error) {
    const auto target = ns_identity(proc + "/ns/mnt");
    if (target.empty()) {
        error = "cannot inspect mount namespace";
        return false;
    }
    if (target == ns_identity("/proc/self/ns/mnt"))
        return read_text(proc + "/mountinfo", text, error);

    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0) {
        error = "cannot open mount observation pipe: " + std::string(std::strerror(errno));
        return false;
    }
    posix::Fd reader(descriptors[0]);
    posix::Fd writer_fd(descriptors[1]);
    const auto child = fork();
    if (child == 0) {
        reader = posix::Fd{};
        const posix::Fd ns(open((proc + "/ns/mnt").c_str(), O_RDONLY | O_CLOEXEC));
        if (ns.get() < 0 || setns(ns.get(), CLONE_NEWNS) != 0) {
            error = "cannot enter mount namespace: " + std::string(std::strerror(errno));
        } else if (ns_identity("/proc/self/ns/mnt") != target) {
            error = "mount namespace changed while querying";
        } else if (read_text(proc + "/mountinfo", text, error)) {
            const bool sent =
                output::write_fd(writer_fd.get(), "1") && output::write_fd(writer_fd.get(), text);
            _exit(sent ? 0 : 1);
        }
        const bool sent =
            output::write_fd(writer_fd.get(), "0") && output::write_fd(writer_fd.get(), error);
        _exit(sent ? 0 : 1);
    }
    writer_fd = posix::Fd{};
    if (child < 0) {
        error = "cannot fork mount observer: " + std::string(std::strerror(errno));
        return false;
    }
    std::string response;
    const bool read_ok = read_complete(reader.get(), response);
    reader = posix::Fd{};
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (!read_ok || waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        response.empty()) {
        error = "incomplete mount namespace observation";
        return false;
    }
    const bool ok = response.front() == '1';
    response.erase(0, 1);
    if (ok)
        text = std::move(response);
    else
        error = std::move(response);
    return ok;
}

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

PendingMountCapture::~PendingMountCapture() {
    if (fd_ >= 0)
        close(fd_);
}
PendingMountCapture::PendingMountCapture(PendingMountCapture&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      contribution_(std::move(other.contribution_)),
      observed_(std::move(other.observed_)),
      resolved_(std::move(other.resolved_)) {}
PendingMountCapture& PendingMountCapture::operator=(PendingMountCapture&& other) noexcept {
    if (this == &other)
        return *this;
    if (fd_ >= 0)
        close(fd_);
    fd_ = std::exchange(other.fd_, -1);
    contribution_ = std::move(other.contribution_);
    observed_ = std::move(other.observed_);
    resolved_ = std::move(other.resolved_);
    return *this;
}

PendingMountCapture capture_mount_id(const std::string& path,
                                     const std::vector<std::string>& owners) {
    PendingMountCapture result;
    result.contribution_ = {path, owners, true, false, {}};
    const auto ns = ns_identity("/proc/self/ns/mnt");
    if (ns.empty() || !resolve_mount_identity(path, result.resolved_, result.observed_))
        return result;
    FileDescriptor fd(open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
    MountIdentity opened;
    if (fd.get() < 0 ||
        !stat_identity_at(fd.get(), "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
                          opened) ||
        !same_inode(opened, result.observed_) || ns_identity("/proc/self/ns/mnt") != ns)
        return result;
    result.observed_.namespace_id = ns;
    result.fd_ = fd.release();
    return result;
}

std::vector<MountContribution> complete_mount_captures(std::vector<PendingMountCapture> captures) {
    std::vector<MountContribution> results;
    results.reserve(captures.size());
    const bool pending = std::any_of(captures.begin(), captures.end(),
                                     [](const auto& capture) { return capture.fd_ >= 0; });
    std::vector<MountIdentity> mounts;
    std::string error;
    const auto ns = pending ? ns_identity("/proc/self/ns/mnt") : std::string{};
    const bool observed = pending && !ns.empty() && read_mounts(mounts, error, true) &&
                          ns_identity("/proc/self/ns/mnt") == ns;
    MountIndex by_id;
    const bool indexed = by_id.prepare_index(mounts);
    for (auto& capture : captures) {
        auto& result = capture.contribution_;
        if (observed && indexed && capture.fd_ >= 0 && capture.observed_.namespace_id == ns) {
            MountIdentity pinned;
            const auto* match = by_id.find(capture.observed_.mount_id);
            if (match && match->namespace_id == ns && match->path == capture.resolved_ &&
                match->device == capture.observed_.device &&
                stat_identity_at(capture.fd_, "",
                                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, pinned) &&
                same_inode(pinned, capture.observed_)) {
                result.identity = *match;
                result.identity.inode = capture.observed_.inode;
                result.path = capture.resolved_;
                result.captured = true;
            }
        }
        results.push_back(std::move(result));
    }
    return results;
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
    MountIndex by_id;
    if (!by_id.prepare_index(mounts))
        return;
    for (auto& contribution : contributions) {
        if (!contribution.committed)
            continue;
        std::string resolved;
        MountIdentity visible;
        if (!resolve_mount_identity(contribution.path, resolved, visible))
            continue;
        const auto* match = by_id.find(visible.mount_id);
        if (!match || match->path != resolved || match->device != visible.device)
            continue;
        contribution.identity = *match;
        contribution.identity.inode = visible.inode;
        contribution.path = resolved;
        contribution.captured = true;
    }
}
bool record_mounts(const std::string& backend, const std::vector<MountContribution>& contributions,
                   bool complete) {
    return update([&](Database& database) {
        distribute(database, backend, contributions, complete, &Record::mounts);
    });
}
bool record_rules(const std::vector<RuleEvidence>& rules, bool complete) {
    return update([&](Database& database) {
        distribute(database, "kasumi", rules, complete, &Record::rules);
    });
}

bool detail::prepare_snapshot(SnapshotContext& context, Snapshot& result) {
    auto& database = context.database;
    std::string reason;
    std::string error;
    context.lock = posix::Fd(open(lock_file().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    const auto& fd = context.lock;
    const bool locked = fd.get() >= 0 && flock(fd.get(), LOCK_SH | LOCK_NB) == 0;
    const int lock_error = locked ? 0 : errno;
    // Acquire before reading. A successful shared lock makes this one read
    // stable; failed acquisition still loads IDs for the unknown/updating view.
    const bool loaded = load(database, reason, error);
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
            value.has_contributions = !record.mounts.empty() || !record.rules.empty();
            result.modules.emplace(id, std::move(value));
        }
        return false;
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
            value.has_contributions = !record.mounts.empty() || !record.rules.empty();
            result.modules.emplace(id, std::move(value));
        }
        return false;
    }
    const bool needs_mounts =
        std::any_of(database.modules.begin(), database.modules.end(),
                    [](const auto& entry) { return !entry.second.mounts.empty(); });
    context.mounts_ok =
        !needs_mounts || read_mounts(context.mounts, context.mount_error, runtime_mount_here());
    context.needs_kasumi =
        std::any_of(database.modules.begin(), database.modules.end(),
                    [](const auto& entry) { return !entry.second.rules.empty(); });
    return true;
}

void detail::finish_snapshot(SnapshotContext& context, const KasumiObservation& observation,
                             Snapshot& result) {
    const auto& database = context.database;
    const auto& mounts = context.mounts;
    KasumiObservation decoded;
    const KasumiObservation* observed = &observation;
    if (observation.ok && !observation.text.empty()) {
        decoded.ok = rules_from_text(observation.text, decoded.rules);
        decoded.enabled = observation.enabled;
        if (!decoded.ok) {
            decoded.error_code = "ambiguous_rule_encoding";
            decoded.error = "cannot parse complete Kasumi rule snapshot";
        }
        observed = &decoded;
    }
    const auto& kasumi = *observed;
    MountIndex mount_index;
    RuleIndex rule_index;
    if (!mount_index.prepare_index(mounts) || !rule_index.prepare_index(kasumi)) {
        result.missing = unknown({}, "read_failed", "cannot prepare runtime snapshot index",
                                 "snapshot_index_unavailable");
        for (const auto& [id, record] : database.modules) {
            auto value = result.missing;
            value.backend = record.backend;
            value.has_contributions = !record.mounts.empty() || !record.rules.empty();
            result.modules.emplace(id, std::move(value));
        }
        return;
    }
    for (const auto& [id, record] : database.modules) {
        auto value = summarize(record, mounts, context.mounts_ok, context.mount_error, kasumi,
                               mount_index, rule_index);
        value.has_contributions = !record.mounts.empty() || !record.rules.empty();
        result.modules.emplace(id, std::move(value));
    }
}

Snapshot read_snapshot(const KasumiObservation& observation) {
    return read_snapshot_observed([&]() -> const KasumiObservation& { return observation; });
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
    // Custom locales may redefine numeric or whitespace extraction. Keep the
    // stream path for them and for every input outside the proc fast path.
    if (std::locale() == std::locale::classic() && parse_proc_mountinfo(text, mounts))
        return true;
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

bool read_complete(int fd, std::string& output) {
    output.clear();
    // Only the byte count returned by read() is consumed.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    std::array<char, 4096> buffer;
    for (;;) {
        const auto count = read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (!count)
            return true;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
}
}  // namespace kagami::mount::module_runtime
