#pragma once

#include <optional>
#include <string>

#include "pipeduck/common.hpp"

namespace pipeduck {

enum class Trigger { Call, Mic, Both };

std::string trigger_name(Trigger trigger);
Trigger parse_trigger(const std::string& value);

struct ConfiguredSource {
    std::optional<std::string> label;
    std::optional<std::string> application_name;
    std::optional<std::string> application_process_binary;
    std::optional<std::string> media_name;
    std::optional<std::string> media_role;
    std::optional<std::string> media_class;
    std::optional<std::string> node_name;

    bool has_match_fields() const;
    bool operator==(const ConfiguredSource&) const = default;
};

struct ConfiguredMic {
    std::optional<std::string> name;
    std::optional<std::string> label;
    bool operator==(const ConfiguredMic&) const = default;
};

struct Config {
    int duck_percent = 25;
    double vad_threshold = 0.01;
    int hold_ms = 700;
    int attack_fade_ms = 80;
    int release_fade_ms = 320;
    Trigger trigger = Trigger::Call;
    std::optional<ConfiguredSource> voice_source;
    std::optional<ConfiguredMic> mic_source;

    Config normalized() const;

    static Config defaults();
    static Config load_or_default();
    static Config load();
    void save() const;

    static std::string serialize(const Config& config);
    static Config parse(const std::string& text);
};

} // namespace pipeduck
