#pragma once

#include "core/bounded.hpp"

#include <new>

namespace kagami {

// Borrowed terminated text. The caller guarantees data()[size()] == '\0';
// argv, literals, the in-place decoder and inventory storage satisfy this.
// Embedded NUL bytes retain the old string versus C-interface semantics.
struct Argument {
    const char* data;
    std::size_t length;
    Argument() noexcept = default;
    constexpr Argument(const char* value) noexcept
        : data(value), length(std::string_view(value).size()) {}
    constexpr explicit Argument(std::string_view value) noexcept
        : data(value.data()), length(value.size()) {}
    constexpr operator std::string_view() const noexcept { return {data, length}; }
    const char* c_str() const noexcept { return data; }
    bool empty() const noexcept { return length == 0; }
    std::size_t size() const noexcept { return length; }
    char front() const noexcept { return data[0]; }
    char back() const noexcept { return data[length - 1]; }
    template <typename T, std::enable_if_t<!std::is_same_v<T, Argument> &&
                                               std::is_convertible_v<T, std::string_view>,
                                           int> = 0>
    friend bool operator==(const T& left, Argument right) noexcept {
        return std::string_view(left) == std::string_view(right);
    }
    template <typename T, std::enable_if_t<!std::is_same_v<T, Argument> &&
                                               std::is_convertible_v<T, std::string_view>,
                                           int> = 0>
    friend bool operator!=(const T& left, Argument right) noexcept {
        return std::string_view(left) != std::string_view(right);
    }
    bool operator==(std::string_view other) const noexcept {
        return std::string_view(*this) == other;
    }
    bool operator!=(std::string_view other) const noexcept {
        return std::string_view(*this) != other;
    }
};

class Arguments {
public:
    constexpr Arguments() noexcept = default;
    constexpr Arguments(const Argument* data, std::size_t size) noexcept
        : data_(data), size_(size) {}
    template <std::size_t N>
    constexpr Arguments(const Argument (&data)[N]) noexcept : data_(data), size_(N) {}
    const Argument* begin() const noexcept { return data_; }
    const Argument* end() const noexcept { return size_ ? data_ + size_ : data_; }
    const Argument& operator[](std::size_t index) const noexcept { return data_[index]; }
    const Argument& front() const noexcept { return data_[0]; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    const Argument* data_ = nullptr;
    std::size_t size_ = 0;
};

class ArgumentStorage {
public:
    ArgumentStorage() noexcept = default;
    ~ArgumentStorage() { delete[] data_; }
    ArgumentStorage(const ArgumentStorage&) = delete;
    ArgumentStorage& operator=(const ArgumentStorage&) = delete;
    ArgumentStorage(ArgumentStorage&&) = delete;
    ArgumentStorage& operator=(ArgumentStorage&&) = delete;
    [[nodiscard]] bool prepare_arguments(std::size_t capacity) noexcept {
        constexpr std::size_t maximum = 65536;
        if (data_ || capacity > maximum ||
            capacity > std::numeric_limits<std::size_t>::max() / sizeof(Argument))
            return false;
        capacity_ = capacity ? capacity : 1;
        // Trivial default initialization starts the array lifetime without
        // clearing unused slots. append initializes every slot before exposure.
        data_ = new (std::nothrow) Argument[capacity_];
        return data_ != nullptr;
    }
    void reset() noexcept { size_ = 0; }
    [[nodiscard]] bool append(std::string_view value) noexcept {
        if (size_ == capacity_)
            return false;
        data_[size_++] = Argument(value);
        return true;
    }
    Arguments view() const noexcept { return {data_, size_}; }

private:
    Argument* data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

}  // namespace kagami
