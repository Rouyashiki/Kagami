#include "mount/magic_mount.hpp"

#include "core/runtime.hpp"
#include "mount/mount_fs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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

enum class NType { Regular, Directory, Symlink, Whiteout };

struct Node {
    std::string name;
    NType type = NType::Directory;
    std::map<std::string, Node> children;
    std::string module_path; // backing module file; "" if mirror-only / root
    bool replace = false;    // opaque dir (.replace / trusted.overlay.opaque)
    bool skip = false;
};

struct Walk {
    std::vector<std::string> committed; // mount points to record for teardown
    int files = 0;
    int tmpfs_dirs = 0;
    int symlinks = 0;
    int ignored = 0;
};

static std::string state_file() {
    return (runtime_data_dir() / "run" / "magic_mounts.list").string();
}

// Report a committed mount to KernelSU for per-app unmount. Skips lib dirs:
// pairip-protected APKs verify native libs after zygote fork, and detaching the
// overlay mid-flight crashes them.
static void ksu_umount_add(const std::string& path) {
    static const char* const kIgnore[] = {"/system/lib", "/system/lib64", "/vendor/lib",
                                          "/vendor/lib64"};
    for (const char* ig : kIgnore) {
        const std::string p(ig);
        if (path == p || path.rfind(p + "/", 0) == 0) {
            return;
        }
    }
    const pid_t pid = fork();
    if (pid == 0) {
        execl("/data/adb/ksud", "ksud", "kernel", "umount", "add", path.c_str(),
              static_cast<char*>(nullptr));
        execl("/data/adb/ksu/bin/ksud", "ksud", "kernel", "umount", "add", path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

static bool lexists(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}

static std::string join(const std::string& base, const std::string& name) {
    return base == "/" ? "/" + name : base + "/" + name;
}

static NType type_from_lstat(const struct stat& st) {
    if (S_ISCHR(st.st_mode) && st.st_rdev == 0) {
        return NType::Whiteout; // 0:0 char device = whiteout marker
    }
    if (S_ISDIR(st.st_mode)) {
        return NType::Directory;
    }
    if (S_ISLNK(st.st_mode)) {
        return NType::Symlink;
    }
    if (S_ISREG(st.st_mode)) {
        return NType::Regular;
    }
    return NType::Whiteout;
}

static bool dir_is_replace(const std::string& path) {
    char buf[8] = {};
    const ssize_t n = lgetxattr(path.c_str(), "trusted.overlay.opaque", buf, sizeof(buf) - 1);
    if (n > 0 && buf[0] == 'y') {
        return true;
    }
    return lexists(path + "/.replace");
}

static bool clone_symlink(const std::string& src, const std::string& dst) {
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

// Collect one module's system/ subtree into `parent` (first module wins a file
// conflict; directories merge). Returns whether any file was added.
static bool collect_into(Node& parent, const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return false;
    }
    bool has = false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        const std::string name = e->d_name;
        const std::string child_path = dir + "/" + name;
        struct stat st;
        if (lstat(child_path.c_str(), &st) != 0) {
            continue;
        }
        const NType t = type_from_lstat(st);

        Node* node;
        auto it = parent.children.find(name);
        if (it != parent.children.end()) {
            node = &it->second;
        } else {
            Node n;
            n.name = name;
            n.type = t;
            n.module_path = child_path;
            if (t == NType::Directory) {
                n.replace = dir_is_replace(child_path);
            }
            node = &parent.children.emplace(name, std::move(n)).first->second;
        }

        if (node->type == NType::Directory) {
            has |= collect_into(*node, child_path) || node->replace;
        } else {
            has = true;
        }
    }
    closedir(d);
    return has;
}

// Build the merged root tree: module system/ trees, with vendor/product/... moved
// out to root level when the device exposes them as /system/<p> symlinks.
static std::optional<Node> collect_module_files(const std::vector<ModuleEntry>& modules,
                                                const std::vector<std::string>& extra) {
    Node root;
    root.type = NType::Directory;
    Node system;
    system.name = "system";
    system.type = NType::Directory;

    bool has = false;
    for (const auto& m : modules) {
        const std::string ms = m.path.string() + "/system";
        struct stat st;
        if (lstat(ms.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        has |= collect_into(system, ms);
    }
    if (!has) {
        return std::nullopt;
    }

    const auto move_partition = [&](const std::string& part, bool require_symlink) {
        std::error_code ec;
        const bool ok = fs::is_directory("/" + part, ec) &&
                        (!require_symlink || fs::is_symlink("/system/" + part, ec));
        if (!ok) {
            return;
        }
        auto it = system.children.find(part);
        if (it != system.children.end()) {
            Node moved = std::move(it->second);
            system.children.erase(it);
            // Partition roots carry no module backing, so do_directory() skips any
            // child that would force skeletonizing the root: the roots are never
            // replaced wholesale.
            moved.module_path.clear();
            moved.replace = false;
            root.children.emplace(part, std::move(moved));
        }
    };

    const std::pair<const char*, bool> builtin[] = {
        {"vendor", true}, {"system_ext", true}, {"product", true}, {"odm", false}};
    for (const auto& [part, req] : builtin) {
        move_partition(part, req);
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
            move_partition(part, false);
        }
    }

    root.children.emplace("system", std::move(system));
    return root;
}

// Create the tmpfs skeleton dir at `work`, cloning mode/owner/SELinux context
// from the real dir (or the module dir when the real one does not exist).
static bool tmpfs_skeleton(const std::string& real, const std::string& work, const Node& node) {
    std::error_code ec;
    fs::create_directories(work, ec);
    if (ec) {
        mlog("skeleton mkdir " + work + " failed: " + ec.message());
        return false;
    }
    const std::string src = lexists(real) ? real : node.module_path;
    if (src.empty()) {
        return false;
    }
    fsutil::clone_attr(src, work);
    return true;
}

// Mirror one unmodified real entry into the skeleton: dirs recursed entry by
// entry, files bind-mounted, symlinks recreated.
static bool mount_mirror(const std::string& real, const std::string& work, const std::string& name) {
    const std::string r = join(real, name);
    const std::string w = work + "/" + name;
    struct stat st;
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
            mlog("mirror opendir " + r + " failed: " + std::strerror(errno));
            return false;
        }
        bool ok = true;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
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
            mlog("mirror bind " + r + " failed: " + std::strerror(errno));
            return false;
        }
        return true;
    }
    return true; // skip device/socket/fifo
}

