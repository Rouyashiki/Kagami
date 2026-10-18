#include "core/command.hpp"
#include "core/daemon.hpp"
#include "core/log.hpp"

#include <cstdio>

using kagami::Arguments;

namespace {

bool runs_in_controller(Arguments args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h" ||
        args[0] == "version" || args[0] == "--version" || args[0] == "daemon" ||
        args[0] == "prepare") {
        return true;
    }
    // post-fs-data may need to create the initial config before service.sh has
    // started. This command does not touch Kasumi and is safe in the controller.
    return args[0] == "config" && args.size() > 1 && args[1] == "gen";
}

bool daemon_can_autostart(Arguments args) {
    return std::getenv("KAGAMI_ALLOW_DAEMON_START") != nullptr ||
           (args.size() > 1 && args[0] == "daemon" && args[1] == "start");
}

}  // namespace

int main(int argc, char** argv) {
    kagami::bounded::Storage output_storage;
    const std::size_t out_capacity =
        argc > 1 && std::string_view(argv[1]) == "daemon" ? 1024UL * 1024 : 8192;
    constexpr std::size_t err_capacity = 64UL * 1024;
    if (!output_storage.prepare_storage(out_capacity + err_capacity, out_capacity + err_capacity)) {
        (void)std::fputs("Kagami: cannot prepare command output storage\n", stderr);
        return 1;
    }
    kagami::output::Context destination{{output_storage.data(), out_capacity},
                                        {output_storage.data() + out_capacity, err_capacity}};
    kagami::ArgumentStorage arguments;
    if (!arguments.prepare_arguments(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0)) {
        (void)std::fputs("Kagami: cannot prepare argument storage\n", stderr);
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        if (!arguments.append(argv[i])) {
            (void)std::fputs("Kagami: too many arguments\n", stderr);
            return 1;
        }
    }
    const auto args = arguments.view();
    if (!runs_in_controller(args) && daemon_can_autostart(args)) {
        constexpr kagami::Argument start_args[] = {"daemon", "start"};
        const int start_code = kagami::run_daemon_command(destination, start_args);
        // Publish controller output before the forwarded response, which writes
        // directly to the final descriptors. Flush errors on start failure too.
        const bool out_ok = kagami::output::write_fd(STDOUT_FILENO, destination.out.view());
        const bool err_ok = kagami::output::write_fd(STDERR_FILENO, destination.err.view());
        if (destination.error || !out_ok || !err_ok)
            return 1;
        destination.out.reset();
        destination.err.reset();
        if (start_code != 0) {
            return start_code;
        }
    }
    const int exit_code = runs_in_controller(args)
                              ? kagami::run_command(destination, args)
                              : kagami::run_via_daemon(destination, args, false);
    if (destination.error) {
        (void)std::fputs("Kagami: command output exceeds prepared capacity\n", stderr);
        return 1;
    }
    const bool out_ok = kagami::output::write_fd(STDOUT_FILENO, destination.out.view());
    const bool err_ok = kagami::output::write_fd(STDERR_FILENO, destination.err.view());
    return out_ok && err_ok ? exit_code : 1;
}
