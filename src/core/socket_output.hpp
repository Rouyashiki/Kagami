#pragma once

#include "core/bounded.hpp"
#include "core/socket_input.hpp"

#include <sys/socket.h>
#include <cerrno>

namespace kagami::daemon_protocol {

// This is a transport buffer, not an owning response string. Escaping writes
// directly to the final wire representation and flushes in bounded batches.
class SocketOutput {
public:
    SocketOutput(int fd, char* bytes, std::size_t capacity) noexcept
        : fd_(fd), bytes_(bytes), capacity_(capacity) {}
    SocketOutput(const SocketOutput&) = delete;
    SocketOutput& operator=(const SocketOutput&) = delete;
    SocketOutput(SocketOutput&&) = delete;
    SocketOutput& operator=(SocketOutput&&) = delete;
    ~SocketOutput() = default;
    [[nodiscard]] bool flush() noexcept {
        std::size_t offset = 0;
        while (offset != used_) {
            const auto count = send(fd_, bytes_ + offset, used_ - offset, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            offset += static_cast<std::size_t>(count);
        }
        used_ = 0;
        return true;
    }
    [[nodiscard]] bool append(std::string_view value) noexcept {
        if (!capacity_) {
            errno = ENOBUFS;
            return false;
        }
        while (!value.empty()) {
            if (used_ == capacity_ && !flush())
                return false;
            const auto available = capacity_ - used_;
            const auto count = value.size() < available ? value.size() : available;
            std::memcpy(bytes_ + used_, value.data(), count);
            used_ += count;
            value.remove_prefix(count);
        }
        return true;
    }

private:
    int fd_;
    char* bytes_;
    std::size_t capacity_;
    std::size_t used_ = 0;
};

[[nodiscard]] inline bool send_response(int fd, bool ok, int code, int error_number,
                                        std::string_view output, std::string_view error,
                                        char* bytes, std::size_t capacity) noexcept {
    char prefix[96];
    bounded::Writer header(prefix, sizeof(prefix));
    const auto prepare_header = [&]() noexcept {
        header.reset();
        return header.write(ok ? "{\"ok\":true,\"exit_code\":" : "{\"ok\":false,\"exit_code\":",
                            code, ",\"errno\":", error_number, ",\"stdout\":\"");
    };
    if (!prepare_header())
        return false;
    const auto overhead = header.view().size() + std::string_view("\",\"stderr\":\"").size() + 3;
    const auto budget = response_limit - overhead;
    // Most replies fit even under the worst-case escaping expansion. Only
    // large replies need an exact preflight before any bytes are published.
    if (output.size() > budget / 2 || error.size() > budget / 2 - output.size()) {
        std::size_t size = overhead;
        const auto fits = [&](std::string_view value) noexcept {
            for (char c : value) {
                size += c == '\"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' ? 2U : 1U;
                if (size > response_limit)
                    return false;
            }
            return true;
        };
        if (!fits(output) || !fits(error)) {
            ok = false;
            code = 1;
            error_number = ENOBUFS;
            output = {};
            error = "encoded response exceeds protocol capacity\n";
            if (!prepare_header())
                return false;
        }
    }
    SocketOutput wire(fd, bytes, capacity);
    return wire.append(header.view()) && bounded::escape(wire, output) &&
           wire.append("\",\"stderr\":\"") && bounded::escape(wire, error) &&
           wire.append("\"}\n") && wire.flush();
}

}  // namespace kagami::daemon_protocol