static bool do_mount(Node& node, const std::string& real, const std::string& work, bool has_tmpfs,
                     Walk& w);

static bool do_directory(Node& node, const std::string& real, const std::string& work,
                         bool has_tmpfs, Walk& w) {
    bool tmpfs = !has_tmpfs && node.replace && !node.module_path.empty();

    if (!has_tmpfs && !tmpfs) {
        for (auto& [name, child] : node.children) {
            const std::string real_child = join(real, name);
            bool need;
            if (child.type == NType::Symlink) {
                need = true;
            } else if (child.type == NType::Whiteout) {
                need = lexists(real_child);
            } else {
                // A dir needs a skeleton when a child's real counterpart is not a
                // directory (a file/symlink) or is missing.
                struct stat st;
                if (lstat(real_child.c_str(), &st) == 0) {
                    need = !S_ISDIR(st.st_mode);
                } else {
                    need = true;
                }
            }
            if (need) {
                if (std::getenv("KAGAMI_DEBUG") != nullptr) {
                    mlog("  need-skeleton at " + real + " due to child '" + name + "' (real=" +
                         real_child + ")");
                }
                if (node.module_path.empty()) {
                    // No module backs this dir; skeletonizing it would drop the real
                    // partition, so skip the child instead.
                    child.skip = true;
                    ++w.ignored;
                    mlog("cannot skeletonize " + real + "; ignoring child " + name);
                    continue;
                }
                tmpfs = true;
                break;
            }
        }
    }

    const bool now_tmpfs = tmpfs || has_tmpfs;
    if (now_tmpfs && !tmpfs_skeleton(real, work, node)) {
        return false;
    }
    if (tmpfs && !fsutil::bind_mount(work, work)) { // make the skeleton movable
        mlog("self-bind " + work + " failed: " + std::strerror(errno));
        return false;
    }

    // Module-overridden entries recurse; the rest are mirrored into the skeleton.
    if (lexists(real) && !node.replace) {
        DIR* d = opendir(real.c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                const std::string name = e->d_name;
                auto it = node.children.find(name);
                if (it != node.children.end()) {
                    Node child = std::move(it->second);
                    node.children.erase(it);
                    if (child.skip) {
                        continue;
                    }
                    if (!do_mount(child, join(real, name), work + "/" + name, now_tmpfs, w) &&
                        now_tmpfs) {
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
            closedir(d);
        }
    }

    // Remaining children are new entries that do not exist in the real dir.
    for (auto& [name, child] : node.children) {
        if (child.skip) {
            continue;
        }
        if (!do_mount(child, join(real, name), work + "/" + name, now_tmpfs, w) && now_tmpfs) {
            return false;
        }
    }

    if (tmpfs) {
        ::mount(nullptr, work.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr);
        if (::mount(work.c_str(), real.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
            mlog("move " + work + " -> " + real + " failed: " + std::strerror(errno));
            return false;
        }
        ::mount("none", real.c_str(), nullptr, MS_PRIVATE | MS_REC, nullptr);
        w.committed.push_back(real);
        ++w.tmpfs_dirs;
        mlog("magic: skeletoned " + real);
    }
    return true;
}

static bool do_mount(Node& node, const std::string& real, const std::string& work, bool has_tmpfs,
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
        if (node.module_path.empty()) {
            mlog("file node without module backing: " + real);
            return false;
        }
        if (!has_tmpfs) {
            // The need-check guarantees a file child forces a parent skeleton, so a
            // regular file is always inside a tmpfs; bail if that invariant breaks.
            mlog("refusing to place file outside tmpfs skeleton: " + real);
            return false;
        }
        // Bind the module file onto a placeholder in the skeleton, then remount it
        // read-only. Binding keeps the file's own SELinux label (module files are
        // labelled system_file); copying would need a relabel the metamount domain
        // may not be allowed to perform.
        {
            const int fd = open(work.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0) {
                close(fd);
            }
        }
        if (!fsutil::bind_mount(node.module_path, work)) {
            mlog("bind module file " + node.module_path + " -> " + work +
                 " failed: " + std::strerror(errno));
            return false;
        }
        ::mount(work.c_str(), work.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr);
        ++w.files;
        return true;
    }
    case NType::Whiteout:
        return true; // absence in the skeleton hides the original
    case NType::Directory:
        return do_directory(node, real, work, has_tmpfs, w);
    }
    return false;
}

// Decode mountinfo octal escapes (\040 space, \011 tab, \012 nl, \134 backslash).
static std::string unescape_mountinfo(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit((unsigned char)s[i + 1]) &&
            std::isdigit((unsigned char)s[i + 2]) && std::isdigit((unsigned char)s[i + 3])) {
            out.push_back(static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 +
                                            (s[i + 3] - '0')));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Mount points whose mount source equals `source`. Our skeletons and work tmpfs
// use config.mount_source; real partitions are block-backed, so teardown can match
// ours without ever touching a real partition.
static std::set<std::string> mounts_with_source(const std::string& source) {
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
        std::string fstype, src;
        post >> fstype >> src;
        if (src == source) {
            out.insert(unescape_mountinfo(f[4]));
        }
    }
    return out;
}

