#include "core/daemon.hpp"

#include "core/command.hpp"
#include "core/json.hpp"
#include "core/json_value.hpp"
#include "core/lkm.hpp"
#include "core/log.hpp"
#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

static int print_status_json();

static void append_log(const std::string& message) {
    logging::append("daemon", message);
}

static std::string request_name(const std::vector<std::string>& args) {
    if (args.empty()) {
        return "unknown";
    }
    return args.size() > 1 ? args[0] + " " + args[1] : args[0];
}

static std::string join_request(const std::vector<std::string>& args, std::size_t start) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = start; i < args.size(); ++i) {
        if (i > start) {
            out << ',';
        }
        out << json_quote(args[i]);
    }
    out << "]\n";
    return out.str();
}

static std::vector<std::string> split_request(const std::string& request) {
    std::vector<std::string> args;
    JsonValue root;
    std::string error;
    if (!parse_json(request, root, error) || !root.is_array()) {
        return args;
    }
    args.reserve(root.array_value.size());
    for (const auto& item : root.array_value) {
        if (!item.is_string()) {
            args.clear();
            return args;
        }
        args.push_back(item.string_value);
    }
    return args;
}

static std::string response_json(bool ok, int exit_code, int error_number,
                                 const std::string& out, const std::string& err) {
    std::ostringstream json;
    json << "{"
         << "\"ok\":" << (ok ? "true" : "false") << ","
         << "\"exit_code\":" << exit_code << ","
         << "\"errno\":" << error_number << ","
         << "\"stdout\":" << json_quote(out) << ","
         << "\"stderr\":" << json_quote(err)
         << "}\n";
    return json.str();
}

static std::string status_json(bool running) {
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
        << "\"log_file\":" << json_quote(runtime_log_file().string())
        << "}\n";
    return out.str();
}

#if defined(__linux__) || defined(__APPLE__)
static bool write_all(int fd, const std::string& data) {
    const char* ptr = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t written = send(fd, ptr, left, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        ptr += written;
        left -= static_cast<std::size_t>(written);
    }
    return true;
}

static std::string read_all(int fd, std::size_t limit) {
    std::string data;
    char buffer[1024] = {};
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        data.append(buffer, static_cast<std::size_t>(n));
        if (data.size() > limit) {
            data.clear();
            break;
        }
        if (data.find('\n') != std::string::npos) {
            break;
        }
    }
    return data;
}

static std::string legacy_join_request(const std::vector<std::string>& args) {
    std::ostringstream out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            out << '\t';
        }
        out << args[i];
    }
    out << '\n';
    return out.str();
}

static int open_client_socket(std::string& error) {
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

static int send_request(const std::vector<std::string>& args, std::size_t start, std::string& response, std::string& error) {
    const int fd = open_client_socket(error);
    if (fd < 0) {
        return 1;
    }
    if (!write_all(fd, join_request(args, start))) {
        error = std::strerror(errno);
        close(fd);
        return 1;
    }
    shutdown(fd, SHUT_WR);
    response = read_all(fd, 1024 * 1024);
    close(fd);
    return 0;
}

static bool send_legacy_request(const std::vector<std::string>& args,
                                std::string& response) {
    std::string error;
    const int fd = open_client_socket(error);
    if (fd < 0) {
        return false;
    }
    if (!write_all(fd, legacy_join_request(args))) {
        close(fd);
        return false;
    }
    shutdown(fd, SHUT_WR);
    response = read_all(fd, 1024 * 1024);
    close(fd);
    return !response.empty();
}

static bool daemon_running() {
    std::vector<std::string> ping = {"daemon", "ping"};
    std::string response;
    std::string error;
    return send_request(ping, 0, response, error) == 0 && response.find("\"ok\":true") != std::string::npos;
}

static bool stop_legacy_daemon() {
    std::string response;

    if (!send_legacy_request({"daemon", "ping"}, response) ||
        response.find("\"ok\":true") == std::string::npos) {
        return false;
    }
    response.clear();
    if (!send_legacy_request({"daemon", "stop"}, response)) {
        return false;
    }
    for (int i = 0; i < 40; ++i) {
        std::string probe;
        if (!send_legacy_request({"daemon", "ping"}, probe)) {
            return true;
        }
        usleep(50000);
    }
    return false;
}

static int serve_foreground() {
    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    if (ec) {
        std::cerr << "failed to create " << runtime_data_dir() << ": " << ec.message() << "\n";
        return 1;
    }

    const auto socket_path = runtime_socket_file().string();
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::cerr << "socket path is too long: " << socket_path << "\n";
        return 1;
    }

    if (fs::exists(runtime_socket_file(), ec) && !daemon_running()) {
        (void)stop_legacy_daemon();
        if (fs::exists(runtime_socket_file(), ec)) {
            std::string probe_error;
            const int probe_fd = open_client_socket(probe_error);
            if (probe_fd >= 0) {
                close(probe_fd);
                std::cerr << "an incompatible kagamid still owns " << socket_path << "\n";
                return 1;
            }
        }
    }

    const auto lock_path = runtime_daemon_lock_file();
    fs::create_directories(lock_path.parent_path(), ec);
    if (ec) {
        std::cerr << "failed to create " << lock_path.parent_path() << ": " << ec.message() << "\n";
        return 1;
    }
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        std::cerr << "open " << lock_path << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::cerr << "kagamid is already running\n";
        close(lock_fd);
        return 1;
    }

    // The lifetime lock proves there is no live owner before replacing a stale
    // socket. It also serializes concurrent first-use auto-start attempts.
    fs::remove(runtime_socket_file(), ec);
    const int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << "\n";
        close(lock_fd);
        return 1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind " << socket_path << ": " << std::strerror(errno) << "\n";
        close(server_fd);
        close(lock_fd);
        return 1;
    }
    chmod(socket_path.c_str(), 0600);

    if (listen(server_fd, 8) != 0) {
        std::cerr << "listen: " << std::strerror(errno) << "\n";
        close(server_fd);
        close(lock_fd);
        return 1;
    }

    // Keep a capability FD only for built-in Kasumi or for the exact LKM
    // instance loaded by this Kagami boot. Starting without Kasumi is valid:
    // an explicit lkm load can acquire ownership later.
    (void)lkm::retain_owned_connection();

    {
        std::ofstream pid(runtime_pid_file(), std::ios::trunc);
        if (pid) {
            pid << getpid() << "\n";
        }
    }
    append_log("kagamid started pid=" + std::to_string(getpid()));

    bool stopping = false;
    while (!stopping) {
        const int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            append_log(std::string("accept failed: ") + std::strerror(errno));
            continue;
        }

        timeval timeout = {};
        timeout.tv_sec = 5;
        (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                         &timeout, sizeof(timeout));

        const auto request = read_all(client_fd, 64 * 1024);
        auto request_args = split_request(request);
        if (request_args.empty()) {
            write_all(client_fd, response_json(false, 1, EINVAL, "", "empty daemon request"));
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "stop") {
            write_all(client_fd, response_json(true, 0, 0, "stopping\n", ""));
            stopping = true;
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "ping") {
            write_all(client_fd, response_json(true, 0, 0, "pong\n", ""));
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "status") {
            write_all(client_fd, response_json(true, 0, 0, status_json(true), ""));
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon") {
            write_all(client_fd, response_json(false, 1, EINVAL, "", "daemon control commands cannot be forwarded\n"));
            close(client_fd);
            continue;
        }

        const auto result = run_command_capture(request_args);
        append_log("request " + request_name(request_args) +
                   " exit=" + std::to_string(result.exit_code));
        write_all(client_fd, response_json(result.exit_code == 0, result.exit_code,
                                           result.error_number, result.stdout_text,
                                           result.stderr_text));
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

