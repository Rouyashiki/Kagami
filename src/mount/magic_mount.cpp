#include "mount/magic_mount.hpp"

#include "core/runtime.hpp"
#include "mount/module_runtime.hpp"
#include "mount/mount_fs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <climits>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>

#include <string>
#include <vector>

#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

// Lazy-tmpfs magic mount: build a merged module tree and walk from "/". A dir
// becomes a tmpfs skeleton only when a direct child needs it; otherwise it is
// recursed into and left as-is, so partition roots and untouched dirs stay put.

namespace kagami::mount::magic {

namespace fs = std::filesystem;
using fsutil::mlog;

enum class NType : std::uint8_t { Regular, Directory, Symlink, Whiteout };

struct Node {
    std::string name;
    NType type = NType::Directory;
    std::map<std::string, Node> children;
    std::string module_path;  // backing module file; "" if mirror-only / root
    std::string owner;
    std::string replace_owner;
    bool replace = false;  // opaque dir (.replace / trusted.overlay.opaque)
};

struct Walk {
    std::vector<std::string> committed;  // mount points to record for teardown
    std::vector<module_runtime::MountContribution> observations;
    std::vector<std::size_t> pending_binds;
    int files = 0;
    int tmpfs_dirs = 0;
    int symlinks = 0;
};

namespace {
std::string state_file() {
    return (runtime_data_dir() / "run" / "magic_mounts.list").string();
}

// Report a committed mount to KernelSU for per-app unmount. Skips lib dirs:
// pairip-protected APKs verify native libs after zygote fork, and detaching the
// overlay mid-flight crashes them.
bool ksu_umount_add(const std::string& path) {
    static const char* const kIgnore[] = {"/system/lib", "/system/lib64", "/vendor/lib",
                                          "/vendor/lib64"};
    for (const char* ig : kIgnore) {
        const std::string p(ig);
        if (path == p || path.rfind(p + "/", 0) == 0) {
            return true;
        }
    }
    return fsutil::register_umount(path);
}

bool lexists(const std::string& p) {
    struct stat st{};
    return lstat(p.c_str(), &st) == 0;
}

std::string join(const std::string& base, const std::string& name) {
    std::string path = base;
    if (path != "/")
        path += '/';
    path += name;
    return path;
}

[[nodiscard]] bool scan_error(const std::string& message) {
    mlog("magic: " + message, logging::Level::Error);
    return false;
}

bool type_from_lstat(const std::string& path, const struct stat& st, NType& type) {
    if (S_ISCHR(st.st_mode) && st.st_rdev == 0) {
        type = NType::Whiteout;
        return true;
    }
    if (S_ISDIR(st.st_mode)) {
        type = NType::Directory;
        return true;
    }
    if (S_ISLNK(st.st_mode)) {
        type = NType::Symlink;
        return true;
    }
    if (S_ISREG(st.st_mode)) {
        type = NType::Regular;
        if (st.st_size == 0) {
            if (lgetxattr(path.c_str(), "trusted.overlay.whiteout", nullptr, 0) >= 0)
                type = NType::Whiteout;
            else if (errno != ENODATA && errno != EOPNOTSUPP)
                return scan_error("cannot read whiteout metadata: " + path);
        }
        return true;
    }
    return scan_error("unsupported module entry: " + path);
}

bool dir_is_replace(const std::string& path, bool& opaque) {
    if (!fsutil::directory_is_opaque(path, opaque))
        return scan_error("cannot read replacement metadata: " + path);
    return true;
}

bool clone_symlink(const std::string& src, const std::string& dst) {
    char tgt[PATH_MAX];
    const ssize_t n = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
    if (n < 0) {
        return false;
    }
    tgt[n] = '\0';
    if (symlink(tgt, dst.c_str()) != 0 && errno != EEXIST) {
        return false;
    }
    std::string ctx;
    if (fsutil::get_context(src, ctx)) {
        fsutil::set_context(dst, ctx);
    }
    return true;
}

// Higher-priority entries win; opaque directories stop lower-layer merging.
bool collect_into(Node& parent, const std::string& dir, const std::string& owner, bool& has) {
    const std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(dir.c_str()), closedir);
    if (!directory)
        return scan_error("cannot scan " + dir + ": " + std::strerror(errno));
    for (;;) {
        errno = 0;
        const auto* entry = readdir(directory.get());
        if (!entry) {
            if (errno)
                return scan_error("cannot read " + dir + ": " + std::strerror(errno));
            break;
        }
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || name == ".replace")
            continue;
        const std::string path = join(dir, name);
        struct stat st{};
        if (lstat(path.c_str(), &st) != 0)
            return scan_error("cannot stat " + path + ": " + std::strerror(errno));
        NType type;
        if (!type_from_lstat(path, st, type))
            return false;
        auto existing = parent.children.find(name);
        if (existing != parent.children.end()) {
            auto& node = existing->second;
            if (node.type != NType::Directory || node.replace) {
                has = true;
                continue;
            }
            if (type != NType::Directory) {
                node.replace = true;
                node.replace_owner = owner;
                has = true;
                continue;
            }
            if (!dir_is_replace(path, node.replace))
                return false;
            if (node.replace)
                node.replace_owner = owner;
            if (!collect_into(node, path, owner, has))
                return false;
            has = node.replace || has;
        } else {
            Node node;
            node.name = name;
            node.type = type;
            node.module_path = path;
            node.owner = owner;
            if (type == NType::Directory) {
                if (!dir_is_replace(path, node.replace))
                    return false;
                if (node.replace)
                    node.replace_owner = owner;
                if (!collect_into(node, path, owner, has))
                    return false;
            }
            has = true;
            parent.children.emplace(name, std::move(node));
        }
    }
    return true;
}

