#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace kagami::json {

// Borrowed, mutable input. Only decoded strings change, and always shrink in
// place. All views remain valid until the caller releases or reuses the input.
template <bool Decode>
class BasicCursor {
public:
    BasicCursor(char* bytes, std::size_t size) noexcept : bytes_(bytes), size_(size) {}
    std::size_t position() const noexcept { return position_; }
    char peek() noexcept {
        while (position_ < size_ && (bytes_[position_] == ' ' || bytes_[position_] == '\n' ||
                                     bytes_[position_] == '\r' || bytes_[position_] == '\t'))
            ++position_;
        return position_ < size_ ? bytes_[position_] : '\0';
    }
    bool consume(char value) noexcept {
        if (peek() != value || position_ == size_)
            return false;
        ++position_;
        return true;
    }
    bool end() noexcept {
        (void)peek();
        return position_ == size_;
    }
    bool literal(std::string_view value) noexcept {
        if (value.size() > size_ - position_ ||
            std::memcmp(bytes_ + position_, value.data(), value.size()) != 0)
            return false;
        position_ += value.size();
        return true;
    }
    bool number(double& value) noexcept {
        (void)peek();
        const auto first = position_;
        if (position_ < size_ && bytes_[position_] == '-')
            ++position_;
        const auto digits = [&]() noexcept {
            while (position_ < size_ && bytes_[position_] >= '0' && bytes_[position_] <= '9')
                ++position_;
        };
        digits();
        if (position_ < size_ && bytes_[position_] == '.') {
            ++position_;
            digits();
        }
        if (position_ < size_ && (bytes_[position_] == 'e' || bytes_[position_] == 'E')) {
            ++position_;
            if (position_ < size_ && (bytes_[position_] == '+' || bytes_[position_] == '-'))
                ++position_;
            digits();
        }
        const auto parsed = std::from_chars(bytes_ + first, bytes_ + position_, value);
        // Android strtod reported ERANGE for subnormal results in the former
        // decoder. Preserve that rejection while avoiding a temporary string.
        return parsed.ec == std::errc{} && parsed.ptr == bytes_ + position_ &&
               std::fpclassify(value) != FP_SUBNORMAL;
    }
    bool string(std::string_view& value) noexcept {
        if (!consume('"'))
            return false;
        const auto first = position_;
        // The common case needs no data movement at all.
        while (position_ < size_ && bytes_[position_] != '"' && bytes_[position_] != '\\')
            ++position_;
        std::size_t written = position_;
        const auto emit = [&](char c) noexcept {
            if constexpr (Decode)
                bytes_[written] = c;
            ++written;
        };
        for (;;) {
            if (position_ == size_)
                return false;
            char c = bytes_[position_++];
            if (c == '"') {
                if constexpr (Decode)
                    bytes_[written] = '\0';
                value = {bytes_ + first, written - first};
                return true;
            }
            if (c != '\\') {
                emit(c);
                continue;
            }
            if (position_ == size_)
                return false;
            c = bytes_[position_++];
            switch (c) {
            case '"':
            case '\\':
            case '/':
                emit(c);
                break;
            case 'b':
                emit('\b');
                break;
            case 'f':
                emit('\f');
                break;
            case 'n':
                emit('\n');
                break;
            case 'r':
                emit('\r');
                break;
            case 't':
                emit('\t');
                break;
            case 'u': {
                if (size_ - position_ < 4)
                    return false;
                unsigned code = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    const char digit = bytes_[position_++];
                    code <<= 4;
                    if (digit >= '0' && digit <= '9')
                        code += static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f')
                        code += static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F')
                        code += static_cast<unsigned>(digit - 'A' + 10);
                    else
                        return false;
                }
                // Preserve the established UTF-16-code-unit encoding of this protocol.
                if (code > 0x7ff)
                    emit(static_cast<char>(0xe0 | (code >> 12)));
                if (code > 0x7f)
                    emit(static_cast<char>((code > 0x7ff ? 0x80 : 0xc0) | ((code >> 6) & 0x3f)));
                emit(static_cast<char>(code <= 0x7f ? code : 0x80 | (code & 0x3f)));
                break;
            }
            default:
                return false;
            }
        }
    }
    bool skip(unsigned depth) noexcept {
        if (depth == 128)
            return false;
        const char token = peek();
        if (token == '"') {
            std::string_view ignored;
            return string(ignored);
        }
        if (token == '-' || (token >= '0' && token <= '9')) {
            double ignored;
            return number(ignored);
        }
        if (token == 't')
            return literal("true");
        if (token == 'f')
            return literal("false");
        if (token == 'n')
            return literal("null");
        if (token != '{' && token != '[')
            return false;
        ++position_;
        const char close = token == '{' ? '}' : ']';
        if (consume(close))
            return true;
        for (;;) {
            if (token == '{') {
                std::string_view ignored;
                if (!string(ignored) || !consume(':'))
                    return false;
            }
            if (!skip(depth + 1))
                return false;
            if (consume(close))
                return true;
            if (!consume(','))
                return false;
        }
    }

private:
    char* bytes_;
    std::size_t size_;
    std::size_t position_ = 0;
};

using Cursor = BasicCursor<true>;
using ValidationCursor = BasicCursor<false>;

}  // namespace kagami::json
