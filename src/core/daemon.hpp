#pragma once
#include "core/arguments.hpp"
#include "core/output.hpp"

#include <string>
#include <vector>

namespace kagami {

int run_daemon_command(output::Context& destination, Arguments args);
int run_via_daemon(output::Context& destination, Arguments args, bool allow_start = true);

}  // namespace kagami
