#include "core/flat_json.hpp"
#include "core/decimal.hpp"

#include <cerrno>

namespace kagami::json {

bool Document::prepare_document(std::string_view input, int& error) noexcept {
    error = 0;
    if (nodes_ || edges_ || input.size() > maximum_bytes) {
        error = input.size() > maximum_bytes ? EOVERFLOW : EINVAL;
        return false;
    }
    // Count lexical tokens before allocating. Keys are conservatively counted
    // too; a large string still requires one node rather than size/2 nodes.
    std::size_t count = 0;
    for (std::size_t i = 0; i < input.size();) {
        const char c = input[i++];
        if (c == '{' || c == '[') {
            ++count;
        } else if (c == '"') {
            ++count;
            while (i < input.size()) {
                const char next = input[i++];
                if (next == '"')
                    break;
                if (next == '\\' && i < input.size())
                    ++i;
            }
        } else if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
            ++count;
            while (i < input.size()) {
                const char next = input[i];
                if (next == ',' || next == ']' || next == '}' || next == ':' || next == ' ' ||
                    next == '\r' || next == '\n' || next == '\t' || next == '"' || next == '[' ||
                    next == '{')
                    break;
                ++i;
            }
        }
    }
    static_assert(maximum_nodes <= std::numeric_limits<std::size_t>::max() / sizeof(Node));
    if (count > maximum_nodes) {
        error = EOVERFLOW;
        return false;
    }
    capacity_ = static_cast<std::uint32_t>(std::max<std::size_t>(count, 1));
    nodes_ = new (std::nothrow) Node[capacity_];
    edges_ = new (std::nothrow) std::uint32_t[capacity_];
    if (!nodes_ || !edges_) {
        error = ENOMEM;
        return false;
    }
    return true;
}

bool Document::parse_value(Cursor& cursor, std::string_view key, unsigned depth,
                           int& error) noexcept {
    if (depth >= 128 || used_ == capacity_) {
        error = depth >= 128 ? EOVERFLOW : ENOBUFS;
        return false;
    }
    const auto index = used_++;
    auto& value = nodes_[index];
    value = {key.data(), key.size(), nullptr, 0, 0, 0, 0, 0, Type::Null, false};
    const char token = cursor.peek();
    if (token == '{' || token == '[') {
        const bool object = token == '{';
        value.type = object ? Type::Object : Type::Array;
        const char close = object ? '}' : ']';
        if (!cursor.consume(token))
            return false;
        if (!cursor.consume(close)) {
            for (;;) {
                std::string_view child_key;
                if (object && (!cursor.string(child_key) || !cursor.consume(':')))
                    return false;
                if (!parse_value(cursor, child_key, depth + 1, error))
                    return false;
                ++value.count;
                if (cursor.consume(close))
                    break;
                if (!cursor.consume(','))
                    return false;
            }
        }
        if (value.count > capacity_ - edges_used_) {
            error = ENOBUFS;
            return false;
        }
        value.children = edges_used_;
        for (auto child = index + 1; child < used_; child = nodes_[child].next)
            edges_[edges_used_++] = child;
        if (object && value.count > 1) {
            auto* first = edges_ + value.children;
            std::sort(first, first + value.count, [&](auto lhs, auto rhs) noexcept {
                return nodes_[lhs].key() < nodes_[rhs].key() ||
                       (nodes_[lhs].key() == nodes_[rhs].key() && lhs < rhs);
            });
            std::uint32_t unique = 0;
            for (std::uint32_t i = 0; i < value.count; ++i) {
                if (i + 1 == value.count || nodes_[first[i]].key() != nodes_[first[i + 1]].key())
                    first[unique++] = first[i];
            }
            value.count = unique;
        }
    } else if (token == '"') {
        value.type = Type::String;
        std::string_view text;
        if (!cursor.string(text))
            return false;
        value.text_data = text.data();
        value.text_size = text.size();
    } else if (token == '-' || (token >= '0' && token <= '9')) {
        value.type = Type::Number;
        if (!cursor.number(value.number))
            return false;
    } else if (token == 't' || token == 'f') {
        value.type = Type::Bool;
        value.boolean = token == 't';
        if (!cursor.literal(value.boolean ? "true" : "false"))
            return false;
    } else if (token != 'n' || !cursor.literal("null")) {
        return false;
    }
    value.next = used_;
    return true;
}

