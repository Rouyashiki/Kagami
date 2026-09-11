#include "core/command.hpp"
#include "core/daemon.hpp"
#include "core/log.hpp"

#include <string>
#include <vector>

namespace {

bool runs_in_controller(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h" ||
        args[0] == "version" || args[0] == "--version" || args[0] == "daemon" || args[0] == "prepare") {
        return true;
    }
    // post-fs-data may need to create the initial config before service.sh has
    // started. This command does not touch Kasumi and is safe in the controller.
    return args[0] == "config" && args.size() > 1 && args[1] == "gen";
}

bool daemon_can_autostart(const std::vector<std::string>& args) {
    return std::getenv("KAGAMI_ALLOW_DAEMON_START") != nullptr ||
           (args.size() > 1 && args[0] == "daemon" && args[1] == "start");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    if (!runs_in_controller(args) && daemon_can_autostart(args)) {
        const int start_code = kagami::run_daemon_command({"daemon", "start"});
        if (start_code != 0) {
            return start_code;
        }
    }
    const int exit_code = runs_in_controller(args) ? kagami::run_command(args)
                                                   : kagami::run_via_daemon(args, false);
    return exit_code;
}
