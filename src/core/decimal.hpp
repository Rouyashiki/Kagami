#pragma once

#include "core/bounded.hpp"
#include "core/compiler.hpp"
#include "core/decimal_scales.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace kagami::decimal {
namespace detail {
constexpr auto powers_of_ten() noexcept {
    std::array<std::uint64_t, 20> values{};
    values[0] = 1;
    for (std::size_t i = 1; i < values.size(); ++i)
        values[i] = values[i - 1] * 10;
    return values;
}
inline constexpr auto tens = powers_of_ten();

// Centralized Clang integer extension; the 32-bit-limb fallback keeps the
// identical arithmetic contract on NDK targets without native uint128.
inline Wide multiply(std::uint64_t lhs, std::uint64_t rhs) noexcept {
#if defined(__SIZEOF_INT128__)
    const auto product = static_cast<unsigned __int128>(lhs) * rhs;
    return {static_cast<std::uint64_t>(product), static_cast<std::uint64_t>(product >> 64)};
#else
    const auto a = lhs >> 32, b = lhs & UINT32_MAX;
    const auto c = rhs >> 32, d = rhs & UINT32_MAX;
    const auto low = b * d;
    const auto middle = a * d + (low >> 32);
    const auto combined = b * c + (middle & UINT32_MAX);
    return {(combined << 32) | (low & UINT32_MAX), a * c + (middle >> 32) + (combined >> 32)};
#endif
}

// Round the exact real value only when its entire error interval selects one
// result. The cache is rounded down; exact - product is in [0, significand).
// False means an ambiguous tie and requests the exact integer fallback.
inline bool scaled(std::uint64_t significand, int exponent, std::uint64_t& coefficient,
                   int& decimal_exponent) noexcept {
    while (significand < (std::uint64_t{1} << 52)) {
        significand <<= 1;
        --exponent;
    }
    const int binary_log = exponent + 52;
    decimal_exponent = floor_log10_pow2(binary_log);
    const auto& power = scales[binary_log + 1074];
    const auto low = multiply(power.low, significand);
    const auto high = multiply(power.high, significand);
    const auto middle = low.high + high.low;
    const auto top = high.high + (middle < low.high);
    coefficient = (top << 16) | (middle >> 48);
    constexpr auto mask = (std::uint64_t{1} << 48) - 1;
    constexpr auto half = std::uint64_t{1} << 47;
    const auto remainder_high = middle & mask;
    // The lower 112 bits can carry across a rounding boundary only when
    // their low limb is within significand of UINT64_MAX.
    const bool carry_possible = low.low > UINT64_MAX - significand;
    if (coefficient >= tens[17]) {
        const auto last = coefficient % 10;
        coefficient /= 10;
        ++decimal_exponent;
        if (last == 4 && remainder_high == mask && carry_possible)
            return false;
        if (last == 5 && !remainder_high && !low.low)
            return false;
        if (last >= 5)
            ++coefficient;
    } else {
        if (remainder_high == half - 1 && carry_possible)
            return false;
        if (remainder_high == half && !low.low)
            return false;
        if (remainder_high >= half)
            ++coefficient;
    }
    return true;
}

inline unsigned digits(std::uint64_t value) noexcept {
    unsigned count = 1;
    while (count < tens.size() && value >= tens[count])
        ++count;
    return count;
}

// Exact decimal integer workspace for a finite binary64. At most 767 decimal
// digits are needed for significand * 5^1074; 96 base-1e9 limbs cover 864.
// Only active limbs are initialized. This stack workspace cannot be copied.
class Integer {
public:
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    explicit Integer(std::uint64_t value) noexcept {
        do {
            limbs_[size_++] = static_cast<std::uint32_t>(value % radix);
            value /= radix;
        } while (value);
    }
    Integer(const Integer&) = delete;
    Integer& operator=(const Integer&) = delete;
    Integer(Integer&&) = delete;
    Integer& operator=(Integer&&) = delete;
    ~Integer() = default;
    [[nodiscard]] bool multiply(std::uint32_t factor) noexcept {
        // All callers use factors below radix, so product+carry fits uint64
        // and the final carry itself fits exactly one base-1e9 limb.
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            const auto product = static_cast<std::uint64_t>(limbs_[i]) * factor + carry;
            limbs_[i] = static_cast<std::uint32_t>(product % radix);
            carry = product / radix;
        }
        if (carry) {
            if (size_ == limbs_.size())
                return false;
            limbs_[size_++] = static_cast<std::uint32_t>(carry);
        }
        return true;
    }
    unsigned decimal_digits() const noexcept {
        return static_cast<unsigned>((size_ - 1) * 9) + digits(limbs_[size_ - 1]);
    }
    unsigned digit(unsigned offset) const noexcept {
        return static_cast<unsigned>((limbs_[offset / 9] / tens[offset % 9]) % 10);
    }
    bool nonzero_below(unsigned offset) const noexcept {
        const auto whole = offset / 9;
        for (unsigned i = 0; i < whole; ++i)
            if (limbs_[i])
                return true;
        return offset % 9 && limbs_[whole] % tens[offset % 9];
    }

private:
    static constexpr std::uint32_t radix = 1000000000;
    std::array<std::uint32_t, 96> limbs_;
    std::size_t size_ = 0;
};

