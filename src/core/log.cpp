#include "core/log.hpp"
#include "core/runtime.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kagami::logging {
namespace {
std::atomic<bool> debug_enabled{false};
constexpr std::time_t first_wall_clock = 1577836800;
struct Fd {
    int value = -1;
    ~Fd() {
        if (value >= 0)
            close(value);
    }
};
struct ErrnoGuard {
    int saved = errno;
    ~ErrnoGuard() { errno = saved; }
};
struct Rotation {
    std::string boot;
    char phase = 'R';
    unsigned long long device = 0, inode = 0;
};
bool write_all(int fd, const std::string &text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const auto count = ::write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count == 0)
                errno = EIO;
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    return true;
}
bool regular(int fd) {
    struct stat st{};
    if (fstat(fd, &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
        errno = EINVAL;
        return false;
    }
    return true;
}
bool save_rotation(const std::string &path, const Rotation &state) {
    std::string temporary = path + ".tmp.XXXXXX";
    Fd file{mkostemp(temporary.data(), O_CLOEXEC)};
    if (file.value < 0)
        return false;
    const std::string record = state.boot + " " + state.phase + " " + std::to_string(state.device) +
                               " " + std::to_string(state.inode) + "\n";
    const bool saved = write_all(file.value, record) && fsync(file.value) == 0 &&
                       rename(temporary.c_str(), path.c_str()) == 0;
    if (!saved)
        unlink(temporary.c_str());
    if (!saved)
        return false;
    Fd directory{open(std::filesystem::path(path).parent_path().c_str(),
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    return directory.value >= 0 && fsync(directory.value) == 0;
}
bool prepare_locked(const std::string &path, const std::string &boot, std::string &error) {
    if (boot.empty() || boot.find_first_of(" \r\n\t") != std::string::npos) {
        error = "boot identity unavailable";
        return false;
    }
    const std::string marker = path + ".boot";
    Rotation state;
    Fd previous{open(marker.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (previous.value >= 0) {
        char record[256]{};
        const auto length = read(previous.value, record, sizeof(record) - 1);
        if (!regular(previous.value) || length < 0)
            return false;
        std::istringstream input(record);
        if (!(input >> state.boot >> state.phase >> state.device >> state.inode) ||
            (state.phase != 'R' && state.phase != 'P'))
            state = {};
    } else if (errno != ENOENT)
        return false;
    struct stat st{};
    const bool exists = lstat(path.c_str(), &st) == 0;
    if (!exists && errno != ENOENT)
        return false;
    if (exists && (!S_ISREG(st.st_mode) || st.st_nlink != 1)) {
        errno = EINVAL;
        return false;
    }
    if (state.boot != boot) {
        state = {boot, 'P', exists ? static_cast<unsigned long long>(st.st_dev) : 0,
                 exists ? static_cast<unsigned long long>(st.st_ino) : 0};
        if (!save_rotation(marker, state))
            return false;
    }
    // A durable pending record identifies the old inode. A restarted writer
    // cannot rotate the newly created file over the previous boot's history.
    if (state.phase == 'P' && exists &&
        state.device == static_cast<unsigned long long>(st.st_dev) &&
        state.inode == static_cast<unsigned long long>(st.st_ino)) {
        if (rename(path.c_str(), (path + ".old").c_str()) != 0)
            return false;
    }
    Fd file{open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (file.value < 0 || !regular(file.value) || fchmod(file.value, 0600) != 0)
        return false;
    if (state.phase == 'P') {
        state.phase = 'R';
        if (!save_rotation(marker, state))
            return false;
    }
    return true;
}
const char *name(Level level) {
    switch (level) {
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warning:
        return "WARN";
    case Level::Error:
        return "ERROR";
    }
    return "ERROR";
}
std::string clean(const std::string &text) {
    std::string output;
    for (const unsigned char c : text) {
        if (output.size() >= 8192) {
            output += "...[truncated]";
            break;
        }
        if (c == '\n')
            output += "\\n";
        else if (c == '\r')
            output += "\\r";
        else if (c < 0x20 && c != '\t')
            output += '?';
        else
            output += static_cast<char>(c);
    }
    return output;
}
} // namespace

void set_debug_enabled(bool value) noexcept { debug_enabled.store(value); }
bool enabled(Level level) noexcept { return level != Level::Debug || debug_enabled.load(); }

std::string timestamp() {
    const auto now = std::time(nullptr);
    char buffer[64]{};
    if (now >= first_wall_clock) {
        tm local{};
        if (localtime_r(&now, &local)) {
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
            return buffer;
        }
    }
#if defined(CLOCK_BOOTTIME)
    timespec boot{};
    if (clock_gettime(CLOCK_BOOTTIME, &boot) == 0) {
        std::snprintf(buffer, sizeof(buffer), "early-boot +%lld.%03ld",
                      static_cast<long long>(boot.tv_sec), boot.tv_nsec / 1000000L);
        return buffer;
    }
#endif
    return "early-boot (clock unset)";
}

bool prepare_boot_log(std::string &error, const std::string &boot_id) {
    error.clear();
    ErrnoGuard saved;
    const auto path = runtime_log_file();
    if (!prepare_private_directory(path.parent_path(), error))
        return false;
    Fd lock{
        open((path.string() + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (lock.value < 0 || !regular(lock.value) || flock(lock.value, LOCK_EX) != 0 ||
        !prepare_locked(path.string(), boot_id.empty() ? runtime_boot_id() : boot_id, error)) {
        if (error.empty())
            error = "prepare " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

void write(Level level, const std::string &component, const std::string &message) noexcept {
    ErrnoGuard saved;
    if (!enabled(level))
        return;
    try {
        const auto path = runtime_log_file();
        std::string error;
        if (!prepare_private_directory(path.parent_path(), error)) {
            std::fprintf(stderr, "[ERROR] [logging] %s\n", error.c_str());
            return;
        }
        Fd lock{open((path.string() + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                     0600)};
        if (lock.value < 0 || !regular(lock.value) || flock(lock.value, LOCK_EX) != 0 ||
            !prepare_locked(path.string(), runtime_boot_id(), error)) {
            std::fprintf(stderr, "[ERROR] [logging] %s: %s\n", path.c_str(), std::strerror(errno));
            return;
        }
        Fd file{open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW)};
        const std::string line = timestamp() + " [" + name(level) + "] [" + clean(component) +
                                 "] " + clean(message) + "\n";
        if (file.value < 0 || !regular(file.value) || !write_all(file.value, line))
            std::fprintf(stderr, "[ERROR] [logging] append %s: %s\n", path.c_str(),
                         std::strerror(errno));
    } catch (...) {
        std::fputs("[ERROR] [logging] log write failed\n", stderr);
    }
}

void append(const std::string &component, const std::string &message) {
    write(Level::Info, component, message);
}
} // namespace kagami::logging
