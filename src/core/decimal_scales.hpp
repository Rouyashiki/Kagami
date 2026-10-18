#pragma once

#include <array>
#include <cstdint>

namespace kagami::decimal::detail {
struct Wide {
    std::uint64_t low, high;
};

// Exact on the binary64 exponent range [-1074, 1023]. Division must round
// down for negative exponents, rather than C++'s truncation toward zero.
constexpr int floor_log10_pow2(int exponent) noexcept {
    const int product = exponent * 78913;
    return product / 262144 - (product < 0 && product % 262144 != 0);
}

namespace scale_generation {
// 10^340 needs 1130 bits; 36 little-endian words provide 1152 bits.
using Integer = std::array<std::uint32_t, 36>;

constexpr void multiply_ten(Integer& value) noexcept {
    std::uint64_t carry = 0;
    for (auto& word : value) {
        const auto product = static_cast<std::uint64_t>(word) * 10 + carry;
        word = static_cast<std::uint32_t>(product);
        carry = product >> 32;
    }
}

constexpr void divide_ten(Integer& value) noexcept {
    std::uint64_t remainder = 0;
    for (std::size_t i = value.size(); i > 0; --i) {
        const auto dividend = (remainder << 32) | value[i - 1];
        value[i - 1] = static_cast<std::uint32_t>(dividend / 10);
        remainder = dividend % 10;
    }
}

// Read 32 bits starting at a signed bit offset, with zero extension on
// either side. Negative offsets express multiplication by a power of two.
constexpr std::uint32_t word_at(const Integer& value, int bit) noexcept {
    if (bit < 0)
        return bit > -32 ? value[0] << -bit : 0;
    const auto index = static_cast<std::size_t>(bit / 32);
    if (index >= value.size())
        return 0;
    std::uint64_t pair = value[index];
    if (index + 1 < value.size())
        pair |= static_cast<std::uint64_t>(value[index + 1]) << 32;
    return static_cast<std::uint32_t>(pair >> (bit % 32));
}

constexpr Wide extract(const Integer& value, int shift) noexcept {
    return {word_at(value, shift) | (static_cast<std::uint64_t>(word_at(value, shift + 32)) << 32),
            word_at(value, shift + 64) |
                (static_cast<std::uint64_t>(word_at(value, shift + 96)) << 32)};
}

constexpr auto make_scales() noexcept {
    std::array<Wide, 2098> result{};
    Integer numerator{};
    numerator[0] = 1;
    int power = 0;
    // C[E] = floor(2^(E+60) * 10^q), q = 16 - floor(log10(2^E)).
    // For q >= 0, build exact powers of ten and extract the scaled bits.
    for (int exponent = 1023; exponent >= -1074; --exponent) {
        const int decimal = 16 - floor_log10_pow2(exponent);
        if (decimal < 0)
            continue;
        while (power < decimal) {
            multiply_ten(numerator);
            ++power;
        }
        result[exponent + 1074] = extract(numerator, -exponent - 60);
    }
    // For q < 0, keep floor(2^1083 / 10^(-q)). A final right shift is exact:
    // floor(floor(N / a) / b) == floor(N / (a*b)) for positive integers.
    // Thus repeated /10 and shifting by 1023-E produce the same C[E].
    numerator = {};
    numerator[1083 / 32] = std::uint32_t{1} << (1083 % 32);
    power = 0;
    for (int exponent = -1074; exponent <= 1023; ++exponent) {
        const int decimal = floor_log10_pow2(exponent) - 16;
        if (decimal <= 0)
            continue;
        while (power < decimal) {
            divide_ten(numerator);
            ++power;
        }
        result[exponent + 1074] = extract(numerator, 1023 - exponent);
    }
    return result;
}
}  // namespace scale_generation

// Required constant evaluation: no runtime generator or first-use setup.
inline constexpr auto scales = scale_generation::make_scales();
}  // namespace kagami::decimal::detail
