#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kagami {

class JsonValue {
public:
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool bool_value = false;
    double number_value = 0;
    std::string string_value;
    std::vector<JsonValue> array_value;
    std::map<std::string, JsonValue> object_value;

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    const JsonValue* find(const std::string& key) const;
    std::string string_or(const std::string& fallback) const;
    bool bool_or(bool fallback) const;
    std::uint32_t u32_or(std::uint32_t fallback) const;
};

bool parse_json(const std::string& input, JsonValue& out, std::string& error);
std::string stringify_json(const JsonValue& value, int indent = 0);

} // namespace kagami
