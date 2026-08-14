#include "mount/overlayfs.hpp"

#include "kagami/kasumi_client.hpp"
#include "mount/backend.hpp"
#include "mount/mount_fs.hpp"
#include "mount/storage.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// The new mount API (fsopen/fsconfig/fsmount/move_mount). bionic doesn't wrap
// these at our minSdk, so invoke them by number; the values are stable across
// the generic syscall table (arm64/x86_64).
#ifndef __NR_fsopen
#define __NR_fsopen 430
#endif
#ifndef __NR_fsconfig
#define __NR_fsconfig 431
#endif
#ifndef __NR_fsmount
#define __NR_fsmount 432
#endif
#ifndef __NR_move_mount
#define __NR_move_mount 429
#endif
#ifndef __NR_open_tree
#define __NR_open_tree 428
#endif

#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif

namespace kagami::mount::overlay {

namespace fs = std::filesystem;
using fsutil::mlog;

static constexpr unsigned kFsopenCloexec = 0x00000001u;
static constexpr unsigned kFsconfigSetString = 1u;
static constexpr unsigned kFsconfigCmdCreate = 6u;
static constexpr unsigned kFsmountCloexec = 0x00000001u;
static constexpr unsigned kMoveMountEmptyPath = 0x00000004u;
static constexpr unsigned kOpenTreeClone = 0x00000001u;     // OPEN_TREE_CLONE
static constexpr unsigned kOpenTreeCloexec = 0x00080000u;   // OPEN_TREE_CLOEXEC (== O_CLOEXEC)

static int sys_fsopen(const char* fsname, unsigned flags) {
    return static_cast<int>(syscall(__NR_fsopen, fsname, flags));
}
static int sys_open_tree(int dfd, const char* path, unsigned flags) {
    return static_cast<int>(syscall(__NR_open_tree, dfd, path, flags));
}
static int sys_fsconfig(int fd, unsigned cmd, const char* key, const char* value, int aux) {
    return static_cast<int>(syscall(__NR_fsconfig, fd, cmd, key, value, aux));
}
static int sys_fsmount(int fd, unsigned flags, unsigned attr) {
    return static_cast<int>(syscall(__NR_fsmount, fd, flags, attr));
}
static int sys_move_mount(int from_fd, const char* from, int to_fd, const char* to, unsigned flags) {
    return static_cast<int>(syscall(__NR_move_mount, from_fd, from, to_fd, to, flags));
}

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Mount an overlay at dest: lower_dirs stacked over lowest, with an optional
// writable upper/work layer. Tries the fsopen API (no length limit on the
// lowerdir list) and falls back to classic mount(2).
static bool mount_overlayfs(const std::vector<std::string>& lower_dirs, const std::string& lowest,
                            const std::string& upperdir, const std::string& workdir,
                            const std::string& dest, const std::string& source) {
    std::string lowerdir;
    for (const auto& dir : lower_dirs) {
        lowerdir += dir;
        lowerdir += ':';
    }
    lowerdir += lowest;

    const bool writable = !upperdir.empty() && !workdir.empty() && fs::exists(upperdir) &&
                          fs::exists(workdir);

    const int fsfd = sys_fsopen("overlay", kFsopenCloexec);
    if (fsfd >= 0) {
        bool ok = sys_fsconfig(fsfd, kFsconfigSetString, "lowerdir", lowerdir.c_str(), 0) == 0;
        if (ok && writable) {
            ok = sys_fsconfig(fsfd, kFsconfigSetString, "upperdir", upperdir.c_str(), 0) == 0 &&
                 sys_fsconfig(fsfd, kFsconfigSetString, "workdir", workdir.c_str(), 0) == 0;
        }
        if (ok) {
            ok = sys_fsconfig(fsfd, kFsconfigSetString, "source", source.c_str(), 0) == 0;
        }
        if (ok) {
            ok = sys_fsconfig(fsfd, kFsconfigCmdCreate, nullptr, nullptr, 0) == 0;
        }
        const int mfd = ok ? sys_fsmount(fsfd, kFsmountCloexec, 0) : -1;
        if (mfd >= 0) {
            const bool moved =
                sys_move_mount(mfd, "", AT_FDCWD, dest.c_str(), kMoveMountEmptyPath) == 0;
            close(mfd);
            close(fsfd);
            if (moved) {
                return true;
            }
        } else {
            close(fsfd);
        }
    }

    std::string data = "lowerdir=" + lowerdir;
    if (writable) {
        data += ",upperdir=" + upperdir + ",workdir=" + workdir;
    }
    if (::mount(source.c_str(), dest.c_str(), "overlay", 0, data.c_str()) == 0) {
        return true;
    }
    mlog("overlay: mount " + dest + " failed: " + std::strerror(errno));
    return false;
}

// Re-establish a (possibly overlay-shadowed) sub-mount onto dst. open_tree clones
// the source mount tree recursively and move_mount attaches it, faithfully
// reproducing sub-mounts even when the source path is shadowed by an overlay;
// fall back to a classic recursive bind if the new mount API is unavailable.
static bool rbind_mount(const std::string& src, const std::string& dst) {
    const int tree =
        sys_open_tree(AT_FDCWD, src.c_str(), kOpenTreeClone | kOpenTreeCloexec | AT_RECURSIVE);
    if (tree >= 0) {
        const bool ok = sys_move_mount(tree, "", AT_FDCWD, dst.c_str(), kMoveMountEmptyPath) == 0;
        close(tree);
        if (ok) {
            return true;
        }
    }
    return ::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND | MS_REC, nullptr) == 0;
}

