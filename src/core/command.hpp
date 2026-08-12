#pragma once

#include <string>
#include <vector>

namespace kagami {

struct CommandResult {
    int exit_code = 0;
    int error_number = 0;
    std::string stdout_text;
    std::string stderr_text;
};

int run_command(const std::vector<std::string>& args);
CommandResult run_command_capture(const std::vector<std::string>& args);

} // namespace kagami
