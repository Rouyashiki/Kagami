#pragma once

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace kagami::bounded {

// Storage is acquired only at the explicit preparation boundary. No growth,
// fallback allocator, copying, or implicit initialization of the data block.
class Storage {
public:
    Storage() noexcept = default;
    ~Storage() { std::free(data_); }
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}
    Storage& operator=(Storage&& other) noexcept {
        if (this != &other) {
            std::free(data_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }
    [[nodiscard]] bool prepare_storage(std::size_t bytes, std::size_t maximum) noexcept {
        if (!bytes || bytes > maximum || data_)
            return false;
        data_ = static_cast<char*>(std::malloc(bytes));
        if (!data_)
            return false;
        size_ = bytes;
        return true;
    }
    char* data() noexcept { return data_; }
    const char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    char* data_ = nullptr;
    std::size_t size_ = 0;
};

// Non-owning output cursor. The caller retains storage for the entire write.
// There is no terminator contract: POSIX/JSON consumers receive the length.
class Writer {
public:
    Writer(char* data, std::size_t capacity) noexcept : data_(data), capacity_(capacity) {}
    void reset() noexcept { size_ = 0; }
    std::string_view view() const noexcept { return {data_, size_}; }
    std::size_t remaining() const noexcept { return capacity_ - size_; }
    char* tail() noexcept { return data_ + size_; }
    [[nodiscard]] bool commit(std::size_t count) noexcept {
        if (count > remaining())
            return false;
        size_ += count;
        return true;
    }
    [[nodiscard]] bool append(std::string_view value) noexcept {
        if (value.size() > remaining())
            return false;
        if (!value.empty())
            std::memcpy(tail(), value.data(), value.size());
        size_ += value.size();
        return true;
    }
    [[nodiscard]] bool append(const char* value) noexcept {
        return append(std::string_view(value));
    }
    [[nodiscard]] bool append(char value) noexcept {
        if (!remaining())
            return false;
        data_[size_++] = value;
        return true;
    }
    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char> &&
                                               !std::is_same_v<T, bool>,
                                           int> = 0>
    [[nodiscard]] bool append(T value) noexcept {
        const auto result = std::to_chars(tail(), data_ + capacity_, value);
        if (result.ec != std::errc{})
            return false;
        size_ = static_cast<std::size_t>(result.ptr - data_);
        return true;
    }
    [[nodiscard]] bool append(bool value) noexcept { return append(value ? '1' : '0'); }
    template <typename... T>
    [[nodiscard]] bool write(const T&... values) noexcept {
        return (append(values) && ...);
    }

private:
    char* data_;
    std::size_t capacity_;
    std::size_t size_ = 0;
};

struct Quoted {
    std::string_view value;
    bool all_controls = false;
};

inline Quoted quoted(std::string_view value) noexcept {
    return {value};
}

inline Quoted json_quoted(std::string_view value) noexcept {
    return {value, true};
}

// Match the established wire escaping exactly, including raw control bytes.
template <typename Sink>
[[nodiscard]] bool escape(Sink& sink, std::string_view value, bool all_controls = false) noexcept {
    if (value.empty())
        return true;
    std::size_t first = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char* escaped = nullptr;
        switch (value[i]) {
        case '"':
            escaped = "\\\"";
            break;
        case '\\':
            escaped = "\\\\";
            break;
        case '\n':
            escaped = "\\n";
            break;
        case '\r':
            escaped = "\\r";
            break;
        case '\t':
            escaped = "\\t";
            break;
        case '\b':
            if (all_controls)
                escaped = "\\b";
            break;
        case '\f':
            if (all_controls)
                escaped = "\\f";
            break;
        default:
            break;
        }
        if (escaped) {
            if (!sink.append(std::string_view(value.data() + first, i - first)) ||
                !sink.append({escaped, 2}))
                return false;
            first = i + 1;
        } else if (all_controls && static_cast<unsigned char>(value[i]) < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            const auto byte = static_cast<unsigned char>(value[i]);
            const char encoded[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
            if (!sink.append(std::string_view(value.data() + first, i - first)) ||
                !sink.append({encoded, sizeof(encoded)}))
                return false;
            first = i + 1;
        }
    }
    return sink.append(std::string_view(value.data() + first, value.size() - first));
}

}  // namespace kagami::bounded
