#pragma once

#include "core/bounded.hpp"
#include "core/json_cursor.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace kagami::json {

enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

// No node owns resources. Each edge is an index into one prepared array. Object
// edges are sorted and deduplicated once during parsing (last input key wins).
struct Node {
    const char* key_data;
    std::size_t key_size;
    const char* text_data;
    std::size_t text_size;
    double number;
    std::uint32_t children;
    std::uint32_t count;
    std::uint32_t next;
    Type type;
    bool boolean;
    std::string_view key() const noexcept { return {key_data, key_size}; }
    std::string_view text() const noexcept { return {text_data, text_size}; }
};

class Value {
public:
    Value() noexcept = default;
    Value(const Node* nodes, const std::uint32_t* edges, std::uint32_t index) noexcept
        : nodes_(nodes), edges_(edges), index_(index) {}
    explicit operator bool() const noexcept { return nodes_ != nullptr; }
    bool is_null() const noexcept { return nodes_ && node().type == Type::Null; }
    bool is_bool() const noexcept { return nodes_ && node().type == Type::Bool; }
    bool is_number() const noexcept { return nodes_ && node().type == Type::Number; }
    bool is_string() const noexcept { return nodes_ && node().type == Type::String; }
    bool is_array() const noexcept { return nodes_ && node().type == Type::Array; }
    bool is_object() const noexcept { return nodes_ && node().type == Type::Object; }
    bool bool_or(bool fallback) const noexcept { return is_bool() ? node().boolean : fallback; }
    double number_or(double fallback) const noexcept {
        return is_number() ? node().number : fallback;
    }
    std::uint32_t u32_or(std::uint32_t fallback) const noexcept {
        return is_number() && node().number >= 0 &&
                       node().number <= std::numeric_limits<std::uint32_t>::max()
                   ? static_cast<std::uint32_t>(node().number)
                   : fallback;
    }
    std::string_view string_or(std::string_view fallback = {}) const noexcept {
        return is_string() ? node().text() : fallback;
    }
    std::string_view key() const noexcept { return nodes_ ? node().key() : std::string_view{}; }
    std::size_t size() const noexcept { return nodes_ ? node().count : 0; }
    Value child(std::size_t index) const noexcept {
        return nodes_ && index < size() ? Value(nodes_, edges_, edges_[node().children + index])
                                        : Value{};
    }
    Value find(std::string_view key) const noexcept {
        if (!is_object())
            return {};
        std::size_t low = 0, high = size();
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (child(middle).key() < key)
                low = middle + 1;
            else
                high = middle;
        }
        return low < size() && child(low).key() == key ? child(low) : Value{};
    }

private:
    const Node& node() const noexcept { return nodes_[index_]; }
    const Node* nodes_ = nullptr;
    const std::uint32_t* edges_ = nullptr;
    std::uint32_t index_ = 0;
};

// Owns only the token/edge arrays. Input remains owned by the caller. Allocation
// and input validation are separate from traversal/serialization, which cannot
// grow storage or invoke a fallback allocator.
class Document {
public:
    static constexpr std::size_t maximum_bytes = 1024 * 1024;
    static constexpr std::size_t maximum_nodes = maximum_bytes / 2 + 1;
    Document() noexcept = default;
    ~Document() {
        delete[] nodes_;
        delete[] edges_;
    }
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept
        : nodes_(std::exchange(other.nodes_, nullptr)),
          edges_(std::exchange(other.edges_, nullptr)),
          capacity_(std::exchange(other.capacity_, 0)),
          used_(std::exchange(other.used_, 0)),
          edges_used_(std::exchange(other.edges_used_, 0)),
          valid_(std::exchange(other.valid_, false)) {}
    Document& operator=(Document&& other) noexcept {
        if (this != &other) {
            Document old(std::move(*this));
            nodes_ = std::exchange(other.nodes_, nullptr);
            edges_ = std::exchange(other.edges_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0);
            used_ = std::exchange(other.used_, 0);
            edges_used_ = std::exchange(other.edges_used_, 0);
            valid_ = std::exchange(other.valid_, false);
        }
        return *this;
    }
    [[nodiscard]] bool prepare_document(std::string_view input, int& error) noexcept;
    [[nodiscard]] bool parse(char* input, std::size_t size, int& error) noexcept;
    Value root() const noexcept { return valid_ ? Value(nodes_, edges_, 0) : Value{}; }

private:
    [[nodiscard]] bool parse_value(Cursor& cursor, std::string_view key, unsigned depth,
                                   int& error) noexcept;
    Node* nodes_ = nullptr;
    std::uint32_t* edges_ = nullptr;
    std::uint32_t capacity_ = 0;
    std::uint32_t used_ = 0;
    std::uint32_t edges_used_ = 0;
    bool valid_ = false;
};