bool unmount_all(const Config& config) {
    // Detach only paths that are BOTH recorded by us AND still backed by a mount
    // whose source is ours. Real partitions are block-backed, so a stale state file
    // listing "/product"/"/vendor" can never make us detach a real partition.
    std::vector<std::string> recorded;
    {
        std::ifstream in(state_file());
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                recorded.push_back(line);
            }
        }
    }
    const std::set<std::string> ours = mounts_with_source(config.mount_source);
    // Deepest first so nested skeletons detach before their parents.
    std::sort(recorded.begin(), recorded.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& p : recorded) {
        if (ours.count(p) == 0) {
            mlog("unmount: refusing to detach " + p + " (not a Kagami '" + config.mount_source +
                 "' mount)");
            continue;
        }
        for (int i = 0; i < 8; ++i) {
            if (umount2(p.c_str(), MNT_DETACH) != 0) {
                break;
            }
        }
    }
    if (ours.count(config.work_dir) != 0) {
        umount2(config.work_dir.c_str(), MNT_DETACH);
    }
    fsutil::rm_rf(config.work_dir);
    std::error_code ec;
    fs::remove(state_file(), ec);
    return true;
}

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config) {
    unmount_all(config); // idempotent: clear any previous Kagami magic mounts

    std::error_code ec;
    fs::create_directories(runtime_data_dir() / "run", ec);

    auto root = collect_module_files(modules, config.partitions);
    if (!root) {
        mlog("magic: no module files to mount");
        { std::ofstream s(state_file(), std::ios::trunc); }
        return true;
    }

    const std::string work = config.work_dir;
    fs::create_directories(work, ec);
    if (::mount(config.mount_source.c_str(), work.c_str(), "tmpfs", 0, nullptr) != 0) {
        mlog("magic: work tmpfs failed: " + std::string(std::strerror(errno)));
        return false;
    }
    ::mount("none", work.c_str(), nullptr, MS_PRIVATE | MS_REC, nullptr);

    Walk w;
    const bool ok = do_mount(*root, "/", work, false, w);

    {
        std::ofstream state(state_file(), std::ios::trunc);
        for (const auto& m : w.committed) {
            state << m << "\n";
        }
    }
    for (const auto& m : w.committed) {
        ksu_umount_add(m);
    }
    mlog("magic: files=" + std::to_string(w.files) + " skeletons=" + std::to_string(w.tmpfs_dirs) +
         " symlinks=" + std::to_string(w.symlinks) + " ignored=" + std::to_string(w.ignored));
    return ok;
}