bool Document::parse(char* input, std::size_t size, int& error) noexcept {
    used_ = edges_used_ = 0;
    valid_ = false;
    error = 0;
    if (!nodes_ || !edges_ || size > maximum_bytes) {
        error = EINVAL;
        return false;
    }
    Cursor cursor(input, size);
    valid_ = parse_value(cursor, {}, 0, error) && cursor.end();
    if (!valid_ && !error)
        error = EINVAL;
    return valid_;
}

bool write_quoted(bounded::Writer& output, std::string_view text) noexcept {
    return output.append('"') && bounded::escape(output, text, true) && output.append('"');
}
bool write_whitespace(bounded::Writer& output, unsigned count) noexcept {
    constexpr std::string_view spaces = "                                ";
    if (!output.append('\n') || count > output.remaining())
        return false;
    while (count) {
        const auto chunk = std::min<std::size_t>(count, spaces.size());
        if (!output.append(spaces.substr(0, chunk)))
            return false;
        count -= static_cast<unsigned>(chunk);
    }
    return true;
}
namespace {
bool number(bounded::Writer& output, double value) noexcept {
    return decimal::write(output, value);
}
bool write_edit(bounded::Writer& output, const Edit& edit, unsigned indent) noexcept {
    if (edit.value)
        return write_value(output, edit.value, indent, 2);
    switch (edit.type) {
    case Type::String:
        return write_quoted(output, edit.text);
    case Type::Bool:
        return output.append(edit.boolean ? "true" : "false");
    case Type::Number:
        return number(output, edit.number);
    case Type::Null:
        return output.append("null");
    default:
        return false;
    }
}
}  // namespace

bool write_value(bounded::Writer& output, Value value, unsigned indent, unsigned depth) noexcept {
    if (!value || depth > 128 || indent > 16)
        return false;
    if (value.is_string())
        return write_quoted(output, value.string_or());
    if (value.is_number())
        return number(output, value.number_or(0));
    if (value.is_bool())
        return output.append(value.bool_or(false) ? "true" : "false");
    if (value.is_null())
        return output.append("null");
    const bool object = value.is_object();
    if (!output.append(object ? '{' : '['))
        return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto child = value.child(i);
        if ((i && !output.append(',')) || (indent && !write_whitespace(output, indent * depth)) ||
            (object &&
             (!write_quoted(output, child.key()) || !output.append(indent ? ": " : ":"))) ||
            !write_value(output, child, indent, depth + 1))
            return false;
    }
    return (!indent || !value.size() || write_whitespace(output, indent * (depth - 1))) &&
           output.append(object ? '}' : ']');
}

bool ObjectWriter::replace(Edit edit) noexcept {
    std::size_t i = 0;
    while (i < size_ && edits_[i].key < edit.key)
        ++i;
    if (i == size_ || edits_[i].key != edit.key) {
        if (size_ == std::size(edits_))
            return false;
        for (auto j = size_; j > i; --j)
            edits_[j] = edits_[j - 1];
        ++size_;
    }
    edits_[i] = edit;
    return true;
}

Value ObjectWriter::find(std::string_view key) const noexcept {
    for (std::size_t i = 0; i < size_; ++i)
        if (edits_[i].key == key)
            return edits_[i].removed ? Value{} : edits_[i].value;
    const auto patched = patch_.find(key);
    return patched ? patched : original_.find(key);
}

bool write_entry(bounded::Writer& output, Value value, const Edit* edit, unsigned indent) noexcept {
    return edit ? write_edit(output, *edit, indent) : write_value(output, value, indent, 2);
}
bool ObjectWriter::write(bounded::Writer& output, unsigned indent) const noexcept {
    return write_each(
        output,
        [](bounded::Writer& out, std::string_view, Value value, const Edit* edit,
           unsigned indentation) noexcept { return write_entry(out, value, edit, indentation); },
        indent);
}

}  // namespace kagami::json
