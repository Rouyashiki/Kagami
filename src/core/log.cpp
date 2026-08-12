#include "core/log.hpp"

#include "core/runtime.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kagami::logging {

namespace fs = std::filesystem;

// Android's clock is commonly set after post-fs-data. Anything earlier than
// 2020 is an unset RTC, not a meaningful date for Kagami diagnostics.
static constexpr std::time_t kFirstPlausibleWallClock = 1577836800;

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    char buffer[48] = {};
    if (now >= kFirstPlausibleWallClock) {
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        return buffer;
    }

#if defined(CLOCK_BOOTTIME)
    timespec boot = {};
    if (clock_gettime(CLOCK_BOOTTIME, &boot) == 0) {
        std::snprintf(buffer, sizeof(buffer), "early-boot +%lld.%03ld",
                      static_cast<long long>(boot.tv_sec), boot.tv_nsec / 1000000L);
        return buffer;
    }
#endif
    return "early-boot (clock unset)";
}

void append(const std::string& component, const std::string& message) {
    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);

    std::string clean_message = message;
    for (char& c : clean_message) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    const std::string line = timestamp() + " [" + component + "] " + clean_message + "\n";

#if defined(__linux__) || defined(__APPLE__)
    const int fd = open(runtime_log_file().c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }
    // One append write keeps individual short entries intact when the boot
    // script and the daemon happen to emit at the same time.
    (void)write(fd, line.data(), line.size());
    close(fd);
#else
    (void)line;
#endif
}

} // namespace kagami::logging
