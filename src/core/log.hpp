#pragma once

#include <string>

namespace kagami::logging {

// A wall-clock timestamp when Android has set the clock; otherwise an explicit
// early-boot marker based on CLOCK_BOOTTIME. This avoids presenting boot-time
// seconds as misleading dates in 1970.
std::string timestamp();

// Append one complete diagnostic line to daemon.log. This is deliberately
// independent of stderr redirection, so boot scripts and interactive commands
// contribute to the same log.
void append(const std::string& component, const std::string& message);

} // namespace kagami::logging
