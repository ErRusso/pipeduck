#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "pipeduck/common.hpp"
#include "pipeduck/config.hpp"
#include "pipeduck/duck.hpp"
#include "pipeduck/identity.hpp"
#include "pipeduck/pulse.hpp"
#include "pipeduck/routing.hpp"
#include "pipeduck/vad.hpp"
#include "pipeduck/volume.hpp"

namespace {

int g_failures = 0;
std::string g_current_test;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::cerr << "FAIL [" << g_current_test << "] " << __FILE__ << ":" << __LINE__ << ": " \
                      << #condition << "\n";                                                       \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (false)

#define CHECK_THROW(expression)                                                                    \
    do {                                                                                           \
        bool thrown = false;                                                                       \
        try {                                                                                      \
            (void)(expression);                                                                    \
        } catch (const std::exception&) {                                                          \
            thrown = true;                                                                         \
        }                                                                                          \
        if (!thrown) {                                                                             \
            std::cerr << "FAIL [" << g_current_test << "] " << __FILE__ << ":" << __LINE__         \
                      << ": expected exception: " << #expression << "\n";                          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (false)

void test_common() {
    g_current_test = "common";
    CHECK(pipeduck::trim("  hello \n") == "hello");
    CHECK(pipeduck::to_lower("AbC") == "abc");
    CHECK(pipeduck::split("a,b,c", ',').size() == 3);
    CHECK(pipeduck::join({"a", "b", "c"}, "-") == "a-b-c");
    CHECK(pipeduck::starts_with("pipeduck", "pipe"));
    CHECK(!pipeduck::starts_with("pipe", "pipeduck"));
    CHECK(pipeduck::format_double(0.0100001, 4) == "0.0100");

    const auto result = pipeduck::run_command("sh", {"-c", "printf hello"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "hello");
    const auto failing = pipeduck::run_command("sh", {"-c", "echo oops >&2; exit 7"});
    CHECK(failing.exit_code == 7);
    CHECK(pipeduck::trim(failing.err) == "oops");

    const auto timed_out = pipeduck::run_command("sleep", {"5"}, std::chrono::milliseconds(100));
    CHECK(timed_out.timed_out);
    CHECK(timed_out.exit_code == 124);
}

void test_config() {
    g_current_test = "config";
    const std::string text = R"(
# comment
duck_percent = 33
vad_threshold = 0.0125
hold_ms = 900
attack_fade_ms = 40
release_fade_ms = 420
trigger = "both"

[voice_source]
label = "Discord / discord / call"
application_name = "Discord"
application_process_binary = "discord"
media_class = "Stream/Output/Audio"
node_name = "discord-call"

[mic_source]
name = "alsa_input.usb-Mic-00.pro-input-0"
label = "USB Microphone"
)";
    const pipeduck::Config config = pipeduck::Config::parse(text);
    CHECK(config.duck_percent == 33);
    CHECK(std::abs(config.vad_threshold - 0.0125) < 1e-9);
    CHECK(config.hold_ms == 900);
    CHECK(config.attack_fade_ms == 40);
    CHECK(config.release_fade_ms == 420);
    CHECK(config.trigger == pipeduck::Trigger::Both);
    CHECK(config.voice_source.has_value());
    CHECK(config.voice_source->application_name == std::optional<std::string>("Discord"));
    CHECK(config.mic_source.has_value());
    CHECK(config.mic_source->name ==
          std::optional<std::string>("alsa_input.usb-Mic-00.pro-input-0"));

    const std::string serialized = pipeduck::Config::serialize(config);
    const pipeduck::Config roundtrip = pipeduck::Config::parse(serialized);
    CHECK(roundtrip.duck_percent == config.duck_percent);
    CHECK(roundtrip.trigger == config.trigger);
    CHECK(roundtrip.voice_source == config.voice_source);
    CHECK(roundtrip.mic_source == config.mic_source);

    // A config written by the original Rust pw-duck must still load.
    const std::string legacy = R"(
duck_percent = 25
vad_threshold = 0.01
hold_ms = 700

[voice_source]
label = "app / bin / media"
application_name = "app"
media_class = "Stream/Output/Audio"
)";
    const pipeduck::Config legacy_config = pipeduck::Config::parse(legacy);
    CHECK(legacy_config.trigger == pipeduck::Trigger::Call);
    CHECK(legacy_config.release_fade_ms == 320);
    CHECK(legacy_config.voice_source.has_value());
    CHECK(!legacy_config.mic_vad_threshold.has_value());
    CHECK(std::abs(pipeduck::DuckingSettings::from_config(legacy_config).mic_vad_threshold -
                   legacy_config.vad_threshold) < 1e-9);

    const pipeduck::Config mic_thresholds = pipeduck::Config::parse(R"(
vad_threshold = 0.0100
mic_vad_threshold = 0.0250
)");
    CHECK(mic_thresholds.mic_vad_threshold.has_value());
    CHECK(std::abs(*mic_thresholds.mic_vad_threshold - 0.025) < 1e-9);
    const auto mic_settings = pipeduck::DuckingSettings::from_config(mic_thresholds);
    CHECK(std::abs(mic_settings.mic_vad_threshold - 0.025) < 1e-9);
    CHECK(mic_settings.vad_threshold != mic_settings.mic_vad_threshold);

    CHECK(pipeduck::parse_trigger("MIC") == pipeduck::Trigger::Mic);
    CHECK(pipeduck::parse_trigger("call+mic") == pipeduck::Trigger::Both);
    CHECK_THROW(pipeduck::parse_trigger("nonsense"));

    pipeduck::Config clamped = config;
    clamped.duck_percent = 400;
    clamped.vad_threshold = 99.0;
    clamped.mic_vad_threshold = -1.0;
    clamped.hold_ms = -3;
    clamped.release_fade_ms = 99999;
    const auto normalized = clamped.normalized();
    CHECK(normalized.duck_percent == 100);
    CHECK(std::abs(normalized.vad_threshold - 0.2) < 1e-9);
    CHECK(std::abs(*normalized.mic_vad_threshold - 0.0025) < 1e-9);
    CHECK(normalized.hold_ms == 0);
    CHECK(normalized.release_fade_ms == 3000);

    const std::string serialized_thresholds = pipeduck::Config::serialize(mic_thresholds);
    CHECK(serialized_thresholds.find("mic_vad_threshold = 0.0250") != std::string::npos);
    const auto threshold_roundtrip = pipeduck::Config::parse(serialized_thresholds);
    CHECK(threshold_roundtrip.mic_vad_threshold.has_value());
    CHECK(std::abs(*threshold_roundtrip.mic_vad_threshold - 0.025) < 1e-9);
}

void test_config_file_roundtrip() {
    g_current_test = "config-file";
    const auto temporary = std::filesystem::temp_directory_path() / "pipeduck-test-config";
    std::filesystem::remove_all(temporary);
    std::filesystem::create_directories(temporary);
    setenv("XDG_CONFIG_HOME", temporary.c_str(), 1);

    pipeduck::Config config = pipeduck::Config::defaults();
    config.duck_percent = 42;
    config.trigger = pipeduck::Trigger::Mic;
    config.save();
    CHECK(std::filesystem::exists(pipeduck::config_path()));
    const pipeduck::Config loaded = pipeduck::Config::load();
    CHECK(loaded.duck_percent == 42);
    CHECK(loaded.trigger == pipeduck::Trigger::Mic);

    std::filesystem::remove_all(temporary);
    unsetenv("XDG_CONFIG_HOME");
}

void test_identity() {
    g_current_test = "identity";
    pipeduck::StringMap props{
        {"application.name", "Discord"}, {"application.process.binary", "discord"},
        {"media.name", "playStream"},    {"media.class", "Stream/Output/Audio"},
        {"node.name", "discord-call"},
    };
    const auto identity = pipeduck::AudioIdentity::from_props(props);
    CHECK(identity.is_playback_stream());
    CHECK(identity.looks_like_voice_source());

    pipeduck::ConfiguredSource source;
    source.application_name = "Discord";
    source.media_class = "Stream/Output/Audio";
    CHECK(identity.matches_configured_source(source));

    // Discord's WEBRTC sub-stream has a different application name but the same binary.
    pipeduck::ConfiguredSource discord;
    discord.application_name = "Discord";
    discord.application_process_binary = "discord";
    discord.media_class = "Stream/Output/Audio";
    pipeduck::StringMap webrtc_props{
        {"application.name", "WEBRTC VoiceEngine"},
        {"application.process.binary", "discord"},
        {"media.name", "playStream"},
        {"media.class", "Stream/Output/Audio"},
    };
    CHECK(pipeduck::AudioIdentity::from_props(webrtc_props).matches_configured_source(discord));

    pipeduck::ConfiguredSource wrong_binary;
    wrong_binary.application_process_binary = "chromium";
    CHECK(!identity.matches_configured_source(wrong_binary));

    CHECK(pipeduck::looks_like_voice_text({"something", "WEBRTC"}));
    CHECK(!pipeduck::looks_like_voice_text({"music", "video"}));
}

void test_call_target() {
    g_current_test = "call-target";

    pipeduck::SinkInput input;
    input.index = 42;
    input.properties = {
        {"application.name", "Discord"},
        {"application.process.binary", "discord"},
        {"media.name", "playStream"},
        {"media.class", "Stream/Output/Audio"},
        {"object.serial", "99"},
        {"node.name", "discord-call"},
    };
    pipeduck::ConfiguredSource source;
    source.application_name = "Discord";
    source.media_class = "Stream/Output/Audio";

    const auto target = pipeduck::find_call_target({input}, source);
    CHECK(target.has_value());
    if (target.has_value()) {
        CHECK(target->label == "#42 Discord / discord / playStream");
        CHECK(target->capture.label == target->label);
        CHECK(target->capture.object_serial == std::optional<std::string>("99"));
        CHECK(target->capture.node_name == std::optional<std::string>("discord-call"));
        CHECK(target->capture.sink_monitor);
    }

    pipeduck::ConfiguredSource missing;
    missing.application_name = "Not Running";
    CHECK(!pipeduck::find_call_target({input}, missing).has_value());
}

void test_vad() {
    g_current_test = "vad";
    using namespace std::chrono_literals;
    {
        pipeduck::VadState vad;
        CHECK(vad.step(0.5, 0.1, 0ms, 0ms));
    }
    {
        pipeduck::VadState vad;
        CHECK(vad.step(0.5, 0.1, 0ms, 0ms));
        CHECK(!vad.step(0.0, 0.1, 0ms, 0ms));
    }
    {
        pipeduck::VadState vad;
        CHECK(!vad.step(0.05, 0.1, 0ms, 0ms));
    }
    {
        pipeduck::VadState vad;
        CHECK(!vad.step(0.015, 0.01, 0ms, 0ms));
        CHECK(vad.step(0.021, 0.01, 0ms, 0ms));
    }
    {
        pipeduck::VadState vad;
        CHECK(vad.step(0.03, 0.01, 0ms, 0ms));
        CHECK(vad.step(0.008, 0.01, 0ms, 0ms));
        CHECK(!vad.step(0.007, 0.01, 0ms, 0ms));
    }
    {
        pipeduck::VadState vad;
        CHECK(!vad.step(1.0, 0.2, 0ms, 0ms));
        CHECK(vad.step(1.0, 0.199, 0ms, 0ms));
        CHECK(!vad.step(1.0, 0.2, 0ms, 0ms));
    }
    {
        pipeduck::VadState vad;
        CHECK(!vad.step(0.5, 0.1, 50ms, 0ms));
        std::this_thread::sleep_for(60ms);
        CHECK(vad.step(0.5, 0.1, 50ms, 0ms));
        std::this_thread::sleep_for(30ms);
        CHECK(!vad.step(0.0, 0.1, 50ms, 20ms));
    }
}

void test_routing_ports() {
    g_current_test = "routing-ports";
    {
        const auto links = pipeduck::monitor_links_for_ports(
            "pipeduck-1:monitor_FL\npipeduck-1:monitor_FR\n",
            "alsa_output.foo:playback_FL\nalsa_output.foo:playback_FR\n", "pipeduck-1",
            "alsa_output.foo");
        CHECK(links.size() == 2);
        CHECK(links[0].output == "pipeduck-1:monitor_FL");
        CHECK(links[0].input == "alsa_output.foo:playback_FL");
        CHECK(links[1].input == "alsa_output.foo:playback_FR");
    }
    {
        const auto links = pipeduck::monitor_links_for_ports(
            "pipeduck-1:monitor_FL\npipeduck-1:monitor_FR\n",
            "alsa_output.pro:playback_AUX0\nalsa_output.pro:playback_AUX1\n", "pipeduck-1",
            "alsa_output.pro");
        CHECK(links.size() == 2);
        CHECK(links[0].input == "alsa_output.pro:playback_AUX0");
        CHECK(links[1].input == "alsa_output.pro:playback_AUX1");
    }
    {
        const auto links = pipeduck::monitor_links_for_ports(
            "pipeduck-1:monitor_FL\npipeduck-1:monitor_FR\n",
            "alsa_output.weird:playback_X\nalsa_output.weird:playback_Y\n"
            "alsa_output.weird:playback_Z\n",
            "pipeduck-1", "alsa_output.weird");
        CHECK(links.size() == 2);
        CHECK(links[0].input == "alsa_output.weird:playback_X");
        CHECK(links[1].input == "alsa_output.weird:playback_Y");
    }
    CHECK_THROW(pipeduck::monitor_links_for_ports("", "", "a", "b"));
}

void test_pulse_json() {
    g_current_test = "pulse-json";
    const std::string sinks = R"([
      {"index": 65, "name": "alsa_output.usb", "description": "USB Headset",
       "properties": {"node.name": "alsa_output.usb"}}
    ])";
    const auto parsed_sinks = pipeduck::parse_sinks_json(sinks);
    CHECK(parsed_sinks.size() == 1);
    CHECK(parsed_sinks[0].index == 65);
    CHECK(parsed_sinks[0].name == "alsa_output.usb");
    CHECK(parsed_sinks[0].description == "USB Headset");

