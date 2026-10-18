#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace kagami::bounded {

template <typename T>
class Span {
public:
    constexpr Span() noexcept = default;
    constexpr Span(const T* data, std::size_t size) noexcept : data_(data), size_(size) {}
    template <std::size_t N>
    constexpr Span(const T (&data)[N]) noexcept : data_(data), size_(N) {}
    const T* data() const noexcept { return data_; }
    const T* begin() const noexcept { return data_; }
    const T* end() const noexcept { return size_ ? data_ + size_ : data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return !size_; }
    const T& operator[](std::size_t index) const noexcept { return data_[index]; }

private:
    const T* data_ = nullptr;
    std::size_t size_ = 0;
};

// Trivial slots only: append writes scalars or borrowed views, never resources.
// Capacity is explicit and immutable; no constructors, growth, or fallback can
// allocate. Unused slots are not initialized or exposed to readers.
template <typename T>
class Array {
    static_assert(std::is_trivially_default_constructible_v<T>);
    static_assert(std::is_trivially_copyable_v<T>);

public:
    Array() noexcept = default;
    ~Array() { delete[] data_; }
    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;
    Array(Array&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          capacity_(std::exchange(other.capacity_, 0)),
          size_(std::exchange(other.size_, 0)) {}
    Array& operator=(Array&& other) noexcept {
        if (this != &other) {
            delete[] data_;
            data_ = std::exchange(other.data_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }
    [[nodiscard]] bool prepare_array(std::size_t capacity, std::size_t maximum) noexcept {
        if (data_ || capacity > maximum ||
            capacity > std::numeric_limits<std::size_t>::max() / sizeof(T))
            return false;
        if (!capacity)
            return true;
        data_ = new (std::nothrow) T[capacity];
        if (!data_)
            return false;
        capacity_ = capacity;
        return true;
    }
    [[nodiscard]] bool append(T value) noexcept {
        if (size_ == capacity_)
            return false;
        data_[size_++] = value;
        return true;
    }
    void reset() noexcept { size_ = 0; }
    Span<T> view() const noexcept { return {data_, size_}; }
    T* data() noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    T& operator[](std::size_t index) noexcept { return data_[index]; }

private:
    T* data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

}  // namespace kagami::bounded
