#include "pipeduck/config.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace pipeduck {
namespace {

std::string strip_comment(const std::string& line) {
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string unquote(const std::string& value, int line_number) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        throw std::runtime_error("line " + std::to_string(line_number) +
                                 ": expected a quoted string");
    }
    std::string result;
    result.reserve(value.size() - 2);
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        const char c = value[i];
        if (c != '\\') {
            result.push_back(c);
            continue;
        }
        if (i + 2 >= value.size()) {
            throw std::runtime_error("line " + std::to_string(line_number) + ": invalid escape");
        }
        const char next = value[++i];
        switch (next) {
        case 'n':
            result.push_back('\n');
            break;
        case 't':
            result.push_back('\t');
            break;
        case 'r':
            result.push_back('\r');
            break;
        case '"':
            result.push_back('"');
            break;
        case '\\':
            result.push_back('\\');
            break;
        default:
            throw std::runtime_error("line " + std::to_string(line_number) + ": invalid escape");
        }
    }
    return result;
}

int parse_int(const std::string& value, int line_number) {
    try {
        std::size_t consumed = 0;
        const int result = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error("line " + std::to_string(line_number) + ": expected an integer");
    }
}

double parse_double(const std::string& value, int line_number) {
    std::size_t consumed = 0;
    double result = 0.0;
    try {
        result = std::stod(value, &consumed);
    } catch (const std::exception&) {
        throw std::runtime_error("line " + std::to_string(line_number) + ": expected a number");
    }
    if (consumed != value.size()) {
        throw std::runtime_error("line " + std::to_string(line_number) + ": expected a number");
    }
    if (!std::isfinite(result)) {
        throw std::runtime_error("line " + std::to_string(line_number) +
                                 ": expected a finite number");
    }
    return result;
}

double finite_or(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

std::string quote(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\t':
            result += "\\t";
            break;
        case '\r':
            result += "\\r";
            break;
        default:
            result.push_back(c);
            break;
        }
    }
    result.push_back('"');
    return result;
}

void parse_source_key(ConfiguredSource& source, const std::string& key, const std::string& value,
                      int line_number) {
    const std::string parsed = unquote(value, line_number);
    if (key == "label")
        source.label = parsed;
    else if (key == "application_name")
        source.application_name = parsed;
    else if (key == "application_process_binary")
        source.application_process_binary = parsed;
    else if (key == "media_name")
        source.media_name = parsed;
    else if (key == "media_role")
        source.media_role = parsed;
    else if (key == "media_class")
        source.media_class = parsed;
    else if (key == "node_name")
        source.node_name = parsed;
}

void validate_profile_name(const std::string& name) {
    if (name.empty()) {
        throw std::runtime_error("profile name must not be empty");
    }
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
            throw std::runtime_error("invalid profile name '" + name +
                                     "' (use letters, digits, '-' or '_')");
        }
    }
}

std::string profile_list(const std::map<std::string, DuckingProfile>& profiles) {
    std::string result;
    for (const auto& [name, _] : profiles) {
        if (!result.empty()) {
            result += ", ";
        }
        result += name;
    }
    return result;
}

void normalize_settings(int& duck_percent, double& vad_threshold,
                        std::optional<double>& mic_vad_threshold, int& hold_ms, int& attack_fade_ms,
                        int& release_fade_ms) {
    duck_percent = std::clamp(duck_percent, 0, 100);
    vad_threshold = std::clamp(finite_or(vad_threshold, 0.01), 0.0025, 0.2);
    if (mic_vad_threshold.has_value()) {
        if (!std::isfinite(*mic_vad_threshold)) {
            // An invalid optional value falls back to vad_threshold, matching
            // the "unset" behavior used by older configurations.
            mic_vad_threshold.reset();
        } else {
            mic_vad_threshold = std::clamp(*mic_vad_threshold, 0.0025, 0.2);
        }
    }
    hold_ms = std::clamp(hold_ms, 0, 4000);
    attack_fade_ms = std::clamp(attack_fade_ms, 0, 2000);
    release_fade_ms = std::clamp(release_fade_ms, 0, 3000);
}

