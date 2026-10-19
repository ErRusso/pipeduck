#include "pipeduck/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

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
    try {
        std::size_t consumed = 0;
        const double result = std::stod(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error("line " + std::to_string(line_number) + ": expected a number");
    }
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

Config Config::normalized() const {
    Config result = *this;
    result.duck_percent = std::clamp(result.duck_percent, 0, 100);
    result.vad_threshold = std::clamp(result.vad_threshold, 0.0025, 0.2);
    if (result.mic_vad_threshold.has_value()) {
        result.mic_vad_threshold = std::clamp(*result.mic_vad_threshold, 0.0025, 0.2);
    }
    result.hold_ms = std::clamp(result.hold_ms, 0, 4000);
    result.attack_fade_ms = std::clamp(result.attack_fade_ms, 0, 2000);
    result.release_fade_ms = std::clamp(result.release_fade_ms, 0, 3000);
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
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to write " + temporary);
        }
        output << text;
        output.flush();
        if (!output) {
            throw std::runtime_error("failed to write " + temporary);
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        std::filesystem::rename(temporary, path);
    }
}

std::string Config::serialize(const Config& config) {
    std::ostringstream output;
    output << "# pipeduck configuration\n"
           << "# https://github.com/ErRusso/pipeduck\n\n"
           << "duck_percent = " << config.duck_percent << "\n"
           << "vad_threshold = " << format_double(config.vad_threshold, 4) << "\n"
           << "mic_vad_threshold = "
           << format_double(config.mic_vad_threshold.value_or(config.vad_threshold), 4) << "\n"
           << "hold_ms = " << config.hold_ms << "\n"
           << "attack_fade_ms = " << config.attack_fade_ms << "\n"
           << "release_fade_ms = " << config.release_fade_ms << "\n"
           << "trigger = " << quote(trigger_name(config.trigger)) << "\n";

    if (config.voice_source.has_value()) {
        const auto& source = *config.voice_source;
        output << "\n[voice_source]\n";
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

    if (config.mic_source.has_value()) {
        const auto& mic = *config.mic_source;
        output << "\n[mic_source]\n";
        if (mic.name.has_value()) {
            output << "name = " << quote(*mic.name) << "\n";
        }
        if (mic.label.has_value()) {
            output << "label = " << quote(*mic.label) << "\n";
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
            section = trim(line.substr(1, line.size() - 2));
            if (section == "voice_source")
                has_voice_source = true;
            if (section == "mic_source")
                has_mic_source = true;
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

    if (has_voice_source) {
        config.voice_source = voice_source;
    }
    if (has_mic_source) {
        config.mic_source = mic_source;
    }
    return config.normalized();
}

} // namespace pipeduck