// Mount points strictly under root (a sub-mount shadowed once we overlay root),
// sorted shallowest-first.
static std::vector<std::string> child_mounts(const std::string& root) {
    std::set<std::string> found;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::vector<std::string> f;
        std::string tok;
        for (int i = 0; i < 5 && iss >> tok; ++i) {
            f.push_back(tok);
        }
        if (f.size() < 5) {
            continue;
        }
        const std::string& mp = f[4];
        if (mp != root && starts_with(mp, root + "/")) {
            found.insert(mp);
        }
    }
    return {found.begin(), found.end()};
}

// Re-establish a sub-mount that the root overlay just shadowed: overlay the
// modules that touch it on top of the stock content, or bind the stock back.
static bool mount_overlay_child(const std::string& mount_point, const std::string& relative,
                                const std::vector<std::string>& module_roots,
                                const std::string& stock, const std::string& source) {
    bool touched = false;
    for (const auto& root : module_roots) {
        if (fs::exists(root + relative)) {
            touched = true;
            break;
        }
    }
    if (!touched) {
        return rbind_mount(stock, mount_point);
    }

    std::error_code ec;
    if (!fs::is_directory(stock, ec)) {
        return true;
    }

    std::vector<std::string> lower;
    for (const auto& root : module_roots) {
        const std::string dir = root + relative;
        if (fs::is_directory(dir, ec)) {
            lower.push_back(dir);
        } else if (fs::exists(dir)) {
            return true; // a module replaced the stock dir with a file
        }
    }
    if (lower.empty()) {
        return true;
    }
    if (!mount_overlayfs(lower, stock, "", "", mount_point, source)) {
        return rbind_mount(stock, mount_point);
    }
    return true;
}

// Overlay module_roots over the stock partition at root, then re-overlay every
// sub-mount the root overlay shadowed. The stock layer is the pre-overlay root,
// reached as "." after chdir (the cwd stays pinned to it once root is covered).
static bool mount_overlay(const std::string& root, const std::vector<std::string>& module_roots,
                          const std::string& upperdir, const std::string& workdir,
                          const std::string& source) {
    mlog("overlay: " + root);
    if (chdir(root.c_str()) != 0) {
        mlog("overlay: chdir " + root + " failed: " + std::strerror(errno));
        return false;
    }
    const std::string stock = ".";
    const std::vector<std::string> children = child_mounts(root);

    if (!mount_overlayfs(module_roots, stock, upperdir, workdir, root, source)) {
        return false;
    }
    for (const auto& mp : children) {
        const std::string relative = mp.substr(root.size());
        const std::string stock_child = stock + relative;
        if (!fs::exists(stock_child)) {
            continue;
        }
        if (!mount_overlay_child(mp, relative, module_roots, stock_child, source)) {
            mlog("overlay: child " + mp + " failed; reverting " + root);
            umount2(root.c_str(), MNT_DETACH);
            return false;
        }
    }
    return true;
}

// True if name is a managed partition other than "system" (vendor/product/...).
// These appear under a module's system/ tree and remap to /<name>.
static bool is_sub_partition(const std::string& name, const std::vector<std::string>& parts) {
    for (const auto& p : parts) {
        if (p != "system" && p == name) {
            return true;
        }
    }
    return false;
}

