#pragma once

#include <string>
#include <string_view>

namespace kagami {

// The input must not refer to out: appending may reallocate its storage.
inline void append_json_escaped(std::string& out, std::string_view value) {
    std::size_t first = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char* escape = nullptr;
        switch (value[i]) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }
        if (escape) {
            out.append(value.data() + first, i - first);
            out.append(escape, 2);
            first = i + 1;
        }
    }
    if (first < value.size()) {
        out.append(value.data() + first, value.size() - first);
    }
}

inline void append_json_quoted(std::string& out, std::string_view value) {
    out.push_back('"');
    append_json_escaped(out, value);
    out.push_back('"');
}

inline std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    append_json_escaped(out, value);
    return out;
}

inline std::string json_quote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    append_json_quoted(out, value);
    return out;
}

}  // namespace kagami
