#pragma once

#include <string>

namespace kagami::logging {

enum class Level { Debug, Info, Warning, Error };

void set_debug_enabled(bool enabled) noexcept;
bool enabled(Level level) noexcept;
void write(Level level, const std::string &component, const std::string &message) noexcept;
bool prepare_boot_log(std::string &error, const std::string &boot_id = {});

// A wall-clock timestamp when Android has set the clock; otherwise an explicit
// early-boot marker based on CLOCK_BOOTTIME. This avoids presenting boot-time
// seconds as misleading dates in 1970.
std::string timestamp();

// Compatibility entry point for informational operation messages.
void append(const std::string &component, const std::string &message);

} // namespace kagami::logging