// Build the merged root tree: module system/ trees, with vendor/product/... moved
// out to root level when the device exposes them as /system/<p> symlinks.
bool collect_module_files(const std::vector<ModuleEntry>& modules, Arguments extra,
                          std::optional<Node>& result) {
    Node root;
    root.type = NType::Directory;
    Node system;
    system.name = "system";
    system.type = NType::Directory;

    bool has = false;
    for (const auto& m : modules) {
        const std::string ms = m.path.string() + "/system";
        struct stat st{};
        if (lstat(ms.c_str(), &st) != 0) {
            if (errno == ENOENT)
                continue;
            return scan_error("cannot stat " + ms + ": " + std::strerror(errno));
        }
        if (!S_ISDIR(st.st_mode))
            return scan_error("module system entry is not a directory: " + ms);
        bool replace = false;
        if (!dir_is_replace(ms, replace))
            return false;
        if (replace)
            return scan_error("partition-root replacement is unsupported: " + ms);
        if (!collect_into(system, ms, m.id, has))
            return false;
    }
    if (!has) {
        result.reset();
        return true;
    }

    const auto move_partition = [&](const std::string& part, bool require_symlink) {
        std::error_code ec;
        const bool ok = fs::is_directory("/" + part, ec) &&
                        (!require_symlink || fs::is_symlink("/system/" + part, ec));
        if (!ok)
            return true;
        auto it = system.children.find(part);
        if (it != system.children.end()) {
            if (it->second.replace)
                return scan_error("partition-root replacement is unsupported: /" + part);
            Node moved = std::move(it->second);
            system.children.erase(it);
            // Partition roots are not replaced wholesale; existing files can be bound individually.
            moved.module_path.clear();
            moved.owner.clear();
            moved.replace = false;
            root.children.emplace(part, std::move(moved));
        }
        return true;
    };

    const std::pair<const char*, bool> builtin[] = {
        {"vendor", true}, {"system_ext", true}, {"product", true}, {"odm", false}};
    for (const auto& [part, req] : builtin) {
        if (!move_partition(part, req))
            return false;
    }
    for (const auto& part : extra) {
        if (part == "system") {
            continue;
        }
        bool is_builtin = false;
        for (const auto& bp : builtin) {
            if (part == bp.first) {
                is_builtin = true;
            }
        }
        if (!is_builtin) {
            if (!move_partition(std::string(std::string_view(part)), false))
                return false;
        }
    }

    root.children.emplace("system", std::move(system));
    result = std::move(root);
    return true;
}

