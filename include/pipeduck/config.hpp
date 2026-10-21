#pragma once

#include <map>
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

// A named preset bundling the ducking settings and routing selection. Profiles
// are stored in the config file; applying one copies its values into the
// top-level keys so the existing SettingsStore/config-reload path remains the
// only source of truth.
struct Config;

struct DuckingProfile {
    int duck_percent = 25;
    double vad_threshold = 0.01;
    std::optional<double> mic_vad_threshold;
    int hold_ms = 700;
    int attack_fade_ms = 80;
    int release_fade_ms = 320;
    Trigger trigger = Trigger::Call;
    std::optional<ConfiguredSource> voice_source;
    std::optional<ConfiguredMic> mic_source;

    static DuckingProfile from_config(const Config& config);
    DuckingProfile normalized() const;
    bool operator==(const DuckingProfile&) const = default;
};

struct Config {
    int duck_percent = 25;
    double vad_threshold = 0.01;
    // Optional microphone-specific threshold. When unset, vad_threshold is used
    // so configurations written by older versions behave exactly as before.
    std::optional<double> mic_vad_threshold;
    int hold_ms = 700;
    int attack_fade_ms = 80;
    int release_fade_ms = 320;
    Trigger trigger = Trigger::Call;
    std::optional<ConfiguredSource> voice_source;
    std::optional<ConfiguredMic> mic_source;
    // Name of the last profile copied into the top-level settings, if any.
    std::optional<std::string> active_profile;
    std::map<std::string, DuckingProfile> profiles;

    Config normalized() const;

    // Replace the top-level settings with the named profile. Throws with an
    // actionable message when the profile does not exist.
    void apply_profile(const std::string& name);

    static Config defaults();
    static Config load_or_default();
    static Config load();
    void save() const;

    static std::string serialize(const Config& config);
    static Config parse(const std::string& text);
};

} // namespace pipeduck
