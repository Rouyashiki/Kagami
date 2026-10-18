#include "core/daemon.hpp"
#include "core/socket_input.hpp"
#include "core/socket_output.hpp"
#include "core/wire_json.hpp"

#include "core/command.hpp"
#include "core/json.hpp"
#include "core/json_value.hpp"
#include "core/lkm.hpp"
#include "core/log.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "core/output.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace kagami {

namespace fs = std::filesystem;

namespace {
int print_status_json(output::Context& destination);
using daemon_protocol::Message;
using daemon_protocol::send_response;

void append_log(const std::string& message, logging::Level level = logging::Level::Info) {
    logging::write(level, "daemon", message);
}

std::string status_json(bool running) {
    const auto pid_text = [&]() -> std::string {
        std::ifstream in(runtime_pid_file());
        std::string line;
        return std::getline(in, line) ? line : "";
    }();

    std::ostringstream out;
    out << "{"
        << "\"running\":" << (running ? "true" : "false") << ","
        << "\"data_dir\":" << json_quote(runtime_data_dir().string()) << ","
        << "\"config_file\":" << json_quote(runtime_config_file().string()) << ","
        << "\"socket\":" << json_quote(runtime_socket_file().string()) << ","
        << "\"pid_file\":" << json_quote(runtime_pid_file().string()) << ","
        << "\"pid\":" << json_quote(pid_text) << ","
        << "\"log_file\":" << json_quote(runtime_log_file().string()) << "}\n";
    return out.str();
}
}  // namespace

#if defined(__linux__) || defined(__APPLE__)
namespace {

int open_client_socket(std::string& error) {
    const auto path = runtime_socket_file().string();
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        error = "socket path is too long: " + path;
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

bool send_arguments(int fd, Arguments args, std::size_t start, bool legacy) noexcept {
    char bytes[4096];
    daemon_protocol::SocketOutput wire(fd, bytes, sizeof(bytes));
    if (!legacy && !wire.append("["))
        return false;
    for (std::size_t i = start; i < args.size(); ++i) {
        if (i != start && !wire.append(legacy ? "\t" : ","))
            return false;
        if (legacy) {
            if (!wire.append(std::string_view(args[i])))
                return false;
        } else if (!wire.append("\"") || !bounded::escape(wire, std::string_view(args[i])) ||
                   !wire.append("\"")) {
            return false;
        }
    }
    return wire.append(legacy ? "\n" : "]\n") && wire.flush();
}

int send_request(Arguments args, std::size_t start, Message& response, std::string& error) {
    if (!response.prepare_message(daemon_protocol::response_limit)) {
        error = "cannot prepare response storage";
        return 1;
    }
    const int fd = open_client_socket(error);
    if (fd < 0)
        return 1;
    if (!send_arguments(fd, args, start, false)) {
        error = std::strerror(errno);
        close(fd);
        return 1;
    }
    shutdown(fd, SHUT_WR);
    const bool complete = response.read_from(fd);
    close(fd);
    return complete ? 0 : 1;
}

bool send_legacy_request(Arguments args, Message& response) {
    if (!response.prepare_message(daemon_protocol::response_limit))
        return false;
    std::string error;
    const int fd = open_client_socket(error);
    if (fd < 0)
        return false;
    if (!send_arguments(fd, args, 0, true)) {
        close(fd);
        return false;
    }
    shutdown(fd, SHUT_WR);
    const bool complete = response.read_from(fd);
    close(fd);
    return complete && response.size() != 0;
}

bool daemon_running() {
    constexpr Argument ping[] = {"daemon", "ping"};
    Message response;
    std::string error;
    return send_request(ping, 0, response, error) == 0 &&
           response.view().find("\"ok\":true") != std::string::npos;
}

bool stop_legacy_daemon() {
    Message response;

    constexpr Argument ping[] = {"daemon", "ping"};
    constexpr Argument stop[] = {"daemon", "stop"};
    if (!send_legacy_request(ping, response) ||
        response.view().find("\"ok\":true") == std::string::npos) {
        return false;
    }
    Message stopping;
    if (!send_legacy_request(stop, stopping)) {
        return false;
    }
    for (int i = 0; i < 40; ++i) {
        Message probe;
        if (!send_legacy_request(ping, probe)) {
            return true;
        }
        usleep(50000);
    }
    return false;
}

int serve_foreground(output::Context& destination) {
    umask(0077);
    std::string preparation_error;
    if (!prepare_runtime(preparation_error) || !logging::prepare_boot_log(preparation_error)) {
        logging::write(logging::Level::Error, "daemon", preparation_error);
        output::err(destination, preparation_error, '\n');
        return 1;
    }
    PreparedConfig log_config;
    std::string config_error;
    if (read_config_file(runtime_config_file().string(), log_config, config_error))
        logging::set_debug_enabled(log_config.debug || log_config.verbose);
    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    if (ec) {
        output::err(destination, "failed to create ", runtime_data_dir(), ": ", ec.message(), "\n");
        return 1;
    }

    const auto socket_path = runtime_socket_file().string();
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        output::err(destination, "socket path is too long: ", socket_path, "\n");
        return 1;
    }

    if (fs::exists(runtime_socket_file(), ec) && !daemon_running()) {
        (void)stop_legacy_daemon();
        if (fs::exists(runtime_socket_file(), ec)) {
            std::string probe_error;
            const int probe_fd = open_client_socket(probe_error);
            if (probe_fd >= 0) {
                close(probe_fd);
                output::err(destination, "an incompatible kagamid still owns ", socket_path, "\n");
                return 1;
            }
        }
    }

    const auto lock_path = runtime_daemon_lock_file();
    fs::create_directories(lock_path.parent_path(), ec);
    if (ec) {
        output::err(destination, "failed to create ", lock_path.parent_path(), ": ", ec.message(),
                    "\n");
        return 1;
    }
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        output::err(destination, "open ", lock_path, ": ", std::strerror(errno), "\n");
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        output::err(destination, "kagamid is already running\n");
        close(lock_fd);
        return 1;
    }

