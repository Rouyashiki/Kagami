#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace kagami::posix {

class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0)
            close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    int get() const noexcept { return fd_; }

private:
    int fd_;
};

struct DirectoryEntry {
    std::string_view name;
    unsigned char type = 0;
    // next() validates the record terminator before publishing this view.
    const char* c_str() const noexcept { return name.data(); }
};

// Linux getdents64 avoids libc's per-opendir allocation and returns d_type for
// ordinary entries. Names borrow the supplied buffer until the next refill.
class Directory {
public:
    Directory(int fd, char* buffer, std::size_t capacity) noexcept
        : fd_(fd), buffer_(buffer), capacity_(capacity) {}
    [[nodiscard]] bool next(DirectoryEntry& entry, int& error) noexcept {
        error = 0;
        for (;;) {
            if (offset_ == size_) {
                long count;
                do {
                    count = syscall(SYS_getdents64, fd_, buffer_, capacity_);
                } while (count < 0 && errno == EINTR);
                if (count <= 0) {
                    error = count < 0 ? errno : 0;
                    return false;
                }
                size_ = static_cast<std::size_t>(count);
                offset_ = 0;
            }
            // d_ino:u64, d_off:i64, d_reclen:u16, d_type:u8, d_name[].
            constexpr std::size_t name_offset = 19;
            if (size_ - offset_ <= name_offset) {
                error = EIO;
                return false;
            }
            std::uint16_t length;
            std::memcpy(&length, buffer_ + offset_ + 16, sizeof(length));
            if (length <= name_offset || length > size_ - offset_) {
                error = EIO;
                return false;
            }
            const char* name = buffer_ + offset_ + name_offset;
            const auto* end = static_cast<const char*>(std::memchr(name, 0, length - name_offset));
            if (!end) {
                error = EIO;
                return false;
            }
            entry = {{name, static_cast<std::size_t>(end - name)},
                     static_cast<unsigned char>(buffer_[offset_ + 18])};
            offset_ += length;
            if (entry.name != "." && entry.name != "..")
                return true;
        }
    }

private:
    int fd_;
    char* buffer_;
    std::size_t capacity_;
    std::size_t offset_ = 0;
    std::size_t size_ = 0;
};

// d_type is authoritative for entries that are not links. Follow symlinks just
// as filesystem::is_directory/exists did; unknown types use one fstatat.
inline bool is_directory(int fd, const DirectoryEntry& entry) noexcept {
    constexpr unsigned char unknown = 0, directory = 4, link = 10;
    if (entry.type == directory)
        return true;
    if (entry.type != unknown && entry.type != link)
        return false;
    struct stat st;
    return fstatat(fd, entry.c_str(), &st, 0) == 0 && S_ISDIR(st.st_mode);
}

inline bool exists(int fd, const DirectoryEntry& entry) noexcept {
    if (entry.type != 0 && entry.type != 10)
        return true;
    struct stat st;
    return fstatat(fd, entry.c_str(), &st, 0) == 0;
}

}  // namespace kagami::posix
