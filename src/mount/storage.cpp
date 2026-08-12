#include "mount/storage.hpp"

#include "mount/mount_fs.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

namespace kagami::mount::storage {

namespace fs = std::filesystem;
using fsutil::mlog;

const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Tmpfs:
        return "tmpfs";
    case Mode::Ext4:
        return "ext4";
    case Mode::Erofs:
        return "erofs";
    }
    return "?";
}

// fork/exec a tool (looked up on PATH); true on exit status 0.
static bool run_tool(const std::vector<std::string>& argv) {
    std::vector<char*> c;
    c.reserve(argv.size() + 1);
    for (const auto& a : argv) {
        c.push_back(const_cast<char*>(a.c_str()));
    }
    c.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        execvp(c[0], c.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Probe whether a mounted fs preserves the trusted.overlay.* xattrs overlayfs
// needs for opaque dirs and whiteouts (security.selinux alone is not enough).
static bool overlay_xattr_ok(const std::string& dir) {
    const std::string probe = dir + "/.kagami_xattr_probe";
    if (mkdir(probe.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
    }
    const bool ok = lsetxattr(probe.c_str(), "trusted.overlay.opaque", "y", 1, 0) == 0;
    rmdir(probe.c_str());
    return ok;
}

static bool make_ext4_image(const std::string& img, int size_mb) {
    const std::string size = std::to_string(size_mb) + "M";
    if (!run_tool({"truncate", "-s", size, img}) &&
        !run_tool({"fallocate", "-l", size, img})) {
        mlog("storage: failed to allocate image " + img);
        return false;
    }
    if (!run_tool({"mke2fs", "-t", "ext4", "-O", "^has_journal", "-F", img}) &&
        !run_tool({"mkfs.ext4", "-O", "^has_journal", "-F", img})) {
        mlog("storage: mke2fs failed for " + img);
        return false;
    }
    return true;
}

// Make a mount private and register it with KernelSU for per-app unmount.
static void finalize(const std::string& dir) {
    ::mount("none", dir.c_str(), nullptr, MS_PRIVATE, nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        execl("/data/adb/ksud", "ksud", "kernel", "umount", "add", dir.c_str(),
              static_cast<char*>(nullptr));
        execl("/data/adb/ksu/bin/ksud", "ksud", "kernel", "umount", "add", dir.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

// Return the filesystem type when `path` is an exact mountpoint in this mount
// namespace. A later backend must reuse the mirror acquired by the first one;
// blindly detaching it would invalidate the other backend's lower trees.
static bool mounted_mode(const std::string& path, Mode& mode) {
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        std::istringstream pre(line.substr(0, sep));
        std::vector<std::string> fields;
        std::string field;
        while (pre >> field) {
            fields.push_back(field);
        }
        if (fields.size() < 5 || fields[4] != path) {
            continue;
        }
        std::istringstream post(line.substr(sep + 3));
        std::string fstype;
        post >> fstype;
        if (fstype == "tmpfs") {
            mode = Mode::Tmpfs;
            return true;
        }
        if (fstype == "ext4") {
            mode = Mode::Ext4;
            return true;
        }
        if (fstype == "erofs") {
            mode = Mode::Erofs;
            return true;
        }
        return false;
    }
    return false;
}

Handle setup(const Config& config) {
    Handle h;
    const std::string base = config.mirror_dir; // shared mount point, off /data
    // The mirror root is the mountpoint itself: module trees live directly at
    // /dev/kagami_mirror/<module-id>, matching hymo's /dev/hymo_mirror layout.
    h.content_dir = base;
    const std::string img = config.mirror_img; // backing image persists on /data
    std::string erofs_img = config.mirror_img;
    const auto dot = erofs_img.rfind(".img");
    if (dot != std::string::npos) {
        erofs_img.replace(dot, 4, ".erofs");
    } else {
        erofs_img += ".erofs";
    }

    std::error_code ec;
    fs::create_directories(h.content_dir, ec);

    const std::string& mode = config.fs_type;
    const bool want_auto = mode == "auto" || mode.empty();
    const bool writable = config.overlay_writable; // upper/work layer is opt-in

    if (mounted_mode(h.content_dir, h.mode)) {
        if (writable) {
            h.rw_dir = h.mode == Mode::Erofs ? base + ".rw" : h.content_dir + "/.rw";
            fs::create_directories(h.rw_dir, ec);
            if (ec) {
                mlog("storage: failed to create shared writable layer: " + ec.message());
                return Handle{};
            }
            if (h.mode == Mode::Erofs) {
                Mode rw_mode;
                if (!mounted_mode(h.rw_dir, rw_mode)) {
                    if (::mount(config.mount_source.c_str(), h.rw_dir.c_str(), "tmpfs", 0, nullptr) != 0) {
                        mlog("storage: shared erofs writable tmpfs failed: " +
                             std::string(std::strerror(errno)));
                        return Handle{};
                    }
                    finalize(h.rw_dir);
                }
            }
        }
        h.ok = true;
        mlog(std::string("storage: reusing shared ") + mode_name(h.mode) + " mirror");
        return h;
    }
    umount2(h.content_dir.c_str(), MNT_DETACH); // clear an unrecognised stale mount

    // erofs: read-only content image, plus a separate tmpfs writable layer if opted in.
    if (mode == "erofs") {
        if (!fs::exists(erofs_img)) {
            mlog("storage: erofs image missing (" + erofs_img + "); build it at install time");
            return h;
        }
        if (!run_tool({"mount", "-t", "erofs", "-o", "loop,ro", erofs_img, h.content_dir})) {
            mlog("storage: mount erofs image failed");
            return h;
        }
        if (writable) {
            // EROFS mounts the root read-only, so its optional OverlayFS
            // upper/work tmpfs must be a sibling rather than a child.
            h.rw_dir = base + ".rw";
            fs::create_directories(h.rw_dir, ec);
            umount2(h.rw_dir.c_str(), MNT_DETACH);
            if (::mount(config.mount_source.c_str(), h.rw_dir.c_str(), "tmpfs", 0, nullptr) != 0) {
                mlog("storage: erofs writable tmpfs failed: " + std::string(std::strerror(errno)));
                umount2(h.content_dir.c_str(), MNT_DETACH);
                return h;
            }
            finalize(h.rw_dir);
        }
        finalize(h.content_dir);
        h.mode = Mode::Erofs;
        h.ok = true;
        mlog(std::string("storage: erofs content") + (writable ? " + tmpfs writable layer" : " (read-only)"));
        return h;
    }

    // auto/tmpfs: try tmpfs; in auto mode fall back to ext4 if overlay xattrs are
    // unsupported. The writable layer lives inside the same tmpfs.
    if (want_auto || mode == "tmpfs") {
        if (::mount(config.mount_source.c_str(), h.content_dir.c_str(), "tmpfs", 0, nullptr) == 0) {
            if (mode == "tmpfs" || overlay_xattr_ok(h.content_dir)) {
                if (writable) {
                    h.rw_dir = h.content_dir + "/.rw";
                    fs::create_directories(h.rw_dir, ec);
                }
                finalize(h.content_dir);
                h.mode = Mode::Tmpfs;
                h.ok = true;
                mlog(std::string("storage: tmpfs base") + (writable ? " (writable)" : " (read-only)"));
                return h;
            }
            mlog("storage: tmpfs lacks overlay xattr support; falling back to ext4");
            umount2(h.content_dir.c_str(), MNT_DETACH);
        } else if (mode == "tmpfs") {
            mlog("storage: tmpfs mount failed: " + std::string(std::strerror(errno)));
            return h;
        }
    }

    // ext4 loop image (forced, or the auto fallback). Writable layer lives inside.
    if (!fs::exists(img) && !make_ext4_image(img, config.mirror_img_size_mb)) {
        return h;
    }
    run_tool({"chcon", "u:object_r:ksu_file:s0", img}); // best-effort label on the image
    if (!run_tool({"mount", "-t", "ext4", "-o", "loop,rw,noatime", img, h.content_dir})) {
        mlog("storage: mount ext4 image failed");
        return h;
    }
    if (writable) {
        h.rw_dir = h.content_dir + "/.rw";
        fs::create_directories(h.rw_dir, ec);
    }
    finalize(h.content_dir);
    h.mode = Mode::Ext4;
    h.ok = true;
    mlog(std::string("storage: ext4 image base") + (writable ? " (writable)" : " (read-only)"));
    return h;
}

void teardown(const Handle& handle) {
    if (!handle.ok) {
        return;
    }
    if (handle.mode == Mode::Erofs && !handle.rw_dir.empty()) {
        umount2(handle.rw_dir.c_str(), MNT_DETACH);
    }
    umount2(handle.content_dir.c_str(), MNT_DETACH);
}

void teardown_shared(const Config& config) {
    const std::string base = config.mirror_dir;
    // rw is only separately mounted in EROFS mode; detaching it is harmless in
    // tmpfs/ext4 mode, where the writable layer lives inside the mirror root.
    umount2((base + ".rw").c_str(), MNT_DETACH);
    umount2(base.c_str(), MNT_DETACH);
}

} // namespace kagami::mount::storage