void emit_settings(std::ostream& output, int duck_percent, double vad_threshold,
                   const std::optional<double>& mic_vad_threshold, int hold_ms, int attack_fade_ms,
                   int release_fade_ms, Trigger trigger, bool always_emit_mic_threshold) {
    output << "duck_percent = " << duck_percent << "\n"
           << "vad_threshold = " << format_double(vad_threshold, 4) << "\n";
    if (always_emit_mic_threshold || mic_vad_threshold.has_value()) {
        output << "mic_vad_threshold = "
               << format_double(mic_vad_threshold.value_or(vad_threshold), 4) << "\n";
    }
    output << "hold_ms = " << hold_ms << "\n"
           << "attack_fade_ms = " << attack_fade_ms << "\n"
           << "release_fade_ms = " << release_fade_ms << "\n"
           << "trigger = " << quote(trigger_name(trigger)) << "\n";
}

void emit_source(std::ostream& output, const ConfiguredSource& source) {
    const auto emit = [&output](const char* key, const std::optional<std::string>& value) {
        if (value.has_value()) {
            output << key << " = " << quote(*value) << "\n";
        }
    };
    emit("label", source.label);
    emit("application_name", source.application_name);
    emit("application_process_binary", source.application_process_binary);
    emit("media_name", source.media_name);
    emit("media_role", source.media_role);
    emit("media_class", source.media_class);
    emit("node_name", source.node_name);
}

void emit_mic(std::ostream& output, const ConfiguredMic& mic) {
    if (mic.name.has_value()) {
        output << "name = " << quote(*mic.name) << "\n";
    }
    if (mic.label.has_value()) {
        output << "label = " << quote(*mic.label) << "\n";
    }
}

} // namespace

std::string trigger_name(Trigger trigger) {
    switch (trigger) {
    case Trigger::Call:
        return "call";
    case Trigger::Mic:
        return "mic";
    case Trigger::Both:
        return "both";
    }
    return "call";
}

Trigger parse_trigger(const std::string& value) {
    const std::string lower = to_lower(trim(value));
    if (lower == "call" || lower == "voice" || lower == "call-stream")
        return Trigger::Call;
    if (lower == "mic" || lower == "microphone" || lower == "my-voice")
        return Trigger::Mic;
    if (lower == "both" || lower == "any" || lower == "call+mic" || lower == "mic+call") {
        return Trigger::Both;
    }
    throw std::runtime_error("invalid trigger '" + value + "' (expected call, mic or both)");
}

bool ConfiguredSource::has_match_fields() const {
    return application_name.has_value() || application_process_binary.has_value() ||
           media_name.has_value() || media_role.has_value() || media_class.has_value() ||
           node_name.has_value();
}

Config Config::defaults() {
    return Config{};
}

DuckingProfile DuckingProfile::from_config(const Config& config) {
    DuckingProfile profile;
    profile.duck_percent = config.duck_percent;
    profile.vad_threshold = config.vad_threshold;
    profile.mic_vad_threshold = config.mic_vad_threshold;
    profile.hold_ms = config.hold_ms;
    profile.attack_fade_ms = config.attack_fade_ms;
    profile.release_fade_ms = config.release_fade_ms;
    profile.trigger = config.trigger;
    profile.voice_source = config.voice_source;
    profile.mic_source = config.mic_source;
    return profile;
}

DuckingProfile DuckingProfile::normalized() const {
    DuckingProfile result = *this;
    normalize_settings(result.duck_percent, result.vad_threshold, result.mic_vad_threshold,
                       result.hold_ms, result.attack_fade_ms, result.release_fade_ms);
    return result;
}

void Config::apply_profile(const std::string& name) {
    const auto found = profiles.find(name);
    if (found == profiles.end()) {
        const std::string available = profile_list(profiles);
        throw std::runtime_error("unknown profile '" + name + "'" +
                                 (available.empty() ? std::string("; no profiles are configured")
                                                    : "; available profiles: " + available));
    }
    const DuckingProfile profile = found->second.normalized();
    duck_percent = profile.duck_percent;
    vad_threshold = profile.vad_threshold;
    mic_vad_threshold = profile.mic_vad_threshold;
    hold_ms = profile.hold_ms;
    attack_fade_ms = profile.attack_fade_ms;
    release_fade_ms = profile.release_fade_ms;
    trigger = profile.trigger;
    voice_source = profile.voice_source;
    mic_source = profile.mic_source;
    active_profile = name;
}

Config Config::normalized() const {
    Config result = *this;
    normalize_settings(result.duck_percent, result.vad_threshold, result.mic_vad_threshold,
                       result.hold_ms, result.attack_fade_ms, result.release_fade_ms);
    for (auto& [name, profile] : result.profiles) {
        validate_profile_name(name);
        profile = profile.normalized();
    }
    return result;
}

