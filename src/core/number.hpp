#pragma once

#include <cctype>
#include <cerrno>
#include <charconv>
#include <string_view>

namespace kagami {

// strtoul(base=0) syntax, with an explicit end and checked overflow. Preserve
// the old unsigned negation semantics; callers enforce narrower UID bounds.
[[nodiscard]] inline bool parse_unsigned(std::string_view text, unsigned long& result,
                                         int base = 0) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty())
        return false;
    if (base == 0 && text.front() == '0') {
        base = 8;
        if (text.size() > 2 && (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
            text.remove_prefix(2);
        }
    }
    if (base == 0)
        base = 10;
    unsigned long value;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        if (parsed.ec == std::errc::result_out_of_range)
            errno = ERANGE;
        return false;
    }
    result = negative ? 0UL - value : value;
    return true;
}

}  // namespace kagami
