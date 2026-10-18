#pragma once

#include "core/arguments.hpp"
#include "core/json_cursor.hpp"

#include <cmath>
#include <limits>

namespace kagami::daemon_protocol {

struct Response {
    int exit_code = 0;
    std::string_view output, error;
};

// A dedicated wire decoder. Strings without escapes stay where they arrived;
// escaped strings shrink in place. No DOM, allocation, or resource ownership.
class WireJson : private json::Cursor {
public:
    WireJson(char* bytes, std::size_t size) noexcept : json::Cursor(bytes, size) {}

    [[nodiscard]] bool request(ArgumentStorage& args) noexcept {
        args.reset();
        if (!consume('['))
            return false;
        if (consume(']'))
            return end();
        for (;;) {
            std::string_view value;
            if (!string(value) || !args.append(value))
                return false;
            if (consume(']'))
                return end();
            if (!consume(','))
                return false;
        }
    }

    [[nodiscard]] bool response(Response& result) noexcept {
        bool code = false, out = false, err = false;
        if (!consume('{'))
            return false;
        if (!consume('}')) {
            for (;;) {
                std::string_view key;
                if (!string(key) || !consume(':'))
                    return false;
                if (key == "stdout" || key == "stderr") {
                    std::string_view value;
                    const bool valid = peek() == '"';
                    if (valid ? !string(value) : !skip(0))
                        return false;
                    if (key == "stdout") {
                        out = valid;
                        result.output = value;
                    } else {
                        err = valid;
                        result.error = value;
                    }
                } else if (key == "exit_code") {
                    double value = 0;
                    const char next = peek();
                    code = next == '-' || (next >= '0' && next <= '9');
                    if (code ? !number(value) : !skip(0))
                        return false;
                    if (code) {
                        // Avoid the old undefined out-of-range double-to-int cast.
                        if (!std::isfinite(value) || value < std::numeric_limits<int>::min() ||
                            value > std::numeric_limits<int>::max())
                            return false;
                        result.exit_code = static_cast<int>(value);
                    }
                } else if (!skip(0)) {
                    return false;
                }
                if (consume('}'))
                    break;
                if (!consume(','))
                    return false;
            }
        }
        return end() && code && out && err;
    }
};

}  // namespace kagami::daemon_protocol