    const std::string inputs = R"([
      {"index": 12, "sink": 65,
       "properties": {"application.name": "Discord", "media.class": "Stream/Output/Audio",
                      "object.serial": "42"}}
    ])";
    const auto parsed_inputs = pipeduck::parse_sink_inputs_json(inputs);
    CHECK(parsed_inputs.size() == 1);
    CHECK(parsed_inputs[0].index == 12);
    CHECK(parsed_inputs[0].sink == 65);
    CHECK(parsed_inputs[0].identity().application_name == std::optional<std::string>("Discord"));
    CHECK(parsed_inputs[0].properties.at("object.serial") == "42");

    const std::string sources = R"([
      {"index": 3, "name": "alsa_input.mic", "description": "Microphone",
       "properties": {"node.name": "alsa_input.mic"}},
      {"index": 4, "name": "alsa_output.usb.monitor", "description": "Monitor"}
    ])";
    const auto parsed_sources = pipeduck::parse_sources_json(sources);
    CHECK(parsed_sources.size() == 2);
    CHECK(!parsed_sources[0].is_monitor);
    CHECK(parsed_sources[1].is_monitor);
    CHECK(parsed_sources[0].properties.at("node.name") == "alsa_input.mic");

    CHECK(pipeduck::parse_sink_inputs_json("[]").empty());
    CHECK_THROW(pipeduck::parse_sinks_json("not json"));
}