// Create the tmpfs skeleton dir at `work`, cloning mode/owner/SELinux context
// from the real dir (or the module dir when the real one does not exist).
bool tmpfs_skeleton(const std::string& real, const std::string& work, const Node& node) {
    std::error_code ec;
    fs::create_directories(work, ec);
    if (ec) {
        mlog("skeleton mkdir " + work + " failed: " + ec.message(), logging::Level::Error);
        return false;
    }
    struct stat st{};
    const std::string src =
        lstat(real.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? real : node.module_path;
    if (src.empty()) {
        return false;
    }
    fsutil::clone_attr(src, work);
    return true;
}

// Mirror one unmodified real entry into the skeleton: dirs recursed entry by
// entry, files bind-mounted, symlinks recreated.
bool mount_mirror(const std::string& real, const std::string& work, const std::string& name) {
    const std::string r = join(real, name);
    const std::string w = join(work, name);
    struct stat st{};
    if (lstat(r.c_str(), &st) != 0) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(w.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
        fsutil::clone_attr(r, w);
        // Mirror each child individually; binding the whole subtree could carry
        // nested mounts and break the moved skeleton.
        DIR* d = opendir(r.c_str());
        if (!d) {
            mlog("mirror opendir " + r + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        bool ok = true;
        struct dirent* e;
        for (;;) {
            errno = 0;
            e = readdir(d);
            if (!e) {
                ok = errno == 0;
                break;
            }
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (!mount_mirror(r, w, e->d_name)) {
                ok = false;
                break;
            }
        }
        closedir(d);
        return ok;
    }
    if (S_ISLNK(st.st_mode)) {
        return clone_symlink(r, w);
    }
    if (S_ISREG(st.st_mode)) {
        const int fd = open(w.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            close(fd);
        }
        if (!fsutil::bind_mount(r, w)) {
            mlog("mirror bind " + r + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        return true;
    }
    return true;  // skip device/socket/fifo
}

bool do_mount(Node& node, const std::string& real, const std::string& work, bool has_tmpfs,
              Walk& w);

// Run before the walk consumes children. Directory metadata inherited from
// stock is not a module contribution; replacement and newly added directories
// are, as are symlink/whiteout nodes. File binds have independent identities.
void contribution_owners(const Node& node, const std::string& real, std::set<std::string>& owners) {
    struct stat st{};
    const bool exists = lstat(real.c_str(), &st) == 0;
    if (node.type != NType::Directory) {
        if (node.type != NType::Regular && !node.owner.empty() &&
            (node.type != NType::Whiteout || exists))
            owners.insert(node.owner);
        return;
    }
    if (node.replace && !node.replace_owner.empty())
        owners.insert(node.replace_owner);
    if ((!exists || !S_ISDIR(st.st_mode)) && !node.owner.empty())
        owners.insert(node.owner);
    for (const auto& [name, child] : node.children)
        contribution_owners(child, join(real, name), owners);
}

bool do_directory(Node& node, const std::string& real, const std::string& work, bool has_tmpfs,
                  Walk& w) {
    struct stat real_stat{};
    const int result = lstat(real.c_str(), &real_stat);
    if (result != 0 && errno != ENOENT && errno != ENOTDIR) {
        mlog("magic: cannot stat directory " + real + ": " + std::strerror(errno),
             logging::Level::Error);
        return false;
    }
    const bool real_directory = result == 0 && S_ISDIR(real_stat.st_mode);
    bool tmpfs = !has_tmpfs && node.replace && !node.module_path.empty();

    if (!has_tmpfs && !tmpfs) {
        for (auto& [name, child] : node.children) {
            const std::string real_child = join(real, name);
            bool need;
            if (child.type == NType::Symlink) {
                need = true;
            } else if (child.type == NType::Regular) {
                struct stat st{};
                need = lstat(real_child.c_str(), &st) != 0 || !S_ISREG(st.st_mode);
            } else if (child.type == NType::Whiteout) {
                need = lexists(real_child);
            } else {
                // A dir needs a skeleton when a child's real counterpart is not a
                // directory (a file/symlink) or is missing.
                struct stat st{};
                if (lstat(real_child.c_str(), &st) == 0) {
                    need = !S_ISDIR(st.st_mode);
                } else {
                    need = true;
                }
            }
            if (need) {
                if (logging::enabled(logging::Level::Debug)) {
                    mlog(std::string("  need-skeleton at ")
                             .append(real)
                             .append(" due to child '")
                             .append(name)
                             .append("' (real=")
                             .append(real_child)
                             .append(")"));
                }
                if (node.module_path.empty()) {
                    mlog(
                        "magic: cannot add, delete or change the type of a partition-root entry: " +
                            real_child,
                        logging::Level::Error);
                    return false;
                }
                tmpfs = true;
                break;
            }
        }
    }

    const bool now_tmpfs = tmpfs || has_tmpfs;
    const auto observation_index = w.observations.size();
    const auto pending_start = w.pending_binds.size();
    if (tmpfs) {
        std::set<std::string> owners;
        contribution_owners(node, real, owners);
        w.observations.push_back({real, {owners.begin(), owners.end()}, false, false, {}});
    }
    if (now_tmpfs && !tmpfs_skeleton(real, work, node)) {
        return false;
    }
    if (tmpfs && !fsutil::bind_mount(work, work)) {  // make the skeleton movable
        mlog("self-bind " + work + " failed: " + std::strerror(errno), logging::Level::Error);
        return false;
    }

    // Only a skeleton needs the untouched stock entries. Direct binds can walk
    // the sparse module tree without enumerating every sibling in the real dir.
    if (now_tmpfs && real_directory && !node.replace) {
        DIR* d = opendir(real.c_str());
        if (!d) {
            mlog("magic: cannot read directory " + real + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        {
            struct dirent* e;
            for (;;) {
                errno = 0;
                e = readdir(d);
                if (!e) {
                    const int error = errno;
                    closedir(d);
                    if (error)
                        return false;
                    break;
                }
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                const std::string name = e->d_name;
                auto it = node.children.find(name);
                if (it != node.children.end()) {
                    Node child = std::move(it->second);
                    node.children.erase(it);
                    if (!do_mount(child, join(real, name), join(work, name), now_tmpfs, w)) {
                        closedir(d);
                        return false;
                    }
                } else if (now_tmpfs) {
                    if (!mount_mirror(real, work, name)) {
                        closedir(d);
                        return false;
                    }
                }
            }
        }
    }

    // Without a skeleton these are all covered entries; with one they are the
    // new children left after mirroring stock.
    for (auto& [name, child] : node.children) {
        if (!do_mount(child, join(real, name), join(work, name), now_tmpfs, w)) {
            return false;
        }
    }

    if (tmpfs) {
        if (::mount(nullptr, work.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) != 0)
            return false;
        if (::mount(work.c_str(), real.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
            mlog("move " + work + " -> " + real + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        w.committed.push_back(real);
        w.observations[observation_index].committed = true;
        // Moving the skeleton gives its payload binds their final mountpoints.
        // Keep those identities separate from the directory they share.
        for (std::size_t i = pending_start; i < w.pending_binds.size(); ++i) {
            w.observations[w.pending_binds[i]].committed = true;
        }
        w.pending_binds.resize(pending_start);
        if (::mount("none", real.c_str(), nullptr, MS_PRIVATE | MS_REC, nullptr) != 0)
            return false;
        ++w.tmpfs_dirs;
        mlog("magic: skeletoned " + real);
    }
    return true;
}

bool do_mount(Node& node, const std::string& real, const std::string& work, bool has_tmpfs,
              Walk& w) {
    switch (node.type) {
    case NType::Symlink:
        if (node.module_path.empty()) {
            mlog("symlink node without module backing: " + real);
            return false;
        }
        if (!clone_symlink(node.module_path, work)) {
            return false;
        }
        ++w.symlinks;
        return true;
    case NType::Regular: {
        if (node.module_path.empty())
            return false;
        const std::string& target = has_tmpfs ? work : real;
        if (has_tmpfs) {
            const int fd = open(target.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
            if (fd < 0)
                return false;
            if (close(fd) != 0)
                return false;
        } else {
            struct stat st{};
            if (lstat(target.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                mlog("magic: direct bind requires an existing regular file: " + target,
                     logging::Level::Error);
                return false;
            }
        }
        const auto observation_index = w.observations.size();
        w.observations.push_back({real, {node.owner}, false, false, {}});
        if (!fsutil::bind_mount(node.module_path, target)) {
            mlog("magic: bind " + node.module_path + " -> " + target + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        if (!has_tmpfs) {
            w.committed.push_back(target);
            w.observations[observation_index].committed = true;
        } else {
            w.pending_binds.push_back(observation_index);
        }
        if (::mount(nullptr, target.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) !=
            0) {
            mlog("magic: readonly remount failed: " + target, logging::Level::Error);
            return false;
        }
        if (!has_tmpfs && ::mount("none", target.c_str(), nullptr, MS_PRIVATE, nullptr) != 0) {
            mlog("magic: private propagation failed: " + target, logging::Level::Error);
            return false;
        }
        ++w.files;
        return true;
    }
    case NType::Whiteout:
        return true;  // absence in the skeleton hides the original
    case NType::Directory:
        return do_directory(node, real, work, has_tmpfs, w);
    }
    return false;
}

// Mount points whose mount source equals `source`. Our skeletons and work tmpfs
// use config.mount_source; real partitions are block-backed, so teardown can match
// ours without ever touching a real partition.
std::set<std::string> mounts_with_source(std::string_view source) {
    std::set<std::string> out;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        std::istringstream pre(line.substr(0, sep));
        std::vector<std::string> f;
        std::string t;
        while (pre >> t) {
            f.push_back(t);
        }
        if (f.size() < 5) {
            continue;
        }
        std::istringstream post(line.substr(sep + 3));
        std::string fstype;
        std::string src;
        post >> fstype >> src;
        if (fsutil::decode_mount_path(src) == source) {
            out.insert(fsutil::decode_mount_path(f[4]));
        }
    }
    return out;
}
}  // namespace

using fsutil::MountRecord;

namespace {
bool read_mounts(std::vector<MountRecord>& records, bool& legacy) {
    return fsutil::read_mount_journal(state_file(), records, legacy);
}

bool rollback(const std::vector<std::string>& paths) {
    const std::set<std::string, std::greater<>> ordered(paths.begin(), paths.end());
    bool ok = true;
    for (const auto& path : ordered) {
        if (umount2(path.c_str(), MNT_DETACH) != 0) {
            mlog("magic: rollback failed for " + path + ": " + std::strerror(errno),
                 logging::Level::Error);
            ok = false;
        } else {
            ok = fsutil::unregister_umount(path) && ok;
        }
    }
    return ok;
}
}  // namespace

bool unmount_all(const Config& config) {
    std::vector<MountRecord> records;
    bool legacy;
    if (!read_mounts(records, legacy)) {
        mlog("magic: invalid or unreadable mount journal", logging::Level::Error);
        return false;
    }
    std::map<std::string, MountRecord, std::greater<>> ordered;
    for (const auto& record : records)
        ordered.emplace(record.path, record);
    const auto old_mounts =
        legacy ? mounts_with_source(config.mount_source) : std::set<std::string>{};
    bool ok = true;
    for (const auto& [path, record] : ordered) {
        if (legacy ? !old_mounts.count(path) : !fsutil::mount_matches(record)) {
            if (!legacy)
                ok = fsutil::unregister_umount(path) && ok;
            continue;
        }
        if (umount2(path.c_str(), MNT_DETACH) != 0) {
            mlog("magic: detach failed for " + path + ": " + std::strerror(errno),
                 logging::Level::Error);
            ok = false;
        } else {
            ok = fsutil::unregister_umount(path) && ok;
        }
    }
    if (ok) {
        std::error_code ec;
        fs::remove(state_file(), ec);
        ok = !ec;
    }
    return ok;
}

std::vector<std::string> active_mounts(const Config& config) {
    std::vector<MountRecord> records;
    bool legacy;
    if (!read_mounts(records, legacy))
        return {};
    const auto old_mounts =
        legacy ? mounts_with_source(config.mount_source) : std::set<std::string>{};
    std::vector<std::string> paths;
    for (const auto& record : records) {
        if (legacy ? old_mounts.count(record.path) != 0 : fsutil::mount_matches(record, true))
            paths.push_back(record.path);
    }
    return paths;
}

namespace {
bool save_mounts(const std::vector<std::string>& mounts) {
    return fsutil::write_mount_journal(state_file(), mounts);
}
}  // namespace

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config) {
    if (!unmount_all(config))
        return false;

    std::error_code ec;
    fs::create_directories(runtime_data_dir() / "run", ec);
    if (ec) {
        mlog("magic: cannot create journal directory: " + ec.message(), logging::Level::Error);
        return false;
    }

    std::optional<Node> root;
    if (!collect_module_files(modules, config.partitions, root))
        return false;
    if (!root) {
        (void)module_runtime::record_mounts("magic", {}, true);
        mlog("magic: no module files to mount");
        return true;
    }

    const std::string work(std::string_view(config.work_dir));
    if (!fsutil::prepare_empty_mountpoint(work))
        return false;
    if (::mount(config.mount_source.c_str(), work.c_str(), "tmpfs", 0, nullptr) != 0) {
        mlog("magic: work tmpfs failed: " + std::string(std::strerror(errno)),
             logging::Level::Error);
        return false;
    }
    if (::mount("none", work.c_str(), nullptr, MS_PRIVATE | MS_REC, nullptr) != 0 ||
        !save_mounts({work})) {
        umount2(work.c_str(), MNT_DETACH);
        return false;
    }

    Walk w;
    bool ok = do_mount(*root, "/", work, false, w);
    module_runtime::capture_mounts(w.observations);
    w.committed.push_back(work);

    if (!save_mounts(w.committed)) {
        const bool cleaned = rollback(w.committed);
        (void)module_runtime::record_mounts("magic", w.observations, cleaned);
        return false;
    }
    for (const auto& m : w.committed)
        ok = ksu_umount_add(m) && ok;
    bool cleaned = false;
    if (!ok)
        cleaned = unmount_all(config);
    (void)module_runtime::record_mounts("magic", w.observations, ok || cleaned);
    mlog("magic: files=" + std::to_string(w.files) + " skeletons=" + std::to_string(w.tmpfs_dirs) +
         " symlinks=" + std::to_string(w.symlinks));
    return ok;
}

bool is_active(const Config& config) {
    return !active_mounts(config).empty();
}

bool normalize_module(const std::string& module_path) {
    struct stat st{};
    if (lstat(module_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    struct Change {
        std::string top, sys;
        bool move;
    };
    std::vector<Change> changes;
    std::error_code ec;
    const auto inspect = [](const std::string& path, struct stat& value) {
        if (lstat(path.c_str(), &value) == 0)
            return 1;
        return errno == ENOENT ? 0 : -1;
    };
    const std::string system = module_path + "/system";
    const int system_exists = inspect(system, st);
    if (system_exists < 0 || (system_exists && !S_ISDIR(st.st_mode)))
        return false;
    for (const auto* part : {"vendor", "system_ext", "product", "odm", "oem"}) {
        if (!(fs::is_directory(std::string("/") + part, ec) &&
              fs::is_symlink(std::string("/system/") + part, ec)))
            continue;
        const std::string top = module_path + "/" + part;
        const std::string sys = system + "/" + part;
        struct stat top_stat{};
        struct stat sys_stat{};
        const int has_top = inspect(top, top_stat);
        const int has_sys = inspect(sys, sys_stat);
        if (has_top < 0 || has_sys < 0)
            return false;
        if (has_top && S_ISLNK(top_stat.st_mode)) {
            const auto resolved_top = fs::weakly_canonical(top, ec);
            if (ec)
                return false;
            const auto resolved_sys = fs::weakly_canonical(sys, ec);
            if (ec || resolved_top != resolved_sys)
                return false;
            continue;
        }
        if ((has_top && !S_ISDIR(top_stat.st_mode)) || (has_sys && !S_ISDIR(sys_stat.st_mode)) ||
            (has_top && has_sys)) {
            mlog(std::string("normalize: conflicting partition entries: ")
                     .append(top)
                     .append(" and ")
                     .append(sys),
                 logging::Level::Error);
            return false;
        }
        if (has_top || has_sys)
            changes.push_back({top, sys, has_top != 0});
    }
    for (const auto& change : changes) {
        if (change.move) {
            fs::create_directories(system, ec);
            if (ec)
                return false;
            fs::rename(change.top, change.sys, ec);
            if (ec)
                return false;
        }
        const auto link = "./system/" + fs::path(change.sys).filename().string();
        if (symlink(link.c_str(), change.top.c_str()) != 0) {
            mlog("normalize: symlink " + change.top + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
    }
    return true;
}

}  // namespace kagami::mount::magic
