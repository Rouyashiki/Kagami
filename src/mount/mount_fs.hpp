#pragma once

#include "core/arguments.hpp"

// Shared low-level mount primitives for Kagami backends.

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "core/log.hpp"
#include "core/runtime.hpp"

namespace kagami::mount::fsutil {

// SELinux label (security.selinux xattr), lstat-based (does not follow links).
bool get_context(const std::string& path, std::string& out);
bool set_context(const std::string& path, const std::string& ctx);

// Copy mode + owner + SELinux context from src to dst (preserves per-entry
// labels such as system_linker_exec). Best-effort.
void clone_attr(const std::string& src, const std::string& dst);

bool bind_mount(const std::string& src, const std::string& dst);

// Recursively materialize the src tree into dst: regular files become empty
// placeholders bind-mounted from src; symlinks are recreated; directories are
// recreated and recursed. SELinux contexts/attrs preserved. Used for opaque
// (.replace) module subtrees that must be fully reproduced.
bool mirror_entry(const std::string& src, const std::string& dst);

// Read native opaque metadata or the module .replace marker.
bool directory_is_opaque(const std::string& path, bool& opaque);

// Copy a module tree into an OverlayFS lower layer, translating .replace and
// whiteouts. Returns false on copy or replacement-metadata errors.
bool copy_tree(const std::string& src, const std::string& dst);

// A successful copy's preorder, with names owned independently of readdir.
// Entry zero is the root (empty name); every other parent precedes its child.
// Overlay can relabel after the entire copy without enumerating it again.
struct CopiedTree {
    struct Entry {
        std::size_t name_offset;
        std::size_t parent;
    };
    std::vector<Entry> entries;
    std::string names;
};
bool copy_tree(const std::string& src, const std::string& dst, CopiedTree& tree);

std::string decode_mount_path(const std::string& input);
struct MountRecord {
    std::string path;
    uint64_t mount_id = 0;
    uint64_t device = 0;
    uint64_t inode = 0;
};

bool capture_mount_identity(const std::string& path, MountRecord& record);
bool mount_matches(const MountRecord& record, bool include_init = false);
bool read_mount_journal(const std::string& journal, std::vector<MountRecord>& records,
                        bool& legacy);
bool write_mount_journal(const std::string& journal, const std::vector<std::string>& mounts);
bool register_umount(const std::string& path);
bool unregister_umount(const std::string& path);
bool prepare_empty_mountpoint(const std::string& path);

// Read CONFIG_TMPFS_XATTR from /proc/config.gz without creating probe mounts.
bool tmpfs_xattr_supported();

// Run fn inside PID 1's mount namespace (fork + setns(/proc/1/ns/mnt)).
// The caller's own namespace is never changed (safe to call from the daemon).
// Returns true iff the child ran fn and it returned true.
void mlog(const std::string& msg, logging::Level level);

template <typename Function>
[[nodiscard]] bool run_in_init_mount_ns(Function&& fn) {
    // Test hook: run in the *current* namespace (caller isolates via `unshare -m`)
    // so the engine can be exercised without touching the init/global namespace.
    // Detach all propagation first so test mounts can never escape this ns.
    if (runtime_mount_here()) {
        if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
            return false;
        return fn();
    }
    const pid_t pid = fork();
    if (pid < 0) {
        mlog("fork failed: " + std::string(std::strerror(errno)), logging::Level::Error);
        return false;
    }
    if (pid == 0) {
        const int fd = open("/proc/1/ns/mnt", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            mlog("open /proc/1/ns/mnt failed: " + std::string(std::strerror(errno)),
                 logging::Level::Error);
            _exit(2);
        }
        if (setns(fd, CLONE_NEWNS) != 0) {
            mlog("setns init mount ns failed: " + std::string(std::strerror(errno)),
                 logging::Level::Error);
            close(fd);
            _exit(3);
        }
        close(fd);
        const bool ok = fn();
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Append a line to the unified Kagami diagnostic log.
void mlog(const std::string& msg, logging::Level level = logging::Level::Info);

// "/system" for system, "/<p>" otherwise.
std::string partition_mount_point(std::string_view partition);

// Resolve a mount point that may be a symlink (e.g. /system/vendor -> /vendor)
// to its real backing directory. Returns "" if the path does not exist.
std::string resolve_real_mount_target(const std::string& mount_point);

// Partitions a metamodule manages, in mount order. system first.
Arguments managed_partitions() noexcept;

}  // namespace kagami::mount::fsutil