void test_ramp() {
    g_current_test = "volume-ramp";
    CHECK(std::abs(pipeduck::smoothstep(0.0) - 0.0) < 1e-9);
    CHECK(std::abs(pipeduck::smoothstep(0.5) - 0.5) < 1e-9);
    CHECK(std::abs(pipeduck::smoothstep(1.0) - 1.0) < 1e-9);
    CHECK(std::abs(pipeduck::smoothstep(-1.0) - 0.0) < 1e-9);
    CHECK(std::abs(pipeduck::smoothstep(2.0) - 1.0) < 1e-9);

    std::vector<double> values;
    pipeduck::run_volume_ramp(100.0, 25.0, 80, 20, nullptr,
                              [&values](double value) { values.push_back(value); });
    CHECK(values.size() >= 5);
    CHECK(std::abs(values.front() - 100.0) > 0.1);
    CHECK(std::abs(values.back() - 25.0) < 1e-9);
    for (std::size_t i = 1; i < values.size(); ++i) {
        CHECK(values[i] <= values[i - 1] + 1e-9);
    }

    std::vector<double> immediate;
    pipeduck::run_volume_ramp(100.0, 25.0, 0, 20, nullptr,
                              [&immediate](double value) { immediate.push_back(value); });
    CHECK(immediate.size() == 1);
    CHECK(std::abs(immediate[0] - 25.0) < 1e-9);
}