// Record a leaf overlay op: target is the live stock dir to overlay, layer is a
// module's content for it. Multiple modules merge into one target. Skips targets
// that do not exist as a directory in the live filesystem.
static void add_leaf(std::map<std::string, std::vector<std::string>>& ops,
                     const std::string& target, const std::string& layer) {
    struct stat st {};
    if (lstat(target.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;
    }
    ops[target].push_back(layer);
}

// Emit depth-1 leaves under a partition root: every subdirectory of subroot
// becomes an overlay at <mount>/<child>. The partition root itself is NEVER
// overlaid — doing so shadows its bind sub-mounts (e.g. /system/vendor, or
// /vendor's qcrild tree) and breaks the device. Deeper sub-mounts under a leaf
// are re-established at mount time by mount_overlay's child handling.
static void plan_partition_root(std::map<std::string, std::vector<std::string>>& ops,
                                const std::string& subroot, const std::string& mount) {
    DIR* d = opendir(subroot.c_str());
    if (!d) {
        return;
    }
    std::error_code ec;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        const std::string layer = subroot + "/" + e->d_name;
        if (fs::is_directory(layer, ec)) {
            add_leaf(ops, mount + "/" + e->d_name, layer);
        }
    }
    closedir(d);
}

// Build the leaf overlay plan for all enabled modules. Module content lives at
// content_dir/<id>/<part>. Under system/, managed children (vendor/product/...)
// remap to /<part>; other system/ children and top-level partition trees become
// depth-1 leaves. Leaves are grouped by target so multiple modules stack.
static std::map<std::string, std::vector<std::string>>
plan_overlays(const std::string& content_dir, const std::vector<std::string>& enabled,
              const std::vector<std::string>& parts) {
    std::map<std::string, std::vector<std::string>> ops;
    std::error_code ec;
    for (const auto& id : enabled) {
        const std::string mbase = content_dir + "/" + id;

        const std::string sysroot = mbase + "/system";
        DIR* d = opendir(sysroot.c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                const std::string child = sysroot + "/" + e->d_name;
                if (!fs::is_directory(child, ec)) {
                    continue;
                }
                if (is_sub_partition(e->d_name, parts)) {
                    plan_partition_root(ops, child, std::string("/") + e->d_name);
                } else {
                    add_leaf(ops, std::string("/system/") + e->d_name, child);
                }
            }
            closedir(d);
        }

        for (const auto& p : parts) {
            if (p == "system") {
                continue;
            }
            const std::string top = mbase + "/" + p;
            if (fs::is_directory(top, ec)) {
                plan_partition_root(ops, top, fsutil::partition_mount_point(p));
            }
        }
    }
    return ops;
}

// Overlay mount points we own (fstype overlay, our source), shallowest-first.
static std::vector<std::string> our_overlays(const std::string& source) {
    std::set<std::string> found;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        std::istringstream pre(line.substr(0, sep));
        std::vector<std::string> f;
        std::string tok;
        while (pre >> tok) {
            f.push_back(tok);
        }
        if (f.size() < 5) {
            continue;
        }
        std::istringstream post(line.substr(sep + 3));
        std::string fstype;
        std::string src;
        post >> fstype >> src;
        if (fstype == "overlay" && src == source) {
            found.insert(f[4]);
        }
    }
    return {found.begin(), found.end()};
}

// Relabel a synced tree so each node carries the SELinux context of the stock
// file at its target path; files the module adds (no stock counterpart) inherit
// their directory's context. KSU labels /data/adb/modules files system_file, but
// content overlaid onto /vendor etc. must match the target's context or vendor/
// system domains get SELinux-denied (e.g. vendor_hal_perf reading /vendor/etc/perf).
static void relabel_tree(const std::string& node, const std::string& target,
                         const std::string& parent_ctx) {
    std::string ctx;
    if (!fsutil::get_context(target, ctx)) {
        ctx = parent_ctx; // module-added path: inherit the directory's context
    }
    if (!ctx.empty()) {
        fsutil::set_context(node, ctx);
    }

    struct stat st {};
    if (lstat(node.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;
    }
    DIR* d = opendir(node.c_str());
    if (!d) {
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        relabel_tree(node + "/" + e->d_name, target + "/" + e->d_name, ctx);
    }
    closedir(d);
}

// Relabel a module's synced partition tree against its live targets. The system/
// tree maps managed children (vendor/product/...) to /<part>, everything else to
// /system/<child>; other partition trees map straight to /<part>.
static void relabel_part(const std::string& dst, const std::string& part,
                         const std::vector<std::string>& parts) {
    if (part != "system") {
        relabel_tree(dst, fsutil::partition_mount_point(part), "");
        return;
    }
    DIR* d = opendir(dst.c_str());
    if (!d) {
        return;
    }
    std::error_code ec;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        const std::string child = dst + "/" + e->d_name;
        if (fs::is_directory(child, ec) && is_sub_partition(e->d_name, parts)) {
            relabel_tree(child, std::string("/") + e->d_name, "");
        } else {
            relabel_tree(child, std::string("/system/") + e->d_name, "");
        }
    }
    closedir(d);
}

