#include "app.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#define NOMMIDS
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fps {

std::string to_string_port(std::uint16_t port) {
    return std::to_string(static_cast<unsigned int>(port));
}

void append_trace_args(const AppConfig& config, std::vector<std::string>& args) {
    if (!config.trace_dir.empty()) {
        args.push_back("--trace-dir");
        args.push_back(config.trace_dir);
    }
    if (!config.trace_frame_data) {
        args.push_back("--trace-frame-data");
        args.push_back("off");
    }
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    if (config.trace_packet_logs) {
        args.push_back("--trace-packet-logs");
        args.push_back("on");
    }
#endif
}

void append_link_args(const AppConfig& config, std::vector<std::string>& args) {
    args.push_back("--latency-ms");
    args.push_back(std::to_string(config.latency_ms));
    args.push_back("--jitter-ms");
    args.push_back(std::to_string(config.jitter_ms));
}

void append_replay_args(const AppConfig& config, std::vector<std::string>& args) {
    args.push_back("--replay-port");
    args.push_back(to_string_port(config.replay_port));
    args.push_back("--replay-dir");
    args.push_back(config.replay_dir);
}

#ifdef _WIN32

std::string quote_arg(const std::string& arg) {
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

PROCESS_INFORMATION spawn_process(const std::vector<std::string>& args) {
    std::string command;
    for (const std::string& arg : args) {
        if (!command.empty()) {
            command += ' ';
        }
        command += quote_arg(arg);
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    if (!CreateProcessA(
            nullptr,
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        throw std::runtime_error("failed to launch child process");
    }
    return process;
}

struct RemoteClientLauncher::Impl {
    std::vector<PROCESS_INFORMATION> clients;

    ~Impl() {
        for (PROCESS_INFORMATION& client : clients) {
            TerminateProcess(client.hProcess, 0);
            WaitForSingleObject(client.hProcess, 2000);
            CloseHandle(client.hThread);
            CloseHandle(client.hProcess);
        }
    }
};

void run_launcher(const AppConfig& config) {
    std::vector<std::string> server_args{
        config.executable,
        "--server",
        "--port",
        to_string_port(config.port),
        "--bots",
        std::to_string(config.bots)};
    append_trace_args(config, server_args);
    append_replay_args(config, server_args);
    PROCESS_INFORMATION server = spawn_process(server_args);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::vector<PROCESS_INFORMATION> clients;
    clients.reserve(static_cast<std::size_t>(config.clients));
    for (int index = 0; index < config.clients; ++index) {
        std::vector<std::string> client_args{
            config.executable,
            "--client",
            "--host",
            config.host,
            "--port",
            to_string_port(config.port)};
        append_link_args(config, client_args);
        append_trace_args(config, client_args);
        append_replay_args(config, client_args);
        clients.push_back(spawn_process(client_args));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    for (PROCESS_INFORMATION& client : clients) {
        WaitForSingleObject(client.hProcess, INFINITE);
        CloseHandle(client.hThread);
        CloseHandle(client.hProcess);
    }
    TerminateProcess(server.hProcess, 0);
    WaitForSingleObject(server.hProcess, 2000);
    CloseHandle(server.hThread);
    CloseHandle(server.hProcess);
}

#else

pid_t spawn_process(const std::vector<std::string>& args) {
    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("failed to fork child process");
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1U);
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

struct RemoteClientLauncher::Impl {
    std::vector<pid_t> clients;

    ~Impl() {
        for (pid_t client : clients) {
            kill(client, SIGTERM);
        }
        for (pid_t client : clients) {
            int status = 0;
            while (waitpid(client, &status, 0) < 0) {
                if (errno != EINTR) {
                    break;
                }
            }
        }
    }
};

void run_launcher(const AppConfig& config) {
    std::vector<std::string> server_args{
        config.executable,
        "--server",
        "--port",
        to_string_port(config.port),
        "--bots",
        std::to_string(config.bots)};
    append_trace_args(config, server_args);
    append_replay_args(config, server_args);
    const pid_t server = spawn_process(server_args);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::vector<pid_t> clients;
    clients.reserve(static_cast<std::size_t>(config.clients));
    for (int index = 0; index < config.clients; ++index) {
        std::vector<std::string> client_args{
            config.executable,
            "--client",
            "--host",
            config.host,
            "--port",
            to_string_port(config.port)};
        append_link_args(config, client_args);
        append_trace_args(config, client_args);
        append_replay_args(config, client_args);
        clients.push_back(spawn_process(client_args));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    for (pid_t client : clients) {
        int status = 0;
        while (waitpid(client, &status, 0) < 0) {
            if (errno != EINTR) {
                break;
            }
        }
    }
    kill(server, SIGTERM);
    int status = 0;
    (void)waitpid(server, &status, 0);
}

#endif

RemoteClientLauncher::RemoteClientLauncher(const AppConfig& config)
    : impl_(std::make_unique<Impl>()) {
    if (config.clients <= 0) {
        return;
    }
    if (config.executable.empty()) {
        throw std::runtime_error("cannot determine executable path for spawned clients");
    }
    impl_->clients.reserve(static_cast<std::size_t>(config.clients));
    for (int index = 0; index < config.clients; ++index) {
        std::vector<std::string> client_args{
            config.executable,
            "--client",
            "--host",
            config.host,
            "--port",
            to_string_port(config.port)};
        append_link_args(config, client_args);
        append_trace_args(config, client_args);
        append_replay_args(config, client_args);
        impl_->clients.push_back(spawn_process(client_args));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

RemoteClientLauncher::~RemoteClientLauncher() = default;

AppConfig parse_args(int argc, char** argv) {
    AppConfig config;
    if (argc > 0 && argv[0] != nullptr) {
        config.executable = argv[0];
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&arg, &i, argc, argv]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--server") {
            config.server = true;
        } else if (arg == "--client") {
            config.client = true;
        } else if (arg == "--listen") {
            config.listen = true;
        } else if (arg == "--clients") {
            config.launcher = true;
            config.clients = std::max(0, std::stoi(require_value()));
        } else if (arg == "--host") {
            config.host = require_value();
        } else if (arg == "--port") {
            config.port = static_cast<std::uint16_t>(std::stoi(require_value()));
        } else if (arg == "--replay-port") {
            config.replay_port = static_cast<std::uint16_t>(std::stoi(require_value()));
        } else if (arg == "--bots") {
            config.bots = std::max(0, std::stoi(require_value()));
        } else if (arg == "--latency-ms") {
            config.latency_ms = std::max(0.0, std::stod(require_value()));
        } else if (arg == "--jitter-ms") {
            config.jitter_ms = std::max(0.0, std::stod(require_value()));
        } else if (arg == "--trace-dir") {
#ifdef KAGE_SYNC_ENABLE_TRACING
            config.trace_dir = require_value();
#else
            (void)require_value();
            throw std::runtime_error("--trace-dir requires a build with KAGE_SYNC_ENABLE_TRACING=ON");
#endif
        } else if (arg == "--replay-dir") {
            config.replay_dir = require_value();
        } else if (arg == "--trace-frame-data") {
            const std::string value = require_value();
            if (value == "on" || value == "true" || value == "1") {
                config.trace_frame_data = true;
            } else if (value == "off" || value == "false" || value == "0") {
                config.trace_frame_data = false;
            } else {
                throw std::runtime_error("--trace-frame-data must be on or off");
            }
        } else if (arg == "--trace-packet-logs") {
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
            const std::string value = require_value();
            if (value == "on" || value == "true" || value == "1") {
                config.trace_packet_logs = true;
            } else if (value == "off" || value == "false" || value == "0") {
                config.trace_packet_logs = false;
            } else {
                throw std::runtime_error("--trace-packet-logs must be on or off");
            }
#else
            (void)require_value();
            throw std::runtime_error("--trace-packet-logs requires a build with KAGE_SYNC_TRACE_PACKET_LOGS=ON");
#endif
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    const int modes = (config.server ? 1 : 0) + (config.client ? 1 : 0) +
        (config.launcher && !config.listen ? 1 : 0) + (config.listen ? 1 : 0);
    if (modes != 1) {
        throw std::runtime_error("pass exactly one of --server, --client, --listen, or --clients N");
    }
    if (config.launcher && !config.listen && config.clients <= 0) {
        throw std::runtime_error("--clients must be greater than zero");
    }
    if (config.executable.empty() && (config.launcher || (config.listen && config.clients > 0))) {
        throw std::runtime_error("cannot determine executable path for launcher mode");
    }
    if (config.replay_port == 0U) {
        config.replay_port = static_cast<std::uint16_t>(config.port + 1U);
    }
    return config;
}

}  // namespace fps
