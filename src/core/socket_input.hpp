#pragma once

#include "core/bounded.hpp"

#include <unistd.h>
#include <cerrno>

namespace kagami::daemon_protocol {

inline constexpr std::size_t request_limit = 64UL * 1024;
inline constexpr std::size_t response_limit = 1024UL * 1024;
inline constexpr std::size_t read_chunk = 1024;

// Storage includes the final entire read. Check size before newline, matching
// the established framing rule; no allocation or rescanning of previous bytes.
[[nodiscard]] inline bool read_message_into(int fd, char* bytes, std::size_t capacity,
                                            std::size_t limit, std::size_t& used) noexcept {
    used = 0;
    if (limit > capacity || read_chunk > capacity - limit) {
        errno = ENOBUFS;
        return false;
    }
    for (;;) {
        const auto count = read(fd, bytes + used, read_chunk);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return true;
        const auto previous = used;
        used += static_cast<std::size_t>(count);
        if (used > limit) {
            used = 0;
            errno = EMSGSIZE;
            return false;
        }
        if (std::memchr(bytes + previous, '\n', static_cast<std::size_t>(count)))
            return true;
    }
}

class Message {
public:
    Message() noexcept = default;
    ~Message() = default;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    Message(Message&&) noexcept = default;
    Message& operator=(Message&&) noexcept = default;
    [[nodiscard]] bool prepare_message(std::size_t limit) noexcept {
        if (limit > response_limit)
            return false;
        if (storage_.data()) {
            used_ = 0;
            return limit == limit_;
        }
        limit_ = limit;
        return storage_.prepare_storage(limit + read_chunk, response_limit + read_chunk);
    }
    [[nodiscard]] bool read_from(int fd) noexcept {
        return read_message_into(fd, storage_.data(), storage_.size(), limit_, used_);
    }
    std::string_view view() const noexcept { return {storage_.data(), used_}; }
    char* data() noexcept { return storage_.data(); }
    std::size_t size() const noexcept { return used_; }

private:
    bounded::Storage storage_;
    std::size_t limit_ = 0;
    std::size_t used_ = 0;
};

}  // namespace kagami::daemon_protocol
