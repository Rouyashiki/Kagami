#pragma once

#include "core/prepared_input.hpp"

#include <array>
#include <string_view>

namespace kagami::mount_table {

// Display-only parsing retains the historical whitespace and incomplete-row
// behavior. Attachment identity uses its separate strict parser.
inline std::string_view next_token(std::string_view& remaining) noexcept {
    constexpr std::string_view whitespace = " \t\n\r\f\v";
    const auto start = remaining.find_first_not_of(whitespace);
    if (start == std::string_view::npos) {
        remaining = {};
        return {};
    }
    remaining.remove_prefix(start);
    const auto end = remaining.find_first_of(whitespace);
    const std::string_view token(remaining.data(), std::min(end, remaining.size()));
    remaining.remove_prefix(token.size());
    return token;
}

inline std::string_view next_line(std::string_view& remaining) noexcept {
    const auto count = std::min(remaining.find('\n'), remaining.size());
    const std::string_view line(remaining.data(), count);
    remaining.remove_prefix(count + (count < remaining.size()));
    return line;
}

struct Entry {
    std::string_view mount_point;
    std::string_view fstype;
    std::string_view source;
};

[[nodiscard]] inline bool parse_entry(std::string_view line, Entry& entry) noexcept {
    const auto separator = line.find(" - ");
    if (separator == std::string_view::npos)
        return false;
    std::string_view before(line.data(), separator);
    for (int field = 0; field < 4; ++field)
        if (next_token(before).empty())
            return false;
    const auto mount_point = next_token(before);
    if (mount_point.empty())
        return false;
    std::string_view after(line.data() + separator + 3, line.size() - separator - 3);
    entry = {mount_point, next_token(after), next_token(after)};
    return true;
}

// Consumes a writable token plus one accessible delimiter byte. Unescaped
// bytes stay where they were read; only an escape's shortened suffix moves.
// The resulting view is terminated and borrows the unique input owner.
inline std::string_view decode_token(char* token, std::size_t size) noexcept {
    std::size_t output = 0;
    for (std::size_t i = 0; i < size; ++i) {
        char value = token[i];
        if (value == '\\' && size - i > 3 && token[i + 1] >= '0' && token[i + 1] <= '3' &&
            token[i + 2] >= '0' && token[i + 2] <= '7' && token[i + 3] >= '0' &&
            token[i + 3] <= '7') {
            value = static_cast<char>((token[i + 1] - '0') * 64 + (token[i + 2] - '0') * 8 +
                                      (token[i + 3] - '0'));
            i += 3;
        }
        if (output != i)
            token[output] = value;
        ++output;
    }
    token[output] = '\0';
    return {token, output};
}

// Reads once into bounded storage. No tuple vector, per-field strings or
// per-line allocation; callbacks consume borrowed views at the batch boundary.
class Table {
public:
    Table() noexcept = default;
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) noexcept = default;
    Table& operator=(Table&&) noexcept = default;
    ~Table() = default;
    [[nodiscard]] bool prepare_file(const char* path, int& error) noexcept {
        return input_.prepare_file(path, error, PreparedInput::maximum);
    }
    [[nodiscard]] bool prepare_text(std::string_view text, int& error) noexcept {
        return input_.prepare_text(text, error);
    }
    template <typename Function>
    void consume(Function&& function) noexcept {
        // Destructive decoding; consume only once. Borrowed fields stay valid
        // until this storage is destroyed or move-assigned.
        auto remaining = input_.view();
        if (consumed_)
            return;
        consumed_ = true;
        while (!remaining.empty()) {
            const auto line = next_line(remaining);
            Entry entry;
            if (!parse_entry(line, entry))
                continue;
            entry.mount_point =
                decode_token(input_.data() + (entry.mount_point.data() - input_.data()),
                             entry.mount_point.size());
            if (!entry.source.empty())
                entry.source = decode_token(input_.data() + (entry.source.data() - input_.data()),
                                            entry.source.size());
            if (!function(entry))
                break;
        }
    }

private:
    PreparedInput input_;
    bool consumed_ = false;
};

template <std::size_t N>
std::array<bool, N> read_permissions(std::string_view input,
                                     const std::array<const char*, N>& mount_points) noexcept {
    std::array<bool, N> readonly{};
    std::array<bool, N> found{};
    std::size_t missing = N;
    while (missing && !input.empty()) {
        auto fields = next_line(input);
        (void)next_token(fields);
        const auto path = next_token(fields);
        (void)next_token(fields);
        const auto options = next_token(fields);
        for (std::size_t i = 0; i < N; ++i) {
            if (!found[i] && path == mount_points[i]) {
                // First match and substring check are compatibility behavior.
                readonly[i] = options.find("ro") != std::string_view::npos;
                found[i] = true;
                --missing;
            }
        }
    }
    return readonly;
}

}  // namespace kagami::mount_table