Config Config::load() {
    const auto path = config_path();
    if (!std::filesystem::exists(path)) {
        return Config::defaults();
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to read " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    try {
        return parse(buffer.str()).normalized();
    } catch (const std::exception& error) {
        throw std::runtime_error("invalid TOML in " + path.string() + ": " + error.what());
    }
}

Config Config::load_or_default() {
    if (!std::filesystem::exists(config_path())) {
        return Config::defaults();
    }
    return load();
}

void Config::save() const {
    const auto path = config_path();
    std::filesystem::create_directories(path.parent_path());
    const std::string text = serialize(normalized());

    // A unique temporary file in the destination directory avoids two tuners
    // writing the same sidecar file. All writers use the same atomic replace,
    // so concurrent saves are last-writer-wins but never expose a partial file.
    std::string template_path = path.string() + ".tmp.XXXXXX";
    std::vector<char> template_buffer(template_path.begin(), template_path.end());
    template_buffer.push_back('\0');
    const int fd = mkstemp(template_buffer.data());
    if (fd < 0) {
        throw std::runtime_error("failed to create temporary config file for " + path.string() +
                                 ": " + std::strerror(errno));
    }
    const std::string temporary(template_buffer.data());

    std::string write_error;
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = write(fd, text.data() + written, text.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            write_error = std::strerror(errno);
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    if (write_error.empty() && fsync(fd) != 0) {
        write_error = std::strerror(errno);
    }
    if (close(fd) != 0 && write_error.empty()) {
        write_error = std::strerror(errno);
    }
    if (!write_error.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("failed to write " + temporary + ": " + write_error);
    }

    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const std::string error = std::strerror(errno);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        // Never unlink the existing configuration as a fallback: preserving the
        // last known-good file is more important than completing this write.
        throw std::runtime_error("failed to replace " + path.string() + ": " + error);
    }

    // Best-effort directory sync so the rename survives a crash on filesystems
    // that require it. Failure here does not invalidate the new file.
    const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        fsync(directory);
        close(directory);
    }
}

std::string Config::serialize(const Config& config) {
    std::ostringstream output;
    output << "# pipeduck configuration\n"
           << "# https://github.com/ErRusso/pipeduck\n\n";
    if (config.active_profile.has_value()) {
        output << "active_profile = " << quote(*config.active_profile) << "\n";
    }
    emit_settings(output, config.duck_percent, config.vad_threshold, config.mic_vad_threshold,
                  config.hold_ms, config.attack_fade_ms, config.release_fade_ms, config.trigger,
                  true);

    if (config.voice_source.has_value()) {
        output << "\n[voice_source]\n";
        emit_source(output, *config.voice_source);
    }

    if (config.mic_source.has_value()) {
        output << "\n[mic_source]\n";
        emit_mic(output, *config.mic_source);
    }

    for (const auto& [name, profile] : config.profiles) {
        validate_profile_name(name);
        const DuckingProfile normalized = profile.normalized();
        output << "\n[profiles." << name << "]\n";
        emit_settings(output, normalized.duck_percent, normalized.vad_threshold,
                      normalized.mic_vad_threshold, normalized.hold_ms, normalized.attack_fade_ms,
                      normalized.release_fade_ms, normalized.trigger, false);
        if (normalized.voice_source.has_value()) {
            output << "\n[profiles." << name << ".voice_source]\n";
            emit_source(output, *normalized.voice_source);
        }
        if (normalized.mic_source.has_value()) {
            output << "\n[profiles." << name << ".mic_source]\n";
            emit_mic(output, *normalized.mic_source);
        }
    }
    return output.str();
}

