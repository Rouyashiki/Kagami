#pragma once

#include <sstream>
#include <string>

namespace kagami {

inline std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (char c : value) {
        switch (c) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << c;
            break;
        }
    }
    return out.str();
}

inline std::string json_quote(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

}  // namespace kagami