    // The lifetime lock proves there is no live owner before replacing a stale
    // socket. It also serializes concurrent first-use auto-start attempts.
    fs::remove(runtime_socket_file(), ec);
    const int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        output::err(destination, "socket: ", std::strerror(errno), "\n");
        close(lock_fd);
        return 1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        output::err(destination, "bind ", socket_path, ": ", std::strerror(errno), "\n");
        close(server_fd);
        close(lock_fd);
        return 1;
    }
    chmod(socket_path.c_str(), 0600);

    if (listen(server_fd, 8) != 0) {
        output::err(destination, "listen: ", std::strerror(errno), "\n");
        close(server_fd);
        close(lock_fd);
        return 1;
    }

    // Keep a capability FD only for built-in Kasumi or for the exact LKM
    // instance loaded by this Kagami boot. Starting without Kasumi is valid:
    // an explicit lkm load can acquire ownership later.
#if defined(KAGAMI_EMBEDDED)
    kasumi::set_connection_persistent(true);
    (void)kasumi::is_available();
#else
    (void)lkm::retain_owned_connection();
#endif

    {
        std::ofstream pid(runtime_pid_file(), std::ios::trunc);
        if (pid) {
            pid << getpid() << "\n";
        }
    }
    append_log("ready pid=" + std::to_string(getpid()) + " socket=" + socket_path);

    bounded::Storage command_storage;
    constexpr std::size_t output_capacity = 1024UL * 1024;
    constexpr std::size_t error_capacity = 64UL * 1024;
    constexpr std::size_t wire_capacity = 64UL * 1024;
    constexpr std::size_t storage_capacity = output_capacity + error_capacity + wire_capacity;
    if (!command_storage.prepare_storage(storage_capacity, storage_capacity)) {
        append_log("cannot prepare daemon command storage", logging::Level::Error);
        close(server_fd);
        fs::remove(runtime_socket_file(), ec);
        fs::remove(runtime_pid_file(), ec);
        close(lock_fd);
        return 1;
    }
    output::Context command_destination{{command_storage.data(), output_capacity},
                                        {command_storage.data() + output_capacity, error_capacity}};
    char* const wire_bytes = command_storage.data() + output_capacity + error_capacity;
    Message request;
    ArgumentStorage request_arguments;
    if (!request.prepare_message(daemon_protocol::request_limit) ||
        !request_arguments.prepare_arguments((daemon_protocol::request_limit / 3) + 1)) {
        close(server_fd);
        fs::remove(runtime_socket_file(), ec);
        fs::remove(runtime_pid_file(), ec);
        close(lock_fd);
        return 1;
    }
    bool stopping = false;
    while (!stopping) {
        const int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            append_log(std::string("accept failed: ") + std::strerror(errno),
                       logging::Level::Error);
            continue;
        }

        timeval timeout = {};
        timeout.tv_sec = 5;
        (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        const auto reply = [&](bool ok, int code, int number, std::string_view out,
                               std::string_view err) {
            if (!send_response(client_fd, ok, code, number, out, err, wire_bytes, wire_capacity))
                append_log("response transport failed", logging::Level::Warning);
        };
        const bool received = request.read_from(client_fd);
        daemon_protocol::WireJson decoder(request.data(), request.size());
        if (!received || !decoder.request(request_arguments) || request_arguments.view().empty()) {
            append_log("rejected malformed or empty request", logging::Level::Warning);
            reply(false, 1, EINVAL, "", "empty daemon request");
            close(client_fd);
            continue;
        }

        const auto request_args = request_arguments.view();
        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "stop") {
            reply(true, 0, 0, "stopping\n", "");
            stopping = true;
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "ping") {
            reply(true, 0, 0, "pong\n", "");
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 &&
            request_args[1] == "status") {
            reply(true, 0, 0, status_json(true), "");
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon") {
            reply(false, 1, EINVAL, "", "daemon control commands cannot be forwarded\n");
            close(client_fd);
            continue;
        }

        const auto result = run_command_capture(request_args, command_destination);
        if (!daemon_protocol::send_response(client_fd, result.exit_code == 0, result.exit_code,
                                            result.error_number, result.stdout_text,
                                            result.stderr_text, wire_bytes, wire_capacity))
            append_log("response transport failed", logging::Level::Warning);
        close(client_fd);
    }

    append_log("kagamid stopped");
    kasumi::release_connection();
    kasumi::set_connection_persistent(false);
    close(server_fd);
    fs::remove(runtime_socket_file(), ec);
    fs::remove(runtime_pid_file(), ec);
    close(lock_fd);
    return 0;
}