bool is_active(const Config& config) {
    (void)config;
    std::ifstream in(state_file());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            return true;
        }
    }
    return false;
}

bool normalize_module(const std::string& module_path) {
    std::error_code ec;
    struct stat st;
    if (lstat(module_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    const char* parts[] = {"vendor", "system_ext", "product", "odm", "oem"};
    for (const auto* part : parts) {
        if (!(fs::is_directory(std::string("/") + part, ec) &&
              fs::is_symlink(std::string("/system/") + part, ec))) {
            continue;
        }
        const std::string top = module_path + "/" + part;
        const std::string sys = module_path + "/system/" + part;
        if (lexists(top) && fs::is_directory(top, ec)) {
            fs::create_directories(module_path + "/system", ec);
            if (!lexists(sys)) {
                fs::rename(top, sys, ec);
                if (ec) {
                    mlog("normalize: rename " + top + " -> " + sys + " failed: " + ec.message());
                    continue;
                }
            }
        }
        if (lexists(sys) && !fs::is_symlink(top, ec)) {
            fs::remove_all(top, ec);
            if (symlink((std::string("./system/") + part).c_str(), top.c_str()) != 0 &&
                errno != EEXIST) {
                mlog("normalize: symlink " + top + " failed: " + std::strerror(errno));
            }
        }
    }
    return true;
}

} // namespace kagami::mount::magic