[[nodiscard]] bool write_value(bounded::Writer& output, Value value, unsigned indent = 0,
                               unsigned depth = 1) noexcept;

// A merge is a small journal of borrowed subtree references or scalar values;
// no existing key, string or subtree is copied to apply a patch.
struct Edit {
    std::string_view key;
    Value value;
    std::string_view text = {};
    Type type = Type::Null;
    double number = 0;
    bool boolean = false;
    bool removed = false;
};
[[nodiscard]] bool write_quoted(bounded::Writer& output, std::string_view text) noexcept;
[[nodiscard]] bool write_whitespace(bounded::Writer& output, unsigned count) noexcept;
[[nodiscard]] bool write_entry(bounded::Writer& output, Value value, const Edit* edit,
                               unsigned indent) noexcept;
class ObjectWriter {
public:
    ObjectWriter(Value original, Value patch = {}) noexcept : original_(original), patch_(patch) {}
    [[nodiscard]] bool replace(Edit edit) noexcept;
    [[nodiscard]] bool write(bounded::Writer& output, unsigned indent = 2) const noexcept;
    template <typename WriteField>
    [[nodiscard]] bool write_each(bounded::Writer& output, const WriteField& write_field,
                                  unsigned indent = 2) const noexcept {
        if (!original_.is_object() || (patch_ && !patch_.is_object()) || indent > 16 ||
            !output.append('{'))
            return false;
        std::size_t original = 0;
        std::size_t patch = 0;
        std::size_t edit = 0;
        bool first = true;
        while (original < original_.size() || patch < patch_.size() || edit < size_) {
            const auto from_original = original_.child(original);
            const auto from_patch = patch_.child(patch);
            std::string_view key;
            if (from_original)
                key = from_original.key();
            else if (from_patch)
                key = from_patch.key();
            else
                key = edits_[edit].key;
            if (from_patch && from_patch.key() < key)
                key = from_patch.key();
            if (edit < size_ && edits_[edit].key < key)
                key = edits_[edit].key;
            Value selected;
            if (from_original && from_original.key() == key) {
                selected = from_original;
                ++original;
            }
            if (from_patch && from_patch.key() == key) {
                selected = from_patch;
                ++patch;
            }
            const Edit* override = nullptr;
            if (edit < size_ && edits_[edit].key == key)
                override = &edits_[edit++];
            if (override && override->removed)
                continue;
            if ((!first && !output.append(',')) || (indent && !write_whitespace(output, indent)) ||
                !write_quoted(output, key) || !output.append(indent ? ": " : ":") ||
                !write_field(output, key, selected, override, indent))
                return false;
            first = false;
        }
        return (!indent || first || write_whitespace(output, 0)) && output.append('}');
    }
    Value find(std::string_view key) const noexcept;

private:
    Value original_;
    Value patch_;
    // Only the fixed schema migrations use this journal. Arbitrarily many
    // user-supplied patch keys are traversed directly from the parsed document.
    Edit edits_[16];
    std::size_t size_ = 0;
};

}  // namespace kagami::json