Config Config::parse(const std::string& text) {
    Config config = Config::defaults();
    ConfiguredSource voice_source;
    ConfiguredMic mic_source;
    bool has_voice_source = false;
    bool has_mic_source = false;

    std::string section;
    std::string profile_name;
    enum class ProfilePart { Settings, Voice, Mic };
    ProfilePart profile_part = ProfilePart::Settings;
    std::map<std::string, ConfiguredSource> profile_voice;
    std::map<std::string, ConfiguredMic> profile_mic;
    std::map<std::string, bool> profile_has_voice;
    std::map<std::string, bool> profile_has_mic;

    const auto finish_profile = [&]() {
        if (profile_name.empty()) {
            return;
        }
        DuckingProfile& profile = config.profiles[profile_name];
        if (profile_has_voice[profile_name]) {
            profile.voice_source = profile_voice[profile_name];
        }
        if (profile_has_mic[profile_name]) {
            profile.mic_source = profile_mic[profile_name];
        }
    };

    const auto lines = split_lines(text);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const std::string line = trim(strip_comment(lines[index]));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                throw std::runtime_error("line " + std::to_string(line_number) +
                                         ": malformed table header");
            }
            const std::string next_section = trim(line.substr(1, line.size() - 2));
            if (next_section != section) {
                finish_profile();
                section = next_section;
                profile_name.clear();
                profile_part = ProfilePart::Settings;
            }
            const std::string prefix = "profiles.";
            if (section.rfind(prefix, 0) == 0) {
                const std::string rest = section.substr(prefix.size());
                std::string name = rest;
                const std::string voice_suffix = ".voice_source";
                const std::string mic_suffix = ".mic_source";
                if (rest.size() > voice_suffix.size() &&
                    rest.compare(rest.size() - voice_suffix.size(), voice_suffix.size(),
                                 voice_suffix) == 0) {
                    name = rest.substr(0, rest.size() - voice_suffix.size());
                    profile_part = ProfilePart::Voice;
                } else if (rest.size() > mic_suffix.size() &&
                           rest.compare(rest.size() - mic_suffix.size(), mic_suffix.size(),
                                        mic_suffix) == 0) {
                    name = rest.substr(0, rest.size() - mic_suffix.size());
                    profile_part = ProfilePart::Mic;
                }
                validate_profile_name(name);
                profile_name = name;
                config.profiles.try_emplace(name);
            } else if (section == "voice_source") {
                has_voice_source = true;
            } else if (section == "mic_source") {
                has_mic_source = true;
            }
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("line " + std::to_string(line_number) +
                                     ": expected key = value");
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("line " + std::to_string(line_number) +
                                     ": expected key = value");
        }

        if (!profile_name.empty()) {
            DuckingProfile& profile = config.profiles[profile_name];
            if (profile_part == ProfilePart::Voice) {
                profile_has_voice[profile_name] = true;
                parse_source_key(profile_voice[profile_name], key, value, line_number);
            } else if (profile_part == ProfilePart::Mic) {
                profile_has_mic[profile_name] = true;
                if (key == "name")
                    profile_mic[profile_name].name = unquote(value, line_number);
                else if (key == "label")
                    profile_mic[profile_name].label = unquote(value, line_number);
            } else if (key == "duck_percent") {
                profile.duck_percent = parse_int(value, line_number);
            } else if (key == "vad_threshold") {
                profile.vad_threshold = parse_double(value, line_number);
            } else if (key == "mic_vad_threshold") {
                profile.mic_vad_threshold = parse_double(value, line_number);
            } else if (key == "hold_ms") {
                profile.hold_ms = parse_int(value, line_number);
            } else if (key == "attack_fade_ms") {
                profile.attack_fade_ms = parse_int(value, line_number);
            } else if (key == "release_fade_ms") {
                profile.release_fade_ms = parse_int(value, line_number);
            } else if (key == "trigger") {
                profile.trigger = parse_trigger(unquote(value, line_number));
            }
            continue;
        }

        if (section.empty()) {
            if (key == "duck_percent")
                config.duck_percent = parse_int(value, line_number);
            else if (key == "vad_threshold")
                config.vad_threshold = parse_double(value, line_number);
            else if (key == "mic_vad_threshold")
                config.mic_vad_threshold = parse_double(value, line_number);
            else if (key == "hold_ms")
                config.hold_ms = parse_int(value, line_number);
            else if (key == "attack_fade_ms")
                config.attack_fade_ms = parse_int(value, line_number);
            else if (key == "release_fade_ms")
                config.release_fade_ms = parse_int(value, line_number);
            else if (key == "trigger")
                config.trigger = parse_trigger(unquote(value, line_number));
            else if (key == "active_profile") {
                const std::string name = unquote(value, line_number);
                validate_profile_name(name);
                config.active_profile = name;
            }
        } else if (section == "voice_source") {
            has_voice_source = true;
            parse_source_key(voice_source, key, value, line_number);
        } else if (section == "mic_source") {
            has_mic_source = true;
            if (key == "name")
                mic_source.name = unquote(value, line_number);
            else if (key == "label")
                mic_source.label = unquote(value, line_number);
        }
    }

    finish_profile();
    if (has_voice_source) {
        config.voice_source = voice_source;
    }
    if (has_mic_source) {
        config.mic_source = mic_source;
    }
    return config.normalized();
}

} // namespace pipeduck