inline bool emit(bounded::Writer& output, std::uint64_t coefficient, unsigned count, int exponent,
                 bool negative) noexcept {
    if (coefficient == tens[count]) {
        coefficient /= 10;
        ++exponent;
    }
    while (count > 1 && coefficient % 10 == 0) {
        coefficient /= 10;
        --count;
    }
    const bool scientific = exponent < -4 || exponent >= 17;
    const unsigned exponent_magnitude = static_cast<unsigned>(exponent < 0 ? -exponent : exponent);
    const unsigned exponent_digits = std::max(2U, digits(exponent_magnitude));
    std::size_t length = negative ? 1 : 0;
    if (scientific)
        length += count + (count > 1) + 2 + exponent_digits;
    else if (exponent < 0)
        length += 1 + exponent_magnitude + count;
    else
        length += std::max(count, static_cast<unsigned>(exponent + 1)) +
                  (count > static_cast<unsigned>(exponent + 1));
    if (length > output.remaining())
        return false;
    if (negative && !output.append('-'))
        return false;
    if (!scientific && exponent < 0) {
        if (!output.append("0."))
            return false;
        for (int i = -1; i > exponent; --i)
            if (!output.append('0'))
                return false;
    }
    auto divisor = tens[count - 1];
    for (unsigned i = 0; i < count; ++i) {
        if (i && ((scientific && i == 1) || (!scientific && static_cast<int>(i) == exponent + 1)) &&
            !output.append('.'))
            return false;
        if (!output.append(static_cast<char>('0' + coefficient / divisor)))
            return false;
        coefficient %= divisor;
        divisor /= 10;
    }
    if (scientific) {
        return output.append('e') && output.append(exponent < 0 ? '-' : '+') &&
               (exponent_magnitude >= 10 || output.append('0')) &&
               output.append(exponent_magnitude);
    }
    for (int i = static_cast<int>(count); i <= exponent; ++i)
        if (!output.append('0'))
            return false;
    return true;
}

inline bool integer(bounded::Writer& output, std::uint64_t value, bool negative) noexcept {
    const auto count = digits(value);
    const auto kept = std::min(count, 17U);
    if (count > kept) {
        const auto divisor = tens[count - kept];
        const auto remainder = value % divisor;
        value /= divisor;
        if (remainder > divisor / 2 || (remainder == divisor / 2 && (value & 1)))
            ++value;
    }
    return emit(output, value, kept, static_cast<int>(count) - 1, negative);
}
// The final ARM64 code otherwise reserved 496 bytes, including this exact
// workspace, even on integer/cache hits. Keep the rare tie calculation out of
// that frame; this boundary is deliberate and remains independent of -Os.
KAGAMI_NOINLINE inline bool write_exact(bounded::Writer& output, std::uint64_t significand,
                                        int binary_exponent, bool negative) noexcept {
    while (binary_exponent < 0 && !(significand & 1)) {
        significand >>= 1;
        ++binary_exponent;
    }
    const int scale = binary_exponent < 0 ? -binary_exponent : 0;
    Integer exact(significand);
    if (binary_exponent < 0) {
        int remaining = -binary_exponent;
        while (remaining >= 12) {
            if (!exact.multiply(244140625))  // 5^12 < 1e9.
                return false;
            remaining -= 12;
        }
        std::uint32_t factor = 1;
        while (remaining-- > 0)
            factor *= 5;
        if (!exact.multiply(factor))
            return false;
    } else {
        while (binary_exponent >= 29) {
            if (!exact.multiply(1U << 29))
                return false;
            binary_exponent -= 29;
        }
        if (!exact.multiply(1U << binary_exponent))
            return false;
    }
    const auto count = exact.decimal_digits();
    const auto kept = std::min(count, 17U);
    std::uint64_t coefficient = 0;
    for (unsigned i = 0; i < kept; ++i)
        coefficient = coefficient * 10 + exact.digit(count - i - 1);
    if (count > kept) {
        const auto rounding_position = count - kept - 1;
        const auto rounding = exact.digit(rounding_position);
        if (rounding > 5 ||
            (rounding == 5 && (exact.nonzero_below(rounding_position) || (coefficient & 1))))
            ++coefficient;
    }
    return emit(output, coefficient, kept, static_cast<int>(count) - scale - 1, negative);
}
}  // namespace detail

// Locale-independent general format with 17 significant digits and round-to-
// nearest/even, matching the saved configuration format. Integer settings take
// a scalar fast path. No libc floating formatter, exception landing pad, heap,
// intermediate text buffer, or first-use initialization is reachable here.
[[nodiscard]] inline bool write(bounded::Writer& output, double value) noexcept {
    static_assert(sizeof(double) == sizeof(std::uint64_t) &&
                  std::numeric_limits<double>::is_iec559);
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const bool negative = (bits >> 63) != 0;
    const auto encoded_exponent = static_cast<unsigned>((bits >> 52) & 0x7ff);
    auto significand = bits & ((std::uint64_t{1} << 52) - 1);
    if (encoded_exponent == 0x7ff)
        return false;
    if (!encoded_exponent && !significand)
        return output.append(negative ? "-0" : "0");
    int binary_exponent = -1074;
    if (encoded_exponent) {
        significand |= std::uint64_t{1} << 52;
        binary_exponent = static_cast<int>(encoded_exponent) - 1075;
    }
    if (binary_exponent >= 0 && binary_exponent < 64 &&
        significand <= (std::numeric_limits<std::uint64_t>::max() >> binary_exponent))
        return detail::integer(output, significand << binary_exponent, negative);
    if (binary_exponent < 0 && binary_exponent > -64 &&
        !(significand & ((std::uint64_t{1} << -binary_exponent) - 1)))
        return detail::integer(output, significand >> -binary_exponent, negative);
    std::uint64_t rounded;
    int decimal_exponent;
    if (detail::scaled(significand, binary_exponent, rounded, decimal_exponent))
        return detail::emit(output, rounded, 17, decimal_exponent, negative);
    return detail::write_exact(output, significand, binary_exponent, negative);
}

}  // namespace kagami::decimal
