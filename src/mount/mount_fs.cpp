#include "mount/mount_fs.hpp"
#include <linux/stat.h>
#include <sys/syscall.h>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "core/log.hpp"
#include "core/runtime.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <zlib.h>
#include <climits>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>

// Older bionic headers may not expose every propagation flag.
#ifndef MS_SLAVE
#define MS_SLAVE (1 << 19)
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

namespace kagami::mount::fsutil {

constexpr const char* kSelinuxXattr = "security.selinux";

Arguments managed_partitions() noexcept {
    static constexpr Argument partitions[] = {
        "system", "vendor", "product", "system_ext", "odm", "oem",
    };
    return partitions;
}

void mlog(const std::string& msg, logging::Level level) {
    logging::write(level, "mount", msg);
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

namespace {
void clone_known_attr(const std::string& src, const std::string& dst, const struct stat& st) {
    if (!S_ISLNK(st.st_mode)) {
        chmod(dst.c_str(), st.st_mode & 07777);
    }
    chown(dst.c_str(), st.st_uid, st.st_gid);
    std::string ctx;
    if (get_context(src, ctx)) {
        set_context(dst, ctx);
    }
}
}  // namespace

void clone_attr(const std::string& src, const std::string& dst) {
    struct stat st{};
    if (lstat(src.c_str(), &st) == 0)
        clone_known_attr(src, dst, st);
}

bool bind_mount(const std::string& src, const std::string& dst) {
    return ::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr) == 0;
}

bool mirror_entry(const std::string& src, const std::string& dst) {
    struct stat st{};
    if (lstat(src.c_str(), &st) != 0) {
        mlog("lstat " + src + " failed: " + std::strerror(errno), logging::Level::Error);
        return false;
    }

    if (S_ISREG(st.st_mode)) {
        const int fd = open(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 07777);
        if (fd < 0) {
            mlog("create placeholder " + dst + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        close(fd);
        if (!bind_mount(src, dst)) {
            mlog("bind " + src + " -> " + dst + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        return true;
    }

    if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_MAX];
        const ssize_t len = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
        if (len < 0) {
            mlog("readlink " + src + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        tgt[len] = '\0';
        if (symlink(tgt, dst.c_str()) != 0 && errno != EEXIST) {
            mlog("symlink " + dst + " failed: " + std::strerror(errno), logging::Level::Error);
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
            mlog("mkdir " + dst + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        clone_known_attr(src, dst, st);
        DIR* d = opendir(src.c_str());
        if (!d) {
            mlog("opendir " + src + " failed: " + std::strerror(errno), logging::Level::Error);
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

    return true;  // skip device/socket/fifo nodes
}

bool directory_is_opaque(const std::string& path, bool& opaque) {
    opaque = false;
    struct stat marker{};
    if (lstat((path + "/.replace").c_str(), &marker) == 0) {
        opaque = true;
        return true;
    }
    if (errno != ENOENT)
        return false;
    char value[8]{};
    const ssize_t count = lgetxattr(path.c_str(), "trusted.overlay.opaque", value, sizeof(value));
    if (count < 0) {
        if (errno == ENODATA || errno == EOPNOTSUPP)
            return true;
        mlog("read opaque attribute " + path + ": " + std::strerror(errno), logging::Level::Error);
        return false;
    }
    opaque = count == 1 && value[0] == 'y';
    return true;
}

namespace {
std::size_t record_copy(CopiedTree* tree, const std::string& name, std::size_t parent) {
    if (!tree)
        return 0;
    const std::size_t index = tree->entries.size();
    tree->entries.push_back({tree->names.size(), parent});
    tree->names.append(name);
    tree->names.push_back('\0');
    return index;
}

bool copy_tree_impl(const std::string& src, const std::string& dst, CopiedTree* tree,
                    const std::string& name, std::size_t parent) {
    struct stat st{};
    if (lstat(src.c_str(), &st) != 0) {
        mlog("copy_tree lstat " + src + " failed: " + std::strerror(errno), logging::Level::Error);
        return false;
    }

    bool whiteout = S_ISCHR(st.st_mode) && st.st_rdev == makedev(0, 0);
    if (S_ISREG(st.st_mode) && st.st_size == 0) {
        const ssize_t count = lgetxattr(src.c_str(), "trusted.overlay.whiteout", nullptr, 0);
        if (count >= 0)
            whiteout = true;
        else if (errno != ENODATA && errno != EOPNOTSUPP) {
            mlog("read whiteout attribute " + src + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
    }
    if (whiteout) {
        if (mknod(dst.c_str(), S_IFCHR | (st.st_mode & 07777), makedev(0, 0)) != 0) {
            mlog("create whiteout " + dst + ": " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        clone_known_attr(src, dst, st);
        record_copy(tree, name, parent);
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            mlog("copy_tree mkdir " + dst + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        clone_known_attr(src, dst, st);
        bool opaque = false;
        if (!directory_is_opaque(src, opaque))
            return false;
        if (opaque && lsetxattr(dst.c_str(), "trusted.overlay.opaque", "y", 1, 0) != 0) {
            mlog("set opaque attribute " + dst + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        std::unique_ptr<DIR, decltype(&closedir)> d(opendir(src.c_str()), closedir);
        if (!d) {
            return false;
        }
        const std::size_t index = record_copy(tree, name, parent);
        bool ok = true;
        struct dirent* e;
        for (;;) {
            errno = 0;
            e = readdir(d.get());
            if (!e) {
                ok = ok && errno == 0;
                break;
            }
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (std::strcmp(e->d_name, ".replace") == 0)
                continue;
            if (!copy_tree_impl(src + "/" + e->d_name, dst + "/" + e->d_name, tree, e->d_name,
                                index)) {
                ok = false;
            }
        }
        if (closedir(d.release()) != 0)
            ok = false;
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
        record_copy(tree, name, parent);
        return true;
    }

    if (S_ISREG(st.st_mode)) {
        const int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            mlog("copy_tree open " + src + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        const int out =
            open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
        if (out < 0) {
            close(in);
            mlog("copy_tree create " + dst + " failed: " + std::strerror(errno),
                 logging::Level::Error);
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
        clone_known_attr(src, dst, st);
        if (ok)
            record_copy(tree, name, parent);
        return ok;
    }

    return true;  // skip device/socket/fifo nodes
}
}  // namespace

bool copy_tree(const std::string& src, const std::string& dst) {
    return copy_tree_impl(src, dst, nullptr, {}, 0);
}

bool copy_tree(const std::string& src, const std::string& dst, CopiedTree& tree) {
    tree.entries.clear();
    tree.names.clear();
    if (copy_tree_impl(src, dst, &tree, {}, 0))
        return true;
    tree.entries.clear();
    tree.names.clear();
    return false;
}

bool tmpfs_xattr_supported() {
    // A complete kernel config is immutable for this process. Failed probes are
    // deliberately not cached, since /proc/config.gz may become readable later.
    static std::atomic<int> cached{-1};
    const int previous = cached.load(std::memory_order_relaxed);
    if (previous >= 0)
        return previous != 0;
    gzFile file = gzopen("/proc/config.gz", "rbe");
    if (!file) {
        mlog("storage: cannot read /proc/config.gz: " + std::string(std::strerror(errno)) +
                 "; defaulting to ext4",
             logging::Level::Warning);
        return false;
    }
    constexpr char enabled_line[] = "CONFIG_TMPFS_XATTR=y";
    std::array<char, sizeof(enabled_line)> line{};
    std::size_t line_size = 0;
    bool line_overflow = false;
    bool enabled = false;
    const auto finish_line = [&]() {
        if (!line_overflow && line_size != 0 && line[line_size - 1] == '\r')
            --line_size;
        if (!line_overflow && line_size == sizeof(enabled_line) - 1 &&
            std::memcmp(line.data(), enabled_line, line_size) == 0)
            enabled = true;
        line_size = 0;
        line_overflow = false;
    };
    std::size_t total = 0;
    char buffer[8192];
    int count;
    while ((count = gzread(file, buffer, sizeof(buffer))) > 0) {
        total += static_cast<std::size_t>(count);
        if (total > 4UL * 1024 * 1024)
            break;
        for (int i = 0; i < count; ++i) {
            if (buffer[i] == '\n')
                finish_line();
            else if (line_size < line.size())
                line[line_size++] = buffer[i];
            else
                line_overflow = true;
        }
    }
    int error;
    (void)gzerror(file, &error);
    const bool valid = count == 0 && error == Z_OK && gzeof(file) && !gzdirect(file);
    const int closed = gzclose(file);
    if (!valid || closed != Z_OK) {
        mlog("storage: invalid or incomplete /proc/config.gz; defaulting to ext4",
             logging::Level::Warning);
        return false;
    }
    finish_line();
    cached.store(enabled ? 1 : 0, std::memory_order_relaxed);
    mlog(std::string("storage: CONFIG_TMPFS_XATTR=") +
         (enabled ? "y; defaulting to tmpfs" : "disabled; defaulting to ext4"));
    return enabled;
}

namespace {
bool kernel_umount_command(const char* op, const std::string& path) {
    const char* ksud = nullptr;
    for (const char* candidate : {"/data/adb/ksud", "/data/adb/ksu/bin/ksud"}) {
        if (access(candidate, X_OK) == 0) {
            ksud = candidate;
            break;
        }
    }
    if (!ksud)
        return true;
    const pid_t pid = fork();
    if (pid == 0) {
        execl(ksud, "ksud", "kernel", "umount", op, path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    pid_t waited = -1;
    if (pid > 0) {
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    const bool ok = waited == pid && pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok)
        mlog("failed to register unmount path " + path, logging::Level::Error);
    return ok;
}
}  // namespace

bool capture_mount_identity(const std::string& path, MountRecord& record) {
    struct statx st{};
    constexpr unsigned mask = STATX_INO | STATX_MNT_ID;
    if (syscall(__NR_statx, AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, mask,
                &st) != 0 ||
        (st.stx_mask & mask) != mask ||
        ((st.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) &&
         !(st.stx_attributes & STATX_ATTR_MOUNT_ROOT)))
        return false;
    record = {path, st.stx_mnt_id,
              (static_cast<uint64_t>(st.stx_dev_major) << 32) | st.stx_dev_minor, st.stx_ino};
    return true;
}

bool mount_matches(const MountRecord& record, bool include_init) {
    MountRecord live;
    if (capture_mount_identity(record.path, live) && live.mount_id == record.mount_id &&
        live.device == record.device && live.inode == record.inode)
        return true;
    if (!include_init)
        return false;
    MountRecord init = record;
    init.path = "/proc/1/root" + record.path;
    return mount_matches(init, false);
}

bool read_mount_journal(const std::string& journal, std::vector<MountRecord>& records,
                        bool& legacy) {
    legacy = false;
    records.clear();
    std::ifstream input(journal);
    if (!input)
        return errno == ENOENT;
    std::string line;
    if (!std::getline(input, line))
        return input.eof();
    constexpr const char* prefix = "KAGAMI_MOUNTS_V2 ";
    if (line.rfind(prefix, 0) != 0) {
        legacy = true;
        do {
            if (!line.empty()) {
                if (line.front() != '/' || line.find('\0') != std::string::npos)
                    return false;
                records.push_back({line});
            }
        } while (std::getline(input, line));
        return input.eof();
    }
    const auto boot = runtime_boot_id();
    if (boot.empty())
        return false;
    if (line.substr(std::strlen(prefix)) != boot)
        return true;
    while (std::getline(input, line)) {
        MountRecord record;
        std::istringstream row(line);
        if (!(row >> record.mount_id >> record.device >> record.inode >>
              std::quoted(record.path)) ||
            !(row >> std::ws).eof() || record.path.empty() || record.path.front() != '/' ||
            record.path.find_first_of("\r\n") != std::string::npos ||
            record.path.find('\0') != std::string::npos)
            return false;
        records.push_back(std::move(record));
    }
    return input.eof();
}

bool write_mount_journal(const std::string& journal, const std::vector<std::string>& mounts) {
    const auto boot = runtime_boot_id();
    if (boot.empty())
        return false;
    std::ostringstream data;
    data << "KAGAMI_MOUNTS_V2 " << boot << "\n";
    for (const auto& path : mounts) {
        MountRecord record;
        if (path.find_first_of("\r\n") != std::string::npos ||
            !capture_mount_identity(path, record))
            return false;
        data << record.mount_id << " " << record.device << " " << record.inode << " "
             << std::quoted(path) << "\n";
    }
    std::string error;
    const bool ok = kagami::write_file_atomic(journal, data.str(), error);
    if (!ok)
        mlog(error, logging::Level::Error);
    return ok;
}

std::string decode_mount_path(const std::string& input) {
    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 3 < input.size() && input[i + 1] >= '0' &&
            input[i + 1] <= '3' && input[i + 2] >= '0' && input[i + 2] <= '7' &&
            input[i + 3] >= '0' && input[i + 3] <= '7') {
            out += static_cast<char>(((input[i + 1] - '0') * 64) + ((input[i + 2] - '0') * 8) +
                                     input[i + 3] - '0');
            i += 3;
        } else {
            out += input[i];
        }
    }
    return out;
}

bool register_umount(const std::string& path) {
    return kernel_umount_command("add", path);
}
bool unregister_umount(const std::string& path) {
    return kernel_umount_command("del", path);
}

bool prepare_empty_mountpoint(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path value(path);
    std::error_code ec;
    if (path.find('\0') != std::string::npos || !value.is_absolute() ||
        value.lexically_normal() == value.root_path() ||
        fs::weakly_canonical(value, ec) != value.lexically_normal() || ec) {
        mlog("refusing unsafe mountpoint " + path, logging::Level::Error);
        return false;
    }
    fs::create_directories(value, ec);
    if (ec || !fs::is_empty(value, ec) || ec) {
        mlog("mountpoint must be an empty directory: " + path, logging::Level::Error);
        return false;
    }
    return true;
}

std::string partition_mount_point(std::string_view partition) {
    if (partition == "system") {
        return "/system";
    }
    return std::string("/").append(partition);
}

std::string resolve_real_mount_target(const std::string& mount_point) {
    char buf[PATH_MAX];
    if (realpath(mount_point.c_str(), buf) == nullptr) {
        return "";
    }
    return {buf};
}

}  // namespace kagami::mount::fsutil
