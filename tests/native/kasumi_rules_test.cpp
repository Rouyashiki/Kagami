#include "kagami/kasumi_client.hpp"
#include "kagami/kasumi_uapi.h"

#include <cerrno>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Case {
    std::string name;
    std::string input;
    bool ok = false;
    bool enabled = false;
    int error = EPROTO;
};

}  // namespace

int main() {
    const std::string protocol = "Kasumi Protocol: " + std::to_string(KSM_PROTOCOL_VERSION) + "\n";
    const std::string enabled = protocol + "Kasumi Enabled: 1\n";
    const std::string disabled = protocol + "Kasumi Enabled: 0\n";
    const std::string rule = "ADD /system/etc/example <- /data/adb/modules/example/file\n";

    std::vector<Case> cases = {
        {"enabled empty list", enabled, true, true, 0},
        {"disabled empty list", disabled, true, false, 0},
        {"enabled list preserves rule text", enabled + rule, true, true, 0},
        {"disabled list preserves rule text", disabled + rule, true, false, 0},
        {"empty response", ""},
        {"missing protocol header", "Kasumi Enabled: 1\n"},
        {"wrong protocol header",
         "Kasumi Version: " + std::to_string(KSM_PROTOCOL_VERSION) + "\nKasumi Enabled: 1\n"},
        {"old protocol",
         "Kasumi Protocol: " + std::to_string(KSM_PROTOCOL_VERSION - 1) + "\nKasumi Enabled: 1\n"},
        {"new protocol",
         "Kasumi Protocol: " + std::to_string(KSM_PROTOCOL_VERSION + 1) + "\nKasumi Enabled: 1\n"},
        {"missing enabled header", protocol},
        {"wrong enabled header", protocol + "Kasumi State: 1\n"},
        {"empty enabled value", protocol + "Kasumi Enabled: \n"},
        {"invalid enabled value", protocol + "Kasumi Enabled: 2\n"},
        {"enabled value has suffix", protocol + "Kasumi Enabled: 10\n"},
        {"reordered headers", "Kasumi Enabled: 1\n" + protocol},
        {"missing header newline", enabled.substr(0, enabled.size() - 1)},
        {"missing rule newline", enabled + rule.substr(0, rule.size() - 1)},
        {"CRLF enabled header", protocol + "Kasumi Enabled: 1\r\n"},
        {"NUL in rule body", enabled + std::string("ADD /bad\0path\n", 14)},
        {"NUL after complete response", enabled + std::string(1, '\0')},
    };

    // LIST_RULES can saturate its 64 KiB scnprintf buffer at 65535 bytes.
    // A final newline alone cannot distinguish saturation from a complete list.
    constexpr std::size_t capacity = 64U * 1024;
    const auto sized_response = [&enabled](std::size_t size) {
        return enabled + std::string(size - enabled.size() - 1, 'x') + '\n';
    };
    cases.push_back({"below saturation", sized_response(capacity - 2), true, true, 0});
    cases.push_back({"saturated response", sized_response(capacity - 1), false, false, EOVERFLOW});
    cases.push_back({"64 KiB response", sized_response(capacity), false, false, EOVERFLOW});
    cases.push_back({"oversized response", sized_response(capacity + 1), false, false, EOVERFLOW});

    std::size_t failed = 0;
    for (const auto& test : cases) {
        const auto actual = kagami::kasumi::decode_active_rules(test.input);
        if (actual.ok != test.ok || actual.enabled != test.enabled ||
            actual.last_errno != test.error || actual.text != (test.ok ? test.input : "")) {
            std::cerr << "FAIL: " << test.name << " (ok=" << actual.ok
                      << ", enabled=" << actual.enabled << ", errno=" << actual.last_errno
                      << ", text size=" << actual.text.size() << ")\n";
            ++failed;
        }
    }
    std::cout << "Kasumi strict rule parsing: " << cases.size() - failed << '/' << cases.size()
              << " passed\n";
    return failed == 0 ? 0 : 1;
}
