#include "core/command.hpp"
#include "core/daemon.hpp"
#include "core/log.hpp"

#include <string>
#include <vector>

namespace {

std::string mutable_command_name(const std::vector<std::string>& args) {
    if (args.empty()) {
        return "";
    }
    const std::string& group = args[0];
    const std::string sub = args.size() > 1 ? args[1] : "";

    if (group == "debug") {
        return "debug " + sub;
    }
    if (group == "config" && sub != "show") {
        return "config " + sub;
    }
    if (group == "module" && sub != "list" && sub != "check-conflicts") {
        return "module " + sub;
    }
    if (group == "kasumi" && sub != "version" && sub != "list") {
        return "kasumi " + sub;
    }
    if (group == "lkm" && sub != "status") {
        return "lkm " + sub;
    }
    if (group == "hide" && sub != "list") {
        return "hide " + sub;
    }
    if (group == "recovery" && sub != "status") {
        return "recovery " + sub;
    }
    if (group == "daemon" && sub != "status" && sub != "ping" && sub != "call") {
        return "daemon " + sub;
    }
    return "";
}

bool runs_in_controller(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h" ||
        args[0] == "version" || args[0] == "--version" || args[0] == "daemon") {
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
    const std::string command = mutable_command_name(args);
    if (!command.empty()) {
        kagami::logging::append("command", command + " exit=" + std::to_string(exit_code));
    }
    return exit_code;
}
