#include "mount/mount_fs.hpp"

#include "core/log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

// Older bionic headers may not expose every propagation flag.
#ifndef MS_SLAVE
#define MS_SLAVE (1 << 19)
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

namespace kagami::mount::fsutil {

constexpr const char* kSelinuxXattr = "security.selinux";

const std::vector<std::string> kManagedPartitions = {
    "system", "vendor", "product", "system_ext", "odm", "oem",
};

void mlog(const std::string& msg) {
    logging::append("mount", msg);
}

bool get_context(const std::string& path, std::string& out) {
    char buf[256];
    const ssize_t n = lgetxattr(path.c_str(), kSelinuxXattr, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    out.assign(buf);
    return true;
}

bool set_context(const std::string& path, const std::string& ctx) {
    return lsetxattr(path.c_str(), kSelinuxXattr, ctx.c_str(), ctx.size() + 1, 0) == 0;
}

void clone_attr(const std::string& src, const std::string& dst) {
    struct stat st;
    if (lstat(src.c_str(), &st) != 0) {
        return;
    }
    if (!S_ISLNK(st.st_mode)) {
        chmod(dst.c_str(), st.st_mode & 07777);
    }
    chown(dst.c_str(), st.st_uid, st.st_gid);
    std::string ctx;
    if (get_context(src, ctx)) {
        set_context(dst, ctx);
    }
}

bool bind_mount(const std::string& src, const std::string& dst) {
    return ::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr) == 0;
}

bool mirror_entry(const std::string& src, const std::string& dst) {
    struct stat st;
    if (lstat(src.c_str(), &st) != 0) {
        mlog("lstat " + src + " failed: " + std::strerror(errno));
        return false;
    }

    if (S_ISREG(st.st_mode)) {
        const int fd = open(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 07777);
        if (fd < 0) {
            mlog("create placeholder " + dst + " failed: " + std::strerror(errno));
            return false;
        }
        close(fd);
        if (!bind_mount(src, dst)) {
            mlog("bind " + src + " -> " + dst + " failed: " + std::strerror(errno));
            return false;
        }
        return true;
    }

    if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_MAX];
        const ssize_t len = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
        if (len < 0) {
            mlog("readlink " + src + " failed: " + std::strerror(errno));
            return false;
        }
        tgt[len] = '\0';
        if (symlink(tgt, dst.c_str()) != 0 && errno != EEXIST) {
            mlog("symlink " + dst + " failed: " + std::strerror(errno));
            return false;
        }
        std::string ctx;
        if (get_context(src, ctx)) {
            set_context(dst, ctx);
        }
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            mlog("mkdir " + dst + " failed: " + std::strerror(errno));
            return false;
        }
        clone_attr(src, dst);
        DIR* d = opendir(src.c_str());
        if (!d) {
            mlog("opendir " + src + " failed: " + std::strerror(errno));
            return false;
        }
        bool ok = true;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (!mirror_entry(src + "/" + e->d_name, dst + "/" + e->d_name)) {
                ok = false;
                break;
            }
        }
        closedir(d);
        return ok;
    }

    return true; // skip device/socket/fifo nodes
}

bool copy_tree(const std::string& src, const std::string& dst) {
    struct stat st;
    if (lstat(src.c_str(), &st) != 0) {
        mlog("copy_tree lstat " + src + " failed: " + std::strerror(errno));
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            mlog("copy_tree mkdir " + dst + " failed: " + std::strerror(errno));
            return false;
        }
        clone_attr(src, dst);
        DIR* d = opendir(src.c_str());
        if (!d) {
            return false;
        }
        bool ok = true;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (!copy_tree(src + "/" + e->d_name, dst + "/" + e->d_name)) {
                ok = false;
            }
        }
        closedir(d);
        return ok;
    }

    if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_MAX];
        const ssize_t len = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
        if (len < 0) {
            return false;
        }
        tgt[len] = '\0';
        unlink(dst.c_str());
        if (symlink(tgt, dst.c_str()) != 0 && errno != EEXIST) {
            return false;
        }
        std::string ctx;
        if (get_context(src, ctx)) {
            set_context(dst, ctx);
        }
        return true;
    }

    if (S_ISREG(st.st_mode)) {
        const int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            mlog("copy_tree open " + src + " failed: " + std::strerror(errno));
            return false;
        }
        const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
        if (out < 0) {
            close(in);
            mlog("copy_tree create " + dst + " failed: " + std::strerror(errno));
            return false;
        }
        char buf[65536];
        ssize_t n;
        bool ok = true;
        while ((n = read(in, buf, sizeof(buf))) > 0) {
            ssize_t off = 0;
            while (off < n) {
                const ssize_t w = write(out, buf + off, static_cast<size_t>(n - off));
                if (w < 0) {
                    ok = false;
                    break;
                }
                off += w;
            }
            if (!ok) {
                break;
            }
        }
        if (n < 0) {
            ok = false;
        }
        close(in);
        close(out);
        clone_attr(src, dst);
        return ok;
    }

    return true; // skip device/socket/fifo nodes
}

void rm_rf(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                continue;
            }
            const std::string child = path + "/" + e->d_name;
            struct stat st;
            if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                rm_rf(child);
            } else {
                unlink(child.c_str());
            }
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

bool tmpfs_xattr_supported() {
    if (getenv("KAGAMI_NO_XATTR") != nullptr) {
        return false;
    }
    const char* probe = "/mnt/.kagami_xattr_probe";
    const char* want = "u:object_r:system_file:s0";
    mkdir(probe, 0700);
    bool ok = false;
    if (::mount("tmpfs", probe, "tmpfs", 0, nullptr) == 0) {
        const std::string f = std::string(probe) + "/probe";
        const int fd = open(f.c_str(), O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) {
            close(fd);
            if (lsetxattr(f.c_str(), kSelinuxXattr, want, std::strlen(want) + 1, 0) == 0) {
                std::string back;
                if (get_context(f, back) && back == want) {
                    ok = true;
                }
            }
        }
        umount2(probe, MNT_DETACH);
    }
    rmdir(probe);
    return ok;
}

bool run_in_init_mount_ns(const std::function<bool()>& fn) {
    // Test hook: run in the *current* namespace (caller isolates via `unshare -m`)
    // so the engine can be exercised without touching the init/global namespace.
    // Detach all propagation first so test mounts can never escape this ns.
    if (getenv("KAGAMI_MOUNT_HERE") != nullptr) {
        ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
        try {
            return fn();
        } catch (...) {
            return false;
        }
    }
    const pid_t pid = fork();
    if (pid < 0) {
        mlog("fork failed: " + std::string(std::strerror(errno)));
        return false;
    }
    if (pid == 0) {
        const int fd = open("/proc/1/ns/mnt", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            mlog("open /proc/1/ns/mnt failed: " + std::string(std::strerror(errno)));
            _exit(2);
        }
        if (setns(fd, CLONE_NEWNS) != 0) {
            mlog("setns init mount ns failed: " + std::string(std::strerror(errno)));
            close(fd);
            _exit(3);
        }
        close(fd);
        bool ok = false;
        try {
            ok = fn();
        } catch (...) {
            ok = false;
        }
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string partition_mount_point(const std::string& partition) {
    if (partition == "system") {
        return "/system";
    }
    return "/" + partition;
}

std::string resolve_real_mount_target(const std::string& mount_point) {
    char buf[PATH_MAX];
    if (realpath(mount_point.c_str(), buf) == nullptr) {
        return "";
    }
    return std::string(buf);
}

} // namespace kagami::mount::fsutil