static int start_background(bool report_status) {
    if (daemon_running()) {
        return report_status ? print_status_json() : 0;
    }

    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (pid == 0) {
        setsid();
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        const int log_fd = open(runtime_log_file().c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        _exit(serve_foreground());
    }

    for (int i = 0; i < 20; ++i) {
        if (daemon_running()) {
            return report_status ? print_status_json() : 0;
        }
        usleep(50000);
    }
    std::cerr << "kagamid did not become ready\n";
    return 1;
}
#endif

int run_via_daemon(const std::vector<std::string>& args, bool allow_start) {
#if defined(__linux__) || defined(__APPLE__)
    if (args.empty()) {
        return 1;
    }

    std::string response;
    std::string error;
    if (send_request(args, 0, response, error) != 0) {
        if (!allow_start || start_background(false) != 0 ||
            send_request(args, 0, response, error) != 0) {
            std::cerr << "daemon unavailable: " << error << "\n";
            return 1;
        }
    }

    JsonValue envelope;
    if (!parse_json(response, envelope, error) || !envelope.is_object()) {
        std::cerr << "invalid daemon response: " << error << "\n";
        return 1;
    }
    const JsonValue* code = envelope.find("exit_code");
    const JsonValue* out = envelope.find("stdout");
    const JsonValue* err = envelope.find("stderr");
    if (!code || !code->is_number() || !out || !out->is_string() ||
        !err || !err->is_string()) {
        std::cerr << "invalid daemon response fields\n";
        return 1;
    }
    std::cout << out->string_value;
    std::cerr << err->string_value;
    return static_cast<int>(code->number_value);
#else
    (void)args;
    std::cerr << "daemon sockets are not supported on this platform\n";
    return 1;
#endif
}

static int print_status_json() {
#if defined(__linux__) || defined(__APPLE__)
    const bool running = daemon_running();
#else
    const bool running = false;
#endif
    std::cout << status_json(running);
    return 0;
}

int run_daemon_command(const std::vector<std::string>& args) {
    const std::string sub = args.size() > 1 ? args[1] : "";
    if (sub == "status") {
        return print_status_json();
    }

#if defined(__linux__) || defined(__APPLE__)
    if (sub == "serve") {
        return serve_foreground();
    }
    if (sub == "start") {
        return start_background(true);
    }
    if (sub == "call") {
        if (args.size() < 3) {
            std::cerr << "daemon call requires a command\n";
            return 1;
        }
        std::string response;
        std::string error;
        const int rc = send_request(args, 2, response, error);
        if (rc != 0) {
            std::cerr << "daemon unavailable: " << error << "\n";
            return rc;
        }
        std::cout << response;
        return 0;
    }
    if (sub == "ping" || sub == "stop") {
        std::string response;
        std::string error;
        const int rc = send_request(args, 0, response, error);
        if (rc != 0) {
            std::cerr << "daemon unavailable: " << error << "\n";
            return rc;
        }
        std::cout << response;
        return 0;
    }
#else
    if (sub == "serve" || sub == "start" || sub == "call" || sub == "ping" || sub == "stop") {
        std::cerr << "daemon sockets are not supported on this platform\n";
        return 1;
    }
#endif

    std::cerr << "usage: kagamid daemon status|start|serve|call|ping|stop\n";
    return 1;
}

} // namespace kagami