int start_background(output::Context& destination, bool report_status) {
    if (daemon_running()) {
        return report_status ? print_status_json(destination) : 0;
    }

    std::string preparation_error;
    if (!prepare_runtime(preparation_error) || !logging::prepare_boot_log(preparation_error)) {
        output::err(destination, preparation_error, '\n');
        return 1;
    }
    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    const pid_t pid = fork();
    if (pid < 0) {
        output::err(destination, "fork: ", std::strerror(errno), "\n");
        return 1;
    }
    if (pid == 0) {
        setsid();
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        const int log_fd = open(runtime_log_file().c_str(),
                                O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        const int exit_code = serve_foreground(destination);
        if (destination.error) {
            (void)output::write_fd(STDERR_FILENO,
                                   "Kagami: command output exceeds prepared capacity\n");
            _exit(1);
        }
        const bool out_ok = output::write_fd(STDOUT_FILENO, destination.out.view());
        const bool err_ok = output::write_fd(STDERR_FILENO, destination.err.view());
        _exit(out_ok && err_ok ? exit_code : 1);
    }

    for (int i = 0; i < 20; ++i) {
        if (daemon_running()) {
            return report_status ? print_status_json(destination) : 0;
        }
        usleep(50000);
    }
    output::err(destination, "kagamid did not become ready\n");
    return 1;
}
}  // namespace
#endif

int run_via_daemon(output::Context& destination, Arguments args, bool allow_start) {
#if defined(__linux__) || defined(__APPLE__)
    if (args.empty()) {
        return 1;
    }

    Message response;
    std::string error;
    if (send_request(args, 0, response, error) != 0) {
        if (!allow_start || start_background(destination, false) != 0 ||
            send_request(args, 0, response, error) != 0) {
            output::err(destination, "daemon unavailable: ", error, "\n");
            return 1;
        }
    }

    daemon_protocol::Response envelope;
    daemon_protocol::WireJson decoder(response.data(), response.size());
    if (!decoder.response(envelope)) {
        output::err(destination, "invalid daemon response fields\n");
        return 1;
    }
    const bool out_ok = output::write_fd(STDOUT_FILENO, envelope.output);
    const bool err_ok = output::write_fd(STDERR_FILENO, envelope.error);
    return out_ok && err_ok ? envelope.exit_code : 1;
#else
    (void)args;
    output::err(destination, "daemon sockets are not supported on this platform\n");
    return 1;
#endif
}

namespace {
int print_status_json(output::Context& destination) {
#if defined(__linux__) || defined(__APPLE__)
    const bool running = daemon_running();
#else
    const bool running = false;
#endif
    output::out(destination, status_json(running));
    return 0;
}
}  // namespace

int run_daemon_command(output::Context& destination, Arguments args) {
    const std::string_view sub = args.size() > 1 ? std::string_view(args[1]) : "";
    if (sub == "status") {
        return print_status_json(destination);
    }

#if defined(__linux__) || defined(__APPLE__)
    if (sub == "serve") {
        return serve_foreground(destination);
    }
    if (sub == "start") {
        return start_background(destination, true);
    }
    if (sub == "call") {
        if (args.size() < 3) {
            output::err(destination, "daemon call requires a command\n");
            return 1;
        }
        Message response;
        std::string error;
        const int rc = send_request(args, 2, response, error);
        if (rc != 0) {
            output::err(destination, "daemon unavailable: ", error, "\n");
            return rc;
        }
        output::out(destination, response.view());
        return 0;
    }
    if (sub == "ping" || sub == "stop") {
        Message response;
        std::string error;
        const int rc = send_request(args, 0, response, error);
        if (rc != 0) {
            output::err(destination, "daemon unavailable: ", error, "\n");
            return rc;
        }
        output::out(destination, response.view());
        return 0;
    }
#else
    if (sub == "serve" || sub == "start" || sub == "call" || sub == "ping" || sub == "stop") {
        output::err(destination, "daemon sockets are not supported on this platform\n");
        return 1;
    }
#endif

    output::err(destination, "usage: kagamid daemon status|start|serve|call|ping|stop\n");
    return 1;
}

}  // namespace kagami
