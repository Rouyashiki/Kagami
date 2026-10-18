#pragma once

#include "core/bounded.hpp"

#include <cstdint>
#include <iterator>

namespace kagami {

// Keep the former binary units and default round-to-nearest/even display,
// including the initial uint64 -> double rounding. The bounded range below
// needs at most a 56-bit significand; no floating formatting runtime is used.
[[nodiscard]] inline bool write_human_bytes(bounded::Writer& output,
                                            unsigned long long bytes) noexcept {
    constexpr char units[] = {'B', 'K', 'M', 'G', 'T'};
    double value = static_cast<double>(bytes);
    unsigned unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    if (!unit)
        return output.append(bytes) && output.append(units[0]);
    const bool fractional = value < 10.0;
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    auto significand = (bits & ((std::uint64_t{1} << 52) - 1)) | (std::uint64_t{1} << 52);
    int exponent = static_cast<int>((bits >> 52) & 0x7ff) - 1075;
    if (fractional) {
        significand *= 5;
        ++exponent;
    }
    // After unit selection, 1 <= value <= 2^24, hence -52 <= exponent <= -28
    // (or -51 <= exponent <= -48 for the one-fractional-digit case).
    const auto shift = static_cast<unsigned>(-exponent);
    const auto mask = (std::uint64_t{1} << shift) - 1;
    const auto half = std::uint64_t{1} << (shift - 1);
    auto rounded = significand >> shift;
    const auto remainder = significand & mask;
    if (remainder > half || (remainder == half && (rounded & 1)))
        ++rounded;
    if (fractional)
        return output.append(rounded / 10) && output.append('.') &&
               output.append(static_cast<char>('0' + rounded % 10)) && output.append(units[unit]);
    return output.append(rounded) && output.append(units[unit]);
}

}  // namespace kagami
