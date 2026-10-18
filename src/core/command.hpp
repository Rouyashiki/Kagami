#pragma once
#include "core/arguments.hpp"

#include <string>
#include <string_view>
#include <vector>
#include "core/output.hpp"

namespace kagami {

struct CommandResult {
    int exit_code = 0;
    int error_number = 0;
    std::string_view stdout_text;
    std::string_view stderr_text;
};

int run_command(output::Context& destination, Arguments args);
CommandResult run_command_capture(Arguments args, output::Context& destination);

}  // namespace kagami