void test_nonblocking_ramp() {
    g_current_test = "nonblocking-ramp";
    using namespace std::chrono_literals;
    using Clock = pipeduck::VolumeRamp::Clock;

    const auto start_time = Clock::now();
    pipeduck::VolumeRamp ramp;
    ramp.start(100.0, 25.0, 1000, start_time);
    CHECK(ramp.active());
    CHECK(std::abs(ramp.current() - 100.0) < 1e-9);
    CHECK(std::abs(ramp.update(start_time + 500ms) - 62.5) < 1e-6);

    // A release fade can be interrupted and restarted from the current volume.
    const double interrupted = ramp.update(start_time + 700ms);
    CHECK(interrupted < 100.0 && interrupted > 25.0);
    ramp.start(interrupted, 25.0, 1000, start_time + 700ms);
    CHECK(ramp.active());
    const double shortly_after = ramp.update(start_time + 750ms);
    CHECK(shortly_after <= interrupted + 1e-9);
    CHECK(shortly_after >= 25.0);
    const double after = ramp.update(start_time + 800ms);
    CHECK(after <= shortly_after + 1e-9);
    CHECK(std::abs(ramp.update(start_time + 1800ms) - 25.0) < 1e-9);
    CHECK(!ramp.active());

    pipeduck::VolumeRamp immediate;
    immediate.start(100.0, 25.0, 0, start_time);
    CHECK(!immediate.active());
    CHECK(std::abs(immediate.current() - 25.0) < 1e-9);
}

void test_settings() {
    g_current_test = "settings";
    pipeduck::DuckingSettings settings;
    settings.duck_percent = 140;
    settings.vad_threshold = 0.0;
    settings.mic_vad_threshold = 0.5;
    settings.hold_ms = 9000;
    settings.release_fade_ms = 5000;
    const auto normalized = settings.normalized();
    CHECK(normalized.duck_percent == 100);
    CHECK(std::abs(normalized.vad_threshold - 0.0025) < 1e-9);
    CHECK(std::abs(normalized.mic_vad_threshold - 0.2) < 1e-9);
    CHECK(normalized.hold_ms == 4000);
    CHECK(normalized.release_fade_ms == 3000);

    pipeduck::SettingsStore store(normalized);
    CHECK(store.get().duck_percent == 100);
}

} // namespace

int main() {
    test_common();
    test_config();
    test_config_file_roundtrip();
    test_identity();
    test_call_target();
    test_vad();
    test_routing_ports();
    test_pulse_json();
    test_ramp();
    test_nonblocking_ramp();
    test_settings();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All pipeduck tests passed\n";
    return 0;
}
