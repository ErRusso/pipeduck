#include "pipeduck/common.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <thread>

extern char** environ;

namespace pipeduck {
namespace {

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void append_available(int fd, std::string& output, bool& open) {
    char buffer[8192];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            open = false;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        open = false;
        return;
    }
}

} // namespace

std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool iequals(const std::string& left, const std::string& right) {
    return to_lower(left) == to_lower(right);
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> split_lines(const std::string& value) {
    std::vector<std::string> lines;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> split(const std::string& value, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : value) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

std::string join(const std::vector<std::string>& values, const std::string& separator) {
    std::string result;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            result += separator;
        }
        result += values[i];
    }
    return result;
}

CommandResult run_command(const std::string& program, const std::vector<std::string>& args,
                          std::chrono::milliseconds timeout) {
    CommandResult result;
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe2(out_pipe, O_CLOEXEC) < 0) {
        throw std::runtime_error("pipe2 failed: " + std::string(std::strerror(errno)));
    }
    if (pipe2(err_pipe, O_CLOEXEC) < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        throw std::runtime_error("pipe2 failed: " + std::string(std::strerror(errno)));
    }

    std::vector<std::string> argv_storage;
    argv_storage.push_back(program);
    argv_storage.insert(argv_storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& value : argv_storage) {
        argv.push_back(const_cast<char*>(value.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid = -1;
    const int spawn_err =
        posix_spawnp(&pid, program.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (spawn_err != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        throw std::runtime_error("failed to execute " + program + ": " + std::strerror(spawn_err));
    }

    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);

    bool out_open = true;
    bool err_open = true;
    bool terminated = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (out_open || err_open) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timed_out = true;
            terminated = true;
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd fds[2] = {
            {out_pipe[0], static_cast<short>(out_open ? POLLIN : 0), 0},
            {err_pipe[0], static_cast<short>(err_open ? POLLIN : 0), 0},
        };
        const int wait_ms = static_cast<int>(std::max<std::int64_t>(remaining.count(), 1));
        const int polled = poll(fds, 2, std::min(wait_ms, 50));
        if (polled < 0) {
            if (errno == EINTR) {
                continue;
            }
            result.error = "poll failed: " + std::string(std::strerror(errno));
            terminated = true;
            break;
        }
        if (polled > 0) {
            if (out_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                append_available(out_pipe[0], result.out, out_open);
            }
            if (err_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                append_available(err_pipe[0], result.err, err_open);
            }
        }
    }

    const auto kill_child = [&pid]() {
        if (pid <= 0) {
            return;
        }
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    };

    // Pipes can close while the child is still running (for example, when it
    // redirects its own output). Never wait unconditionally: keep polling
    // waitpid() until the original deadline.
    int status = 0;
    bool reaped = false;
    const auto reap_deadline =
        terminated ? std::chrono::steady_clock::now() + std::chrono::seconds(2) : deadline;
    for (;;) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            reaped = true;
            break;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ECHILD) {
                result.error = "waitpid failed: " + std::string(std::strerror(errno));
            }
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= reap_deadline) {
            if (!terminated) {
                result.timed_out = true;
                terminated = true;
            }
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(reap_deadline - now);
        poll(nullptr, 0, static_cast<int>(std::min<std::int64_t>(remaining.count(), 20)));
    }

    if (terminated) {
        kill_child();
        // Reap the killed child, but do not block forever if it is stuck in an
        // uninterruptible state. The timeout remains reported either way.
        const auto final_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!reaped && std::chrono::steady_clock::now() < final_deadline) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                reaped = true;
                break;
            }
            if (waited < 0 && errno != EINTR) {
                break;
            }
            poll(nullptr, 0, 10);
        }
        append_available(out_pipe[0], result.out, out_open);
        append_available(err_pipe[0], result.err, err_open);
        if (result.timed_out) {
            result.exit_code = 124;
        }
    } else if (reaped) {
        append_available(out_pipe[0], result.out, out_open);
        append_available(err_pipe[0], result.err, err_open);
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
        } else {
            result.error = "child did not exit normally";
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    return result;
}

std::string run_command_checked(const std::string& program, const std::vector<std::string>& args,
                                std::chrono::milliseconds timeout) {
    const CommandResult result = run_command(program, args, timeout);
    if (!result.error.empty()) {
        throw std::runtime_error(program + " failed: " + result.error);
    }
    if (result.timed_out) {
        std::string error = program + " timed out after " + std::to_string(timeout.count()) + " ms";
        const std::string detail = trim(result.err);
        if (!detail.empty()) {
            error += ": " + detail;
        }
        throw std::runtime_error(error);
    }
    if (result.exit_code != 0) {
        std::string error = "exit code " + std::to_string(result.exit_code);
        const std::string detail = trim(result.err);
        if (!detail.empty()) {
            error += ": " + detail;
        }
        throw std::runtime_error(program + " failed: " + error);
    }
    return result.out;
}

void run_status_checked(const std::string& program, const std::vector<std::string>& args,
                        std::chrono::milliseconds timeout) {
    (void)run_command_checked(program, args, timeout);
}

std::optional<std::string> getenv_string(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::filesystem::path config_dir() {
    if (const auto xdg = getenv_string("XDG_CONFIG_HOME")) {
        return std::filesystem::path(*xdg);
    }
    if (const auto home = getenv_string("HOME")) {
        return std::filesystem::path(*home) / ".config";
    }
    throw std::runtime_error("cannot determine the configuration directory (HOME is unset)");
}

std::filesystem::path config_path() {
    return config_dir() / "pipeduck" / "config.toml";
}

std::filesystem::path runtime_dir() {
    if (const auto xdg = getenv_string("XDG_RUNTIME_DIR")) {
        return std::filesystem::path(*xdg);
    }
    return std::filesystem::temp_directory_path();
}

std::string unique_instance_name(const std::string& prefix) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return prefix + "-" + std::to_string(millis);
}

std::string format_double(double value, int precision) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(precision);
    stream << value;
    return stream.str();
}

} // namespace pipeduck