// Populate content_dir/<id>/<part> with real copies of each module's partition
// trees, then relabel them against the live targets. A tmpfs base is volatile so
// it is copied every boot; an ext4 base persists, so we copy only what is missing;
// an erofs base is read-only and is built at install time.
static void sync_content(const std::vector<ModuleEntry>& modules, const storage::Handle& base,
                         const std::vector<std::string>& partitions) {
    if (base.mode == storage::Mode::Erofs) {
        return;
    }
    const bool volatile_base = base.mode == storage::Mode::Tmpfs;
    for (const auto& m : modules) {
        for (const auto& part : partitions) {
            const std::string src = (m.path / part).string();
            std::error_code ec;
            if (!fs::is_directory(src, ec) || fs::is_empty(src, ec)) {
                continue;
            }
            const std::string dst = base.content_dir + "/" + m.id + "/" + part;
            if (!volatile_base && fs::exists(dst, ec)) {
                continue;
            }
            fs::create_directories(dst, ec);
            fsutil::copy_tree(src, dst);
            relabel_part(dst, part, partitions);
        }
    }
}

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config) {
    storage::Handle base = storage::setup(config);
    if (!base.ok) {
        mlog("overlay: storage base setup failed");
        return false;
    }
    mlog(std::string("overlay: base mode=") + storage::mode_name(base.mode) + " content=" +
         base.content_dir);

    const std::vector<std::string>& partitions =
        config.partitions.empty() ? fsutil::kManagedPartitions : config.partitions;

    sync_content(modules, base, partitions);

    std::vector<std::string> enabled;
    enabled.reserve(modules.size());
    for (const auto& m : modules) {
        enabled.push_back(m.id);
    }

    // Overlay each module-touched leaf (a child of a partition root), never the
    // partition root itself. Shallower targets first so a parent leaf is mounted
    // before any nested leaf under it.
    const auto ops = plan_overlays(base.content_dir, enabled, partitions);
    int n = 0;
    for (const auto& [target, layers] : ops) {
        if (layers.empty()) {
            continue;
        }
        if (mount_overlay(target, layers, "", "", config.mount_source)) {
            ++n;
            if (config.kasumi_enabled && config.enable_overlay_xattr_hide &&
                ::kagami::kasumi::is_available() &&
                !::kagami::kasumi::hide_overlay_xattrs(target)) {
                mlog("overlay: failed to hide xattrs for " + target);
            }
        }
    }
    mlog("overlay: mounted " + std::to_string(n) + " leaf overlay(s)");
    return n > 0;
}

bool unmount_all(const Config& config) {
    const std::vector<std::string> overlays = our_overlays(config.mount_source);
    for (auto it = overlays.rbegin(); it != overlays.rend(); ++it) {
        umount2(it->c_str(), MNT_DETACH);
    }
    // The shared mirror is detached by backend::unmount_all() only after both
    // OverlayFS and Kasumi have released their references.
    return true;
}

bool restore_xattr_hiding(const Config& config) {
    if (!config.enable_overlay_xattr_hide) {
        return true;
    }
    if (!config.kasumi_enabled || !::kagami::kasumi::is_available()) {
        return false;
    }

    bool ok = true;
    for (const auto& target : our_overlays(config.mount_source)) {
        if (!::kagami::kasumi::hide_overlay_xattrs(target)) {
            mlog("overlay: failed to restore xattr hiding for " + target);
            ok = false;
        }
    }
    return ok;
}

bool is_active(const Config& config) { return !our_overlays(config.mount_source).empty(); }

} // namespace kagami::mount::overlay
