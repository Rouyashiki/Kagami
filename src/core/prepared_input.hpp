#pragma once

#include "core/bounded.hpp"
#include "core/directory.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>

namespace kagami {

// Owns the final input buffer. Preparation reads straight into it; no stream,
// intermediate data block or capacity growth. Views are invalidated only by
// destruction/move assignment or explicit in-place decoding by the consumer.
class PreparedInput {
public:
    static constexpr std::size_t maximum = 1024 * 1024;
    PreparedInput() noexcept = default;
    PreparedInput(const PreparedInput&) = delete;
    PreparedInput& operator=(const PreparedInput&) = delete;
    PreparedInput(PreparedInput&& other) noexcept
        : bytes_(std::move(other.bytes_)), size_(std::exchange(other.size_, 0)) {}
    PreparedInput& operator=(PreparedInput&& other) noexcept {
        if (this != &other) {
            bytes_ = std::move(other.bytes_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }
    ~PreparedInput() = default;
    [[nodiscard]] bool prepare_text(std::string_view text, int& error) noexcept {
        error = 0;
        if (text.size() > maximum) {
            error = EOVERFLOW;
            return false;
        }
        if (!bytes_.prepare_storage(text.size() + 1, maximum + 1)) {
            error = ENOMEM;
            return false;
        }
        if (!text.empty())
            std::memcpy(bytes_.data(), text.data(), text.size());
        size_ = text.size();
        bytes_.data()[size_] = '\0';
        return true;
    }
    [[nodiscard]] bool prepare_file(const char* path, int& error,
                                    std::size_t minimum_capacity = 4096) noexcept {
        error = 0;
        if (minimum_capacity > maximum) {
            error = EOVERFLOW;
            return false;
        }
        const posix::Fd fd(open(path, O_RDONLY | O_CLOEXEC));
        if (fd.get() < 0) {
            error = errno;
            return false;
        }
        // fstat initializes every consumed field before access.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct stat info;
        if (fstat(fd.get(), &info)) {
            error = errno;
            return false;
        }
        if (info.st_size < 0) {
            error = EIO;
            return false;
        }
        if (static_cast<std::uint64_t>(info.st_size) > maximum) {
            error = EOVERFLOW;
            return false;
        }
        // The bounded allowance handles small concurrent appends and proc files
        // reporting size zero. Growth beyond the prepared capacity is an error.
        const auto capacity = std::max(
            minimum_capacity, std::min(maximum, static_cast<std::size_t>(info.st_size) + 4096));
        if (!bytes_.prepare_storage(capacity + 1, maximum + 1)) {
            error = ENOMEM;
            return false;
        }
        for (;;) {
            const auto count = read(fd.get(), bytes_.data() + size_, capacity + 1 - size_);
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0) {
                error = errno;
                size_ = 0;
                return false;
            }
            if (!count) {
                bytes_.data()[size_] = '\0';
                return true;
            }
            size_ += static_cast<std::size_t>(count);
            if (size_ > capacity) {
                error = EOVERFLOW;
                size_ = 0;
                return false;
            }
        }
    }
    char* data() noexcept { return bytes_.data(); }
    std::size_t size() const noexcept { return size_; }
    std::string_view view() const noexcept { return {bytes_.data(), size_}; }

private:
    bounded::Storage bytes_;
    std::size_t size_ = 0;
};

}  // namespace kagami
