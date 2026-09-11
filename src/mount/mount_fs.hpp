#pragma once

// Shared low-level mount primitives for Kagami backends.

#include "core/log.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kagami::mount::fsutil {

// SELinux label (security.selinux xattr), lstat-based (does not follow links).
bool get_context(const std::string &path, std::string &out);
bool set_context(const std::string &path, const std::string &ctx);

// Copy mode + owner + SELinux context from src to dst (preserves per-entry
// labels such as system_linker_exec). Best-effort.
void clone_attr(const std::string &src, const std::string &dst);

bool bind_mount(const std::string &src, const std::string &dst);

// Recursively materialize the src tree into dst: regular files become empty
// placeholders bind-mounted from src; symlinks are recreated; directories are
// recreated and recursed. SELinux contexts/attrs preserved. Used for opaque
// (.replace) module subtrees that must be fully reproduced.
bool mirror_entry(const std::string &src, const std::string &dst);

// Read native opaque metadata or the module .replace marker.
bool directory_is_opaque(const std::string &path, bool &opaque);

// Copy a module tree into an OverlayFS lower layer, translating .replace and
// whiteouts. Returns false on copy or replacement-metadata errors.
bool copy_tree(const std::string &src, const std::string &dst);

std::string decode_mount_path(const std::string &input);
struct MountRecord {
    std::string path;
    uint64_t mount_id = 0;
    uint64_t device = 0;
    uint64_t inode = 0;
};

bool capture_mount_identity(const std::string &path, MountRecord &record);
bool mount_matches(const MountRecord &record, bool include_init = false);
bool read_mount_journal(const std::string &journal, std::vector<MountRecord> &records,
                        bool &legacy);
bool write_mount_journal(const std::string &journal, const std::vector<std::string> &paths);
bool register_umount(const std::string &path);
bool unregister_umount(const std::string &path);
bool prepare_empty_mountpoint(const std::string &path);

// Read CONFIG_TMPFS_XATTR from /proc/config.gz without creating probe mounts.
bool tmpfs_xattr_supported();

// Run fn inside PID 1's mount namespace (fork + setns(/proc/1/ns/mnt)).
// The caller's own namespace is never changed (safe to call from the daemon).
// Returns true iff the child ran fn and it returned true.
bool run_in_init_mount_ns(const std::function<bool()> &fn);

// Append a line to the unified Kagami diagnostic log.
void mlog(const std::string &msg, logging::Level level = logging::Level::Info);

// "/system" for system, "/<p>" otherwise.
std::string partition_mount_point(const std::string &partition);

// Resolve a mount point that may be a symlink (e.g. /system/vendor -> /vendor)
// to its real backing directory. Returns "" if the path does not exist.
std::string resolve_real_mount_target(const std::string &mount_point);

// Partitions a metamodule manages, in mount order. system first.
extern const std::vector<std::string> kManagedPartitions;

} // namespace kagami::mount::fsutil
