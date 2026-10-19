#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pipeduck {

using StringMap = std::map<std::string, std::string>;

std::string trim(std::string value);
std::string to_lower(std::string value);
bool iequals(const std::string& left, const std::string& right);
bool starts_with(const std::string& value, const std::string& prefix);
std::vector<std::string> split_lines(const std::string& value);
std::vector<std::string> split(const std::string& value, char separator);
std::string join(const std::vector<std::string>& values, const std::string& separator);

struct CommandResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string out;
    std::string err;
};

// Runs a program without a shell and captures stdout/stderr. The process group is
// killed when the timeout expires. Never throws for a non-zero exit code.
CommandResult run_command(const std::string& program, const std::vector<std::string>& args,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

// Like run_command(), but returns stdout or throws std::runtime_error with stderr.
std::string
run_command_checked(const std::string& program, const std::vector<std::string>& args,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

// Like run_command_checked(), but discards stdout.
void run_status_checked(const std::string& program, const std::vector<std::string>& args,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

std::optional<std::string> getenv_string(const char* name);
std::filesystem::path config_dir();
std::filesystem::path config_path();
std::filesystem::path runtime_dir();
std::string unique_instance_name(const std::string& prefix);

std::string format_double(double value, int precision);

} // namespace pipeduck
