#pragma once

#include "core/bounded.hpp"

#include <unistd.h>
#include <cerrno>
#include <filesystem>

namespace kagami::output {

// Explicit, caller-owned destinations. No global/TLS lookup, allocation or
// first-use initialization; each concurrent invocation has its own context.
struct Context {
    bounded::Writer out;
    bounded::Writer err;
    int error = 0;
};

[[nodiscard]] inline bool append(bounded::Writer& out, bounded::Quoted value) noexcept {
    return out.append('"') && bounded::escape(out, value.value, value.all_controls) &&
           out.append('"');
}
[[nodiscard]] inline bool append(bounded::Writer& out,
                                 const std::filesystem::path& value) noexcept {
    // std::filesystem's stream insertion quotes paths and escapes backslashes.
    if (!out.append('"'))
        return false;
    for (char c : value.native()) {
        if ((c == '\\' || c == '"') && !out.append('\\'))
            return false;
        if (!out.append(c))
            return false;
    }
    return out.append('"');
}
template <typename T>
[[nodiscard]] bool append(bounded::Writer& out, const T& value) noexcept {
    return out.append(value);
}

// All writes are checked and their first error is latched. The command boundary
// must consume Context::error before publishing any success or output bytes.
template <bool Error, typename... T>
void write(Context& context, const T&... values) noexcept {
    if (context.error)
        return;
    auto& out = Error ? context.err : context.out;
    if (!(append(out, values) && ...))
        context.error = ENOBUFS;
}
template <typename... T>
void out(Context& context, const T&... values) noexcept {
    write<false>(context, values...);
}
template <typename... T>
void err(Context& context, const T&... values) noexcept {
    write<true>(context, values...);
}

[[nodiscard]] inline bool write_fd(int fd, std::string_view bytes) noexcept {
    while (!bytes.empty()) {
        const auto count = ::write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

}  // namespace kagami::output
