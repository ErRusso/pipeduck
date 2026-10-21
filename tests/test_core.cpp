#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pipeduck/cli.hpp"
#include "pipeduck/common.hpp"
#include "pipeduck/config.hpp"
#include "pipeduck/duck.hpp"
#include "pipeduck/identity.hpp"
#include "pipeduck/pulse.hpp"
#include "pipeduck/routing.hpp"
#include "pipeduck/tray_state.hpp"
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

pipeduck::CliOptions parse_arguments(std::initializer_list<std::string> values) {
    std::vector<std::string> storage(values);
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& value : storage) {
        argv.push_back(value.data());
    }
    return pipeduck::parse_cli(static_cast<int>(argv.size()), argv.data());
}

struct FakeRoutingSink final : pipeduck::RoutingSink {
    FakeRoutingSink(std::string name, std::function<void()> on_destroy,
                    std::function<void()> on_abandon)
        : name_(std::move(name)), on_destroy_(std::move(on_destroy)),
          on_abandon_(std::move(on_abandon)) {}

    const std::string& name() const override {
        return name_;
    }
    void destroy() override {
        on_destroy_();
    }
    void abandon() override {
        on_abandon_();
    }

private:
    std::string name_;
    std::function<void()> on_destroy_;
    std::function<void()> on_abandon_;
};

struct FakeRoutingVolume final : pipeduck::RoutingVolume {
    bool available() const override {
        return available_result;
    }
    bool set_percent(const std::string&, double, bool allow_subprocess_fallback) override {
        ++calls;
        last_allow_subprocess_fallback = allow_subprocess_fallback;
        return result;
    }

    int calls = 0;
    bool result = true;
    bool available_result = true;
    bool last_allow_subprocess_fallback = true;
};

struct FakeRoutingBackend final : pipeduck::RoutingBackend {
    explicit FakeRoutingBackend(std::string virtual_name = "pipeduck-1")
        : virtual_sink_name(std::move(virtual_name)) {
        refresh_port_lists();
    }

    void refresh_port_lists() {
        output_ports = virtual_sink_name + ":monitor_FL\n" + virtual_sink_name + ":monitor_FR\n";
        input_ports = real_sink_name + ":playback_FL\n" + real_sink_name + ":playback_FR\n";
    }

    std::string links_listing() const {
        std::string listing;
        for (const auto& link : links) {
            listing += link.output + "\n  |-> " + link.input + "\n";
        }
        return listing;
    }

    std::string default_sink_name() override {
        return real_sink_name;
    }
    std::optional<pipeduck::Sink> sink_by_name(const std::string& name) override {
        if (fail_sink_by_name) {
            throw std::runtime_error("simulated backend unavailable");
        }
        if (name == real_sink_name) {
            return pipeduck::Sink{real_sink_index, real_sink_name, "Real output"};
        }
        if (name == virtual_sink_name && sink_present) {
            return pipeduck::Sink{virtual_sink_index, virtual_sink_name, "Virtual sink"};
        }
        return std::nullopt;
    }
    std::optional<pipeduck::Sink> sink_by_index(uint32_t index) override {
        if (index == real_sink_index) {
            return pipeduck::Sink{real_sink_index, real_sink_name, "Real output"};
        }
        if (index == virtual_sink_index && sink_present) {
            return pipeduck::Sink{virtual_sink_index, virtual_sink_name, "Virtual sink"};
        }
        return std::nullopt;
    }
    pipeduck::Sink wait_for_sink(const std::string& name, std::chrono::milliseconds) override {
        ++wait_for_sink_calls;
        if (!sink_present) {
            throw std::runtime_error("sink " + name + " did not appear");
        }
        return pipeduck::Sink{virtual_sink_index, name, "Virtual sink"};
    }
    std::vector<pipeduck::SinkInput> sink_inputs() override {
        ++sink_input_calls;
        if (fail_sink_inputs) {
            throw std::runtime_error("simulated backend unavailable");
        }
        return inputs;
    }
    void set_default_sink(const std::string& name) override {
        default_sink_set_to = name;
    }
    void move_sink_input(uint32_t input_index, const std::string& sink_name) override {
        ++move_calls;
        if (fail_move_to_name_calls > 0) {
            --fail_move_to_name_calls;
            throw std::runtime_error("simulated move by name failure");
        }
        auto found = find_input(input_index);
        if (found == nullptr) {
            throw std::runtime_error("no such sink-input");
        }
        if (sink_name == virtual_sink_name) {
            found->sink = virtual_sink_index;
        } else if (sink_name == real_sink_name) {
            found->sink = real_sink_index;
        } else {
            throw std::runtime_error("no such sink");
        }
    }
    void move_sink_input_to_index(uint32_t input_index, uint32_t sink_index) override {
        ++move_calls;
        if (fail_move_to_index_calls > 0) {
            --fail_move_to_index_calls;
            throw std::runtime_error("simulated move by index failure");
        }
        auto found = find_input(input_index);
        if (found == nullptr) {
            throw std::runtime_error("no such sink-input");
        }
        found->sink = sink_index;
    }

    std::unique_ptr<pipeduck::RoutingSink> create_sink(const std::string&) override {
        ++create_sink_calls;
        sink_destroyed = false;
        sink_abandoned = false;
        return std::make_unique<FakeRoutingSink>(
            virtual_sink_name,
            [this] {
                sink_destroyed = true;
                sink_present = false;
                links.clear();
            },
            [this] { sink_abandoned = true; });
    }
    std::unique_ptr<pipeduck::RoutingVolume> create_volume() override {
        ++create_volume_calls;
        auto volume = std::make_unique<FakeRoutingVolume>();
        last_volume = volume.get();
        return volume;
    }

    std::string list_output_ports() override {
        return output_ports;
    }
    std::string list_input_ports() override {
        return input_ports;
    }
    std::string list_links() override {
        if (fail_list_links_calls > 0) {
            --fail_list_links_calls;
            throw std::runtime_error("simulated pw-link -l failure");
        }
        return links_listing();
    }
    void create_link(const pipeduck::PortLink& link) override {
        ++create_link_calls;
        if (!fail_create_input.empty() && link.input == fail_create_input &&
            fail_create_input_calls > 0) {
            --fail_create_input_calls;
            throw std::runtime_error("simulated pw-link failure for " + link.input);
        }
        if (std::find(links.begin(), links.end(), link) != links.end()) {
            throw std::runtime_error("link already exists: " + link.output + " -> " + link.input);
        }
        links.push_back(link);
    }
    void remove_link(const pipeduck::PortLink& link) override {
        ++remove_link_calls;
        if (fail_remove_link_calls > 0) {
            --fail_remove_link_calls;
            throw std::runtime_error("simulated unlink failure");
        }
        links.erase(std::remove(links.begin(), links.end(), link), links.end());
    }

    pipeduck::SinkInput* find_input(uint32_t index) {
        const auto found = std::find_if(inputs.begin(), inputs.end(), [index](const auto& input) {
            return input.index == index;
        });
        return found == inputs.end() ? nullptr : &*found;
    }

    std::string real_sink_name = "real";
    uint32_t real_sink_index = 1;
    std::string virtual_sink_name;
    uint32_t virtual_sink_index = 2;
    bool sink_present = true;
    bool sink_destroyed = false;
    bool sink_abandoned = false;
    bool fail_sink_inputs = false;
    bool fail_sink_by_name = false;
    int fail_list_links_calls = 0;
    std::string fail_create_input;
    int fail_create_input_calls = 0;
    int fail_remove_link_calls = 0;
    int fail_move_to_index_calls = 0;
    int fail_move_to_name_calls = 0;

    std::string output_ports;
    std::string input_ports;
    std::vector<pipeduck::PortLink> links;
    std::vector<pipeduck::SinkInput> inputs;
    std::string default_sink_set_to;

    int wait_for_sink_calls = 0;
    int sink_input_calls = 0;
    int move_calls = 0;
    int create_sink_calls = 0;
    int create_volume_calls = 0;
    int create_link_calls = 0;
    int remove_link_calls = 0;
    FakeRoutingVolume* last_volume = nullptr;
};

pipeduck::RoutingOptions fast_routing_options() {
    pipeduck::RoutingOptions options;
    options.sink_appear_timeout = std::chrono::milliseconds(0);
    options.port_appear_timeout = std::chrono::milliseconds(0);
    options.restore_timeout = std::chrono::milliseconds(0);
    options.retry_interval = std::chrono::milliseconds(1);
    options.volume_retry_initial = std::chrono::milliseconds(5);
    options.volume_retry_max = std::chrono::milliseconds(10);
    return options;
}

pipeduck::SinkInput playback_input(uint32_t index, uint32_t sink, const std::string& serial,
                                   const std::string& app = "Discord") {
    pipeduck::SinkInput input;
    input.index = index;
    input.sink = sink;
    input.properties = {
        {"application.name", app},
        {"media.class", "Stream/Output/Audio"},
        {"object.serial", serial},
    };
    return input;
}

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

    // The child closes both pipes immediately and then hangs. The timeout must
    // still be enforced after the pipes are gone.
    const auto start = std::chrono::steady_clock::now();
    const auto closed_pipes = pipeduck::run_command("sh", {"-c", "exec >/dev/null 2>&1; sleep 5"},
                                                    std::chrono::milliseconds(150));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    CHECK(closed_pipes.timed_out);
    CHECK(closed_pipes.exit_code == 124);
    CHECK(elapsed < 2000);

    bool timeout_message = false;
    try {
        (void)pipeduck::run_command_checked("sh", {"-c", "exec >/dev/null 2>&1; sleep 5"},
                                            std::chrono::milliseconds(100));
    } catch (const std::exception& error) {
        timeout_message =
            std::string(error.what()).find("timed out after 100 ms") != std::string::npos;
    }
    CHECK(timeout_message);

    CHECK_THROW(pipeduck::run_command_checked("pipeduck-no-such-executable", {},
                                              std::chrono::milliseconds(100)));

    const auto signaled =
        pipeduck::run_command("sh", {"-c", "kill -TERM $$"}, std::chrono::milliseconds(1000));
    CHECK(signaled.exit_code == 128 + SIGTERM);
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

    for (const char* value : {"nan", "NaN", "inf", "-inf", "infinity"}) {
        CHECK_THROW(pipeduck::Config::parse(std::string("vad_threshold = ") + value));
        CHECK_THROW(pipeduck::Config::parse(std::string("mic_vad_threshold = ") + value));
    }
    CHECK_THROW(pipeduck::Config::parse("vad_threshold = 0.01 trailing"));
    CHECK_THROW(pipeduck::Config::parse("vad_threshold ="));

    pipeduck::Config nonfinite = pipeduck::Config::defaults();
    nonfinite.vad_threshold = std::nan("");
    nonfinite.mic_vad_threshold = std::numeric_limits<double>::infinity();
    const auto repaired = nonfinite.normalized();
    CHECK(std::isfinite(repaired.vad_threshold));
    CHECK(!repaired.mic_vad_threshold.has_value());

    pipeduck::DuckingSettings nonfinite_settings;
    nonfinite_settings.vad_threshold = std::nan("");
    nonfinite_settings.mic_vad_threshold = -std::numeric_limits<double>::infinity();
    const auto repaired_settings = nonfinite_settings.normalized();
    CHECK(std::isfinite(repaired_settings.vad_threshold));
    CHECK(std::isfinite(repaired_settings.mic_vad_threshold));

    CHECK_THROW(parse_arguments({"pipeduck", "--vad-threshold", "nan"}));
    CHECK_THROW(parse_arguments({"pipeduck", "--mic-vad-threshold", "inf"}));
    CHECK_THROW(parse_arguments({"pipeduck", "--vad-threshold", "0.01junk"}));
    const auto parsed_cli = parse_arguments({"pipeduck", "--vad-threshold", "0.02"});
    CHECK(parsed_cli.vad_threshold.has_value());
    CHECK(std::abs(*parsed_cli.vad_threshold - 0.02) < 1e-9);

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

    // Several writers using the same config path must never expose a partial
    // file or leave a temporary sidecar behind.
    std::atomic<int> save_failures{0};
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 4; ++writer) {
        writers.emplace_back([writer, &save_failures]() {
            for (int iteration = 0; iteration < 25; ++iteration) {
                try {
                    pipeduck::Config concurrent = pipeduck::Config::defaults();
                    concurrent.duck_percent = (writer * 25 + iteration) % 101;
                    concurrent.save();
                } catch (const std::exception& error) {
                    std::cerr << "concurrent save failed: " << error.what() << "\n";
                    ++save_failures;
                }
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    CHECK(save_failures.load() == 0);
    for (int read = 0; read < 50; ++read) {
        try {
            const auto concurrent = pipeduck::Config::load();
            CHECK(concurrent.duck_percent >= 0 && concurrent.duck_percent <= 100);
        } catch (const std::exception& error) {
            std::cerr << "concurrent load failed: " << error.what() << "\n";
            CHECK(false);
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(temporary / "pipeduck")) {
        CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
    }

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
        {"application.name", "Discord"}, {"application.process.binary", "discord"},
        {"media.name", "playStream"},    {"media.class", "Stream/Output/Audio"},
        {"object.serial", "99"},         {"node.name", "discord-call"},
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

void test_routing_links() {
    g_current_test = "routing-links";
    const std::string listing = "pipeduck-1:monitor_FL\n"
                                "  |-> alsa_output.foo:playback_FL\n"
                                "pipeduck-1:monitor_FR\n"
                                "  |-> alsa_output.foo:playback_FR\n"
                                "alsa_output.foo:playback_AUX0\n"
                                "  |<- pipeduck-2:monitor_FL\n";
    const auto parsed = pipeduck::parse_link_list(listing);
    CHECK(parsed.size() == 3);
    CHECK(
        (parsed[0] == pipeduck::PortLink{"pipeduck-1:monitor_FL", "alsa_output.foo:playback_FL"}));
    CHECK(
        (parsed[1] == pipeduck::PortLink{"pipeduck-1:monitor_FR", "alsa_output.foo:playback_FR"}));
    // A `|<-` row under an input port still normalizes to output -> input.
    CHECK((parsed[2] ==
           pipeduck::PortLink{"pipeduck-2:monitor_FL", "alsa_output.foo:playback_AUX0"}));
    CHECK(pipeduck::parse_link_list("").empty());
    CHECK(pipeduck::parse_link_list("  |-> orphan\n").empty());

    const std::vector<pipeduck::PortLink> desired{
        {"pipeduck-1:monitor_FL", "alsa_output.foo:playback_FL"},
        {"pipeduck-1:monitor_FR", "alsa_output.foo:playback_FR"},
    };
    const std::vector<pipeduck::PortLink> existing{
        {"pipeduck-1:monitor_FL", "alsa_output.foo:playback_FL"},
        {"unrelated:out", "unrelated:in"},
    };
    const auto missing = pipeduck::missing_links(desired, existing);
    CHECK(missing.size() == 1);
    CHECK((missing[0] == desired[1]));
    CHECK(pipeduck::missing_links(desired, desired).empty());
}

void test_routing_link_plan() {
    g_current_test = "routing-link-plan";
    const pipeduck::PortLink left{"pipeduck-1:monitor_FL", "alsa_output.foo:playback_FL"};
    const pipeduck::PortLink right{"pipeduck-1:monitor_FR", "alsa_output.foo:playback_FR"};
    const pipeduck::PortLink orphan{"pipeduck-1:monitor_FL", "alsa_output.old:playback_FL"};

    {
        // A retry after a partially completed first attempt only creates the
        // link that is still missing.
        const auto plan = pipeduck::plan_link_changes({left, right}, {left}, {left});
        CHECK(plan.create.size() == 1);
        CHECK((plan.create[0] == right));
        CHECK(plan.remove.empty());
    }
    {
        // Links this session created but no longer desires are slated for
        // removal, while desired links are left alone.
        const auto plan = pipeduck::plan_link_changes({left}, {left, orphan}, {left, orphan});
        CHECK(plan.create.empty());
        CHECK(plan.remove.size() == 1);
        CHECK((plan.remove[0] == orphan));
    }
    {
        // A desired link that already exists is not created again, and is not
        // adopted as owned just because it is present.
        const auto plan = pipeduck::plan_link_changes({left}, {}, {left});
        CHECK(plan.create.empty());
        CHECK(plan.remove.empty());
    }
    {
        // Nothing desired and nothing existing still releases ownership so
        // teardown does not leak a stale link.
        const auto plan = pipeduck::plan_link_changes({}, {left}, {});
        CHECK(plan.create.empty());
        CHECK(plan.remove.size() == 1);
        CHECK((plan.remove[0] == left));
    }
    {
        const auto plan = pipeduck::plan_link_changes({}, {}, {});
        CHECK(plan.create.empty());
        CHECK(plan.remove.empty());
    }
}

void test_routing_backend_startup() {
    g_current_test = "routing-backend-startup";
    const auto make_engine = [](const std::shared_ptr<FakeRoutingBackend>& backend) {
        return pipeduck::RoutingEngine(std::nullopt, std::nullopt, backend);
    };

    {
        // The second link fails once. The retry must not recreate the first one.
        auto backend = std::make_shared<FakeRoutingBackend>();
        backend->fail_create_input = backend->real_sink_name + ":playback_FR";
        backend->fail_create_input_calls = 1;
        auto options = fast_routing_options();
        options.port_appear_timeout = std::chrono::milliseconds(500);
        auto engine = make_engine(backend);
        auto session = engine.start(options);
        CHECK(backend->links.size() == 2);
        CHECK(backend->create_link_calls == 3);
        CHECK(backend->sink_destroyed == false);
        CHECK(session.virtual_sink_name() == backend->virtual_sink_name);
        const auto cleanup = session.stop();
        CHECK(cleanup.complete);
        CHECK(backend->sink_destroyed);
        CHECK(backend->links.empty());
    }
    {
        // A terminal link failure removes the partial link and destroys the sink.
        auto backend = std::make_shared<FakeRoutingBackend>();
        backend->fail_create_input = backend->real_sink_name + ":playback_FR";
        backend->fail_create_input_calls = 100;
        auto options = fast_routing_options();
        auto engine = make_engine(backend);
        bool threw = false;
        try {
            (void)engine.start(options);
        } catch (const std::exception& error) {
            threw = true;
            const std::string message = error.what();
            CHECK(message.find("could not route") != std::string::npos);
            CHECK(message.find("real:playback_FR") != std::string::npos);
        }
        CHECK(threw);
        CHECK(backend->create_link_calls == 2);
        CHECK(backend->remove_link_calls >= 1);
        CHECK(backend->links.empty());
        CHECK(backend->sink_destroyed);
    }
    {
        // Missing ports are reported clearly and still trigger sink cleanup.
        auto backend = std::make_shared<FakeRoutingBackend>();
        backend->output_ports.clear();
        auto engine = make_engine(backend);
        bool threw = false;
        try {
            (void)engine.start(fast_routing_options());
        } catch (const std::exception& error) {
            threw = true;
            CHECK(std::string(error.what()).find("no monitor ports") != std::string::npos);
        }
        CHECK(threw);
        CHECK(backend->create_link_calls == 0);
        CHECK(backend->sink_destroyed);
    }
    {
        // A virtual sink that never appears fails startup without leaking links.
        auto backend = std::make_shared<FakeRoutingBackend>();
        backend->sink_present = false;
        auto engine = make_engine(backend);
        CHECK_THROW(engine.start(fast_routing_options()));
        CHECK(backend->wait_for_sink_calls == 1);
        CHECK(backend->create_link_calls == 0);
        CHECK(backend->links.empty());
    }
}

void test_routing_backend_monitor_links() {
    g_current_test = "routing-backend-monitor-links";
    auto backend = std::make_shared<FakeRoutingBackend>();
    pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
    auto session = engine.start(fast_routing_options());
    CHECK(backend->links.size() == 2);

    backend->links.pop_back();
    CHECK(session.ensure_monitor_links());
    CHECK(backend->links.size() == 2);
    CHECK(backend->create_link_calls == 3);

    backend->fail_list_links_calls = 1;
    CHECK(!session.ensure_monitor_links());
    CHECK(backend->links.size() == 2);

    const auto cleanup = session.stop();
    CHECK(cleanup.complete);
}

void test_routing_backend_restore_failure() {
    g_current_test = "routing-backend-restore-failure";
    auto backend = std::make_shared<FakeRoutingBackend>();
    pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
    auto session = engine.start(fast_routing_options());

    backend->inputs.push_back(playback_input(7, backend->real_sink_index, "42"));
    CHECK(session.route_existing_outputs(std::nullopt) == 1);
    CHECK(backend->inputs[0].sink == backend->virtual_sink_index);

    backend->fail_move_to_index_calls = 100;
    backend->fail_move_to_name_calls = 100;
    const auto cleanup = session.stop();
    CHECK(!cleanup.complete);
    CHECK(cleanup.sink_left_alive);
    CHECK(backend->sink_abandoned);
    CHECK(!backend->sink_destroyed);
    CHECK(cleanup.failed_inputs.size() == 1);
    CHECK(cleanup.failed_inputs[0] == 7);
    CHECK(backend->links.size() == 2);

    const int moves = backend->move_calls;
    const auto repeated = session.stop();
    CHECK(repeated.complete == cleanup.complete);
    CHECK(repeated.sink_left_alive == cleanup.sink_left_alive);
    CHECK(backend->move_calls == moves);
    CHECK(!backend->sink_destroyed);
}

void test_routing_backend_stream_changes() {
    g_current_test = "routing-backend-stream-changes";
    {
        // A stream that disappears before teardown is not reported as a failure.
        auto backend = std::make_shared<FakeRoutingBackend>();
        pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
        auto session = engine.start(fast_routing_options());
        backend->inputs.push_back(playback_input(7, backend->real_sink_index, "42"));
        CHECK(session.route_existing_outputs(std::nullopt) == 1);
        backend->inputs.clear();
        const auto cleanup = session.stop();
        CHECK(cleanup.complete);
        CHECK(backend->move_calls == 1);
        CHECK(backend->sink_destroyed);
    }
    {
        // A reused sink-input index must not be moved during restore.
        auto backend = std::make_shared<FakeRoutingBackend>();
        pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
        auto session = engine.start(fast_routing_options());
        backend->inputs.push_back(playback_input(7, backend->real_sink_index, "42"));
        CHECK(session.route_existing_outputs(std::nullopt) == 1);
        backend->inputs[0] =
            playback_input(7, backend->real_sink_index, "99", "An unrelated application");
        const auto cleanup = session.stop();
        CHECK(cleanup.complete);
        CHECK(backend->move_calls == 1);
        CHECK(backend->sink_destroyed);
    }
}

void test_routing_backend_sink_disappearance() {
    g_current_test = "routing-backend-sink-disappearance";
    auto backend = std::make_shared<FakeRoutingBackend>();
    pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
    auto session = engine.start(fast_routing_options());

    backend->fail_sink_inputs = true;
    backend->sink_present = false;
    const auto cleanup = session.stop();
    CHECK(cleanup.complete);
    CHECK(!cleanup.sink_left_alive);
    CHECK(cleanup.detail.find("disappeared") != std::string::npos);
    CHECK(!backend->sink_destroyed);
    CHECK(!backend->sink_abandoned);
}

void test_routing_control_tick_no_fallback() {
    g_current_test = "routing-control-tick";
    auto backend = std::make_shared<FakeRoutingBackend>();
    pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
    auto session = engine.start(fast_routing_options());
    CHECK(backend->last_volume != nullptr);

    // The 25 ms tick path must never permit the blocking `pactl` fallback.
    CHECK(session.start_ramp(25.0, 1000));
    for (int tick = 0; tick < 10; ++tick) {
        (void)session.tick_ramp();
    }
    CHECK(backend->last_volume->calls > 0);
    CHECK(!backend->last_volume->last_allow_subprocess_fallback);

    // An immediate (zero-duration) fade also stays subprocess-free.
    CHECK(session.start_ramp(100.0, 0));
    CHECK(!backend->last_volume->last_allow_subprocess_fallback);

    const auto cleanup = session.stop();
    CHECK(cleanup.complete);
}

void test_routing_volume_backoff() {
    g_current_test = "routing-volume-backoff";
    using namespace std::chrono_literals;
    auto backend = std::make_shared<FakeRoutingBackend>();
    pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
    auto session = engine.start(fast_routing_options());
    FakeRoutingVolume* volume = backend->last_volume;
    CHECK(volume != nullptr);

    volume->result = false;
    // The initial immediate update fails and schedules the first retry.
    const int calls_before = volume->calls;
    CHECK(!session.start_ramp(25.0, 0));
    const int calls_after_failure = volume->calls;
    CHECK(calls_after_failure == calls_before + 1);

    // Ticks inside the backoff window must not call the backend again.
    for (int tick = 0; tick < 50; ++tick) {
        (void)session.tick_ramp();
    }
    CHECK(volume->calls == calls_after_failure);

    // Once the window elapses, the pending value is retried.
    std::this_thread::sleep_for(8ms);
    (void)session.tick_ramp();
    CHECK(volume->calls > calls_after_failure);

    // A recovered backend applies the pending value and clears the failure.
    volume->result = true;
    std::this_thread::sleep_for(15ms);
    CHECK(session.tick_ramp());
    CHECK(std::abs(session.volume_percent() - 25.0) < 1e-9);

    const auto cleanup = session.stop();
    CHECK(cleanup.complete);
}

void test_routing_volume_reconnects() {
    g_current_test = "routing-volume-reconnect";
    auto backend = std::make_shared<FakeRoutingBackend>();
    pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
    auto session = engine.start(fast_routing_options());
    CHECK(backend->create_volume_calls == 1);

    // The current volume backend reports itself gone. The next update must
    // recreate it instead of reusing a dead context.
    backend->last_volume->available_result = false;
    CHECK(session.start_ramp(25.0, 0));
    CHECK(backend->create_volume_calls == 2);
    CHECK(std::abs(session.volume_percent() - 25.0) < 1e-9);

    const auto cleanup = session.stop();
    CHECK(cleanup.complete);
}

void test_routing_graph_health() {
    g_current_test = "routing-graph-health";
    auto backend = std::make_shared<FakeRoutingBackend>();
    pipeduck::RoutingEngine engine(std::nullopt, std::nullopt, backend);
    auto session = engine.start(fast_routing_options());
    CHECK(session.check_graph() == pipeduck::GraphHealth::Healthy);

    // The virtual sink disappeared: the control loop must be able to detect it
    // without touching a live audio server or spawning a process.
    backend->sink_present = false;
    CHECK(session.check_graph() == pipeduck::GraphHealth::SinkMissing);

    backend->sink_present = true;
    backend->fail_sink_by_name = true;
    CHECK(session.check_graph() == pipeduck::GraphHealth::BackendUnavailable);

    backend->fail_sink_by_name = false;
    const auto cleanup = session.stop();
    CHECK(cleanup.complete);
}

void test_routing_backend_factory() {
    g_current_test = "routing-backend-factory";
    auto backend = pipeduck::make_pulse_routing_backend();
    CHECK(backend != nullptr);

    // Exercise the production command adapter with a fake `pw-link` on PATH so
    // the smoke test does not touch the real audio graph.
    const auto bin_dir =
        std::filesystem::temp_directory_path() /
        ("pipeduck-routing-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(bin_dir);
    const auto script = bin_dir / "pw-link";
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n"
               "case \"$1\" in\n"
               "  -o) echo 'pipeduck-1:monitor_FL'; echo 'pipeduck-1:monitor_FR' ;;\n"
               "  -i) echo 'real:playback_FL'; echo 'real:playback_FR' ;;\n"
               "  -l) echo 'pipeduck-1:monitor_FL'; echo '  |-> real:playback_FL' ;;\n"
               "  -d) exit 0 ;;\n"
               "  fail-output) echo 'simulated link failure' >&2; exit 1 ;;\n"
               "  *) exit 0 ;;\n"
               "esac\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write |
                                             std::filesystem::perms::owner_exec);

    const std::string original_path = std::getenv("PATH") != nullptr ? std::getenv("PATH") : "";
    const auto restore = [&]() {
        setenv("PATH", original_path.c_str(), 1);
        std::error_code ignored;
        std::filesystem::remove_all(bin_dir, ignored);
    };

    try {
        setenv("PATH", bin_dir.c_str(), 1);
        CHECK(backend->list_output_ports().find("monitor_FR") != std::string::npos);
        CHECK(backend->list_input_ports().find("playback_FL") != std::string::npos);
        CHECK(pipeduck::parse_link_list(backend->list_links()).size() == 1);
        const pipeduck::PortLink link{"pipeduck-1:monitor_FL", "real:playback_FL"};
        backend->create_link(link);
        backend->remove_link(link);
        bool failed = false;
        try {
            backend->create_link({"fail-output", "fail-input"});
        } catch (const std::exception& error) {
            failed = true;
            const std::string message = error.what();
            CHECK(message.find("could not link fail-output -> fail-input") != std::string::npos);
            CHECK(message.find("simulated link failure") != std::string::npos);
        }
        CHECK(failed);
    } catch (...) {
        restore();
        throw;
    }
    restore();
    CHECK(std::getenv("PATH") != nullptr);
    CHECK(std::string(std::getenv("PATH")) == original_path);
}

void test_routed_stream_identity() {
    g_current_test = "routed-stream-identity";
    const auto input = [](uint32_t index, uint32_t sink, const std::string& serial,
                          const std::string& node) {
        pipeduck::SinkInput value;
        value.index = index;
        value.sink = sink;
        value.properties = {
            {"application.name", "Discord"},
            {"application.process.binary", "discord"},
            {"media.name", "playStream"},
            {"media.class", "Stream/Output/Audio"},
        };
        if (!serial.empty()) {
            value.properties["object.serial"] = serial;
        }
        if (!node.empty()) {
            value.properties["node.name"] = node;
        }
        return value;
    };

    const auto original = pipeduck::routed_stream_from_input(input(7, 3, "42", "discord-call"), 3);
    CHECK(original.index == 7);
    CHECK(original.original_sink == 3);
    CHECK(original.object_serial == "42");
    CHECK(original.node_name == "discord-call");
    CHECK(original.label == "Discord / discord / playStream");
    CHECK(original.matches(input(9, 8, "42", "renamed-node")));
    CHECK(!original.matches(input(9, 8, "43", "discord-call")));

    pipeduck::RoutedStream node_only;
    node_only.index = 1;
    node_only.node_name = "discord-call";
    CHECK(node_only.matches(input(1, 8, "", "discord-call")));
    CHECK(!node_only.matches(input(1, 8, "", "other")));

    pipeduck::RoutedStream label_only;
    label_only.index = 2;
    label_only.label = "Discord / discord / playStream";
    CHECK(label_only.matches(input(2, 8, "", "")));
    pipeduck::SinkInput other = input(2, 8, "", "");
    other.properties["application.name"] = "Firefox";
    CHECK(!label_only.matches(other));

    pipeduck::RoutedStream no_fingerprint;
    no_fingerprint.index = 3;
    CHECK(!no_fingerprint.matches(input(3, 8, "", "")));
}

void test_routed_stream_reconcile() {
    g_current_test = "routed-stream-reconcile";
    const auto make_routed = [](uint32_t index, const std::string& serial) {
        pipeduck::RoutedStream stream;
        stream.index = index;
        stream.original_sink = 3;
        stream.object_serial = serial;
        return stream;
    };
    const auto make_input = [](uint32_t index, uint32_t sink, const std::string& serial) {
        pipeduck::SinkInput input;
        input.index = index;
        input.sink = sink;
        input.properties["object.serial"] = serial;
        return input;
    };

    const uint32_t virtual_sink = 9;
    auto routed = std::vector<pipeduck::RoutedStream>{make_routed(1, "11"), make_routed(2, "22"),
                                                      make_routed(3, "33"), make_routed(4, "44")};
    const std::vector<pipeduck::SinkInput> current{
        make_input(1, virtual_sink, "11"), // still routed
        make_input(2, 3, "22"),            // already restored
        // 3 vanished
        make_input(4, virtual_sink, "99"), // index reused by another stream
    };
    const auto kept = pipeduck::reconcile_routed_streams(routed, current, virtual_sink);
    CHECK(kept.size() == 1);
    CHECK(kept[0].index == 1);

    CHECK(pipeduck::reconcile_routed_streams({}, current, virtual_sink).empty());
}

void test_restore_routed_streams() {
    g_current_test = "restore-routed-streams";
    const auto make_routed = [](uint32_t index, uint32_t original_sink, const std::string& serial) {
        pipeduck::RoutedStream stream;
        stream.index = index;
        stream.original_sink = original_sink;
        stream.object_serial = serial;
        return stream;
    };
    const auto make_input = [](uint32_t index, uint32_t sink, const std::string& serial) {
        pipeduck::SinkInput input;
        input.index = index;
        input.sink = sink;
        input.properties["object.serial"] = serial;
        return input;
    };

    const uint32_t virtual_sink = 9;
    std::vector<pipeduck::RoutedStream> routed{
        make_routed(1, 3, "11"), // restored by index
        make_routed(2, 4, "22"), // index move fails, name fallback succeeds
        make_routed(3, 5, "33"), // both moves fail
        make_routed(4, 3, "44"), // vanished
        make_routed(5, 3, "55"), // index reused by an unrelated stream
        make_routed(6, 3, "66"), // user already moved it away
    };
    const std::vector<pipeduck::SinkInput> current{
        make_input(1, virtual_sink, "11"),
        make_input(2, virtual_sink, "22"),
        make_input(3, virtual_sink, "33"),
        make_input(5, virtual_sink, "different"),
        make_input(6, 3, "66"),
    };

    std::vector<uint32_t> index_attempts;
    std::vector<uint32_t> name_attempts;
    const auto result = pipeduck::restore_routed_streams(
        routed, current, virtual_sink,
        [&index_attempts](uint32_t index, uint32_t) {
            index_attempts.push_back(index);
            return index == 1;
        },
        [&name_attempts](uint32_t index, const std::string& sink_name) {
            name_attempts.push_back(index);
            return sink_name == "real-sink" && index == 2;
        },
        "real-sink");

    CHECK(result.attempted == 3);
    CHECK(result.restored.size() == 2);
    CHECK(result.restored[0] == 1);
    CHECK(result.restored[1] == 2);
    CHECK(result.failed.size() == 1);
    CHECK(result.failed[0] == 3);
    CHECK(result.skipped.size() == 3);
    CHECK(std::find(result.skipped.begin(), result.skipped.end(), 4) != result.skipped.end());
    CHECK(std::find(result.skipped.begin(), result.skipped.end(), 5) != result.skipped.end());
    CHECK(std::find(result.skipped.begin(), result.skipped.end(), 6) != result.skipped.end());
    CHECK(std::find(index_attempts.begin(), index_attempts.end(), 5) == index_attempts.end());
    CHECK(std::find(name_attempts.begin(), name_attempts.end(), 5) == name_attempts.end());
    CHECK(routed.size() == 1);
    CHECK(routed[0].index == 3);

    // A second pass retries only the stream that still failed.
    index_attempts.clear();
    name_attempts.clear();
    const auto retry = pipeduck::restore_routed_streams(
        routed, {make_input(3, virtual_sink, "33")}, virtual_sink,
        [](uint32_t, uint32_t) { return false; },
        [](uint32_t, const std::string&) { return false; }, "real-sink");
    CHECK(retry.attempted == 1);
    CHECK(retry.failed.size() == 1);
    CHECK(routed.size() == 1);
}

void test_cleanup_description() {
    g_current_test = "cleanup-description";
    pipeduck::RouteCleanupResult clean;
    CHECK(pipeduck::describe_cleanup(clean) == "all routed streams restored");

    pipeduck::RouteCleanupResult partial;
    partial.complete = false;
    partial.sink_left_alive = true;
    partial.failed_inputs = {12, 13};
    partial.skipped_inputs = {14};
    partial.detail = "virtual sink still has streams";
    const std::string message = pipeduck::describe_cleanup(partial);
    CHECK(message.find("virtual sink still has streams") != std::string::npos);
    CHECK(message.find("#12") != std::string::npos);
    CHECK(message.find("#13") != std::string::npos);
    CHECK(message.find("skipped: #14") != std::string::npos);
    CHECK(message.find("left alive") != std::string::npos);

    pipeduck::RouteCleanupResult inspection;
    inspection.complete = false;
    inspection.inspection_failed = true;
    CHECK(pipeduck::describe_cleanup(inspection).find("inspect") != std::string::npos);
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

void test_tray_state() {
    g_current_test = "tray-state";
    using pipeduck::TrayState;

    CHECK(!pipeduck::tray_state_visible(TrayState::Idle));
    CHECK(!pipeduck::tray_state_visible(TrayState::Starting));
    CHECK(!pipeduck::tray_state_visible(TrayState::Stopping));
    CHECK(pipeduck::tray_state_visible(TrayState::Waiting));
    CHECK(pipeduck::tray_state_visible(TrayState::Neutral));
    CHECK(pipeduck::tray_state_visible(TrayState::Ducked));
    CHECK(!pipeduck::tray_state_visible(TrayState::Recovering));
    CHECK(!pipeduck::tray_state_visible(TrayState::Degraded));
    CHECK(!pipeduck::tray_state_visible(TrayState::Error));
    CHECK(pipeduck::tray_state_value(TrayState::Neutral) == "ON");
    CHECK(pipeduck::tray_state_value(TrayState::Starting) == "OFF");

    pipeduck::TrayStatus status;
    pipeduck::DuckingEvent waiting;
    waiting.type = pipeduck::DuckingEventType::WaitingForSource;
    auto reduction = pipeduck::reduce_tray_event(status, waiting);
    CHECK(reduction.changed);
    CHECK(reduction.status.state == TrayState::Waiting);
    CHECK(pipeduck::tray_state_visible(reduction.status.state));

    // A failed startup must not be reported as Ducking ON.
    pipeduck::DuckingEvent failure;
    failure.type = pipeduck::DuckingEventType::RoutingFailed;
    failure.message = "pw-link unavailable";
    reduction = pipeduck::reduce_tray_event(reduction.status, failure);
    CHECK(reduction.status.state == TrayState::Recovering);
    CHECK(!pipeduck::tray_state_visible(reduction.status.state));
    CHECK(pipeduck::tray_state_attention(reduction.status.state));
    CHECK(reduction.status.message.find("pw-link unavailable") != std::string::npos);

    // Only an established session clears recovery.
    pipeduck::DuckingEvent recovering;
    recovering.type = pipeduck::DuckingEventType::Recovering;
    recovering.message = "rebuilding";
    reduction = pipeduck::reduce_tray_event(reduction.status, recovering);
    CHECK(reduction.status.state == TrayState::Recovering);

    // Repeated rebuild failures keep the tray out of the ON state and ask for
    // attention instead of silently reporting recovery.
    for (int attempt = 0; attempt < 3; ++attempt) {
        reduction = pipeduck::reduce_tray_event(reduction.status, failure);
        CHECK(reduction.status.state == TrayState::Recovering);
        CHECK(!pipeduck::tray_state_visible(reduction.status.state));
        CHECK(pipeduck::tray_state_attention(reduction.status.state));
        CHECK(pipeduck::tray_state_value(reduction.status.state) == "OFF");
    }

    pipeduck::DuckingEvent started;
    started.type = pipeduck::DuckingEventType::Started;
    started.label = "#7 Discord / discord / playStream";
    started.threshold = 0.01;
    started.start_threshold = 0.02;
    started.hold_ms = 700;
    reduction = pipeduck::reduce_tray_event(reduction.status, started);
    CHECK(reduction.status.state == TrayState::Neutral);
    CHECK(!pipeduck::tray_state_attention(reduction.status.state));
    CHECK(reduction.status.message.find("#7 Discord") != std::string::npos);
    CHECK(pipeduck::tray_state_visible(reduction.status.state));

    pipeduck::DuckingEvent active;
    active.type = pipeduck::DuckingEventType::VoiceActive;
    active.level = 0.42;
    active.duck_percent = 25;
    reduction = pipeduck::reduce_tray_event(reduction.status, active);
    CHECK(reduction.status.state == TrayState::Ducked);
    CHECK(reduction.status.message.find("25%") != std::string::npos);

    pipeduck::DuckingEvent inactive;
    inactive.type = pipeduck::DuckingEventType::VoiceInactive;
    reduction = pipeduck::reduce_tray_event(reduction.status, inactive);
    CHECK(reduction.status.state == TrayState::Neutral);

    // A degraded session is surfaced instead of showing a healthy Ducked/ON.
    pipeduck::DuckingEvent degraded;
    degraded.type = pipeduck::DuckingEventType::Degraded;
    degraded.message = "volume backend lost";
    reduction = pipeduck::reduce_tray_event(reduction.status, degraded);
    CHECK(reduction.status.state == TrayState::Degraded);
    CHECK(!pipeduck::tray_state_visible(reduction.status.state));
    reduction = pipeduck::reduce_tray_event(reduction.status, started);
    CHECK(reduction.status.state == TrayState::Neutral);

    // A plain message keeps the current state.
    pipeduck::DuckingEvent note;
    note.type = pipeduck::DuckingEventType::Message;
    note.message = "Configured microphone is not available";
    reduction = pipeduck::reduce_tray_event(reduction.status, note);
    CHECK(reduction.status.state == TrayState::Neutral);
    CHECK(reduction.status.message == note.message);

    // Partial cleanup stays visible even when the worker stops afterwards.
    pipeduck::DuckingEvent restore_failed;
    restore_failed.type = pipeduck::DuckingEventType::RestoreFailed;
    restore_failed.message = "streams: #12";
    reduction = pipeduck::reduce_tray_event(reduction.status, restore_failed);
    CHECK(reduction.status.state == TrayState::Error);
    CHECK(!pipeduck::tray_state_visible(reduction.status.state));
    const auto stopped = pipeduck::reduce_worker_stopped(reduction.status);
    CHECK(stopped.status.state == TrayState::Error);
    CHECK(!stopped.changed);

    pipeduck::TrayStatus working;
    working.state = TrayState::Starting;
    const auto clean_stop = pipeduck::reduce_worker_stopped(working);
    CHECK(clean_stop.status.state == TrayState::Idle);
    CHECK(clean_stop.status.message == "Off");

    const auto fatal = pipeduck::reduce_worker_failed(working, "boom");
    CHECK(fatal.status.state == TrayState::Error);
    CHECK(fatal.status.message == "boom");
    CHECK(pipeduck::tray_state_attention(fatal.status.state));
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
    test_routing_links();
    test_routing_link_plan();
    test_routing_backend_startup();
    test_routing_backend_monitor_links();
    test_routing_backend_restore_failure();
    test_routing_backend_stream_changes();
    test_routing_backend_sink_disappearance();
    test_routing_control_tick_no_fallback();
    test_routing_volume_backoff();
    test_routing_volume_reconnects();
    test_routing_graph_health();
    test_routing_backend_factory();
    test_routed_stream_identity();
    test_routed_stream_reconcile();
    test_restore_routed_streams();
    test_cleanup_description();
    test_pulse_json();
    test_ramp();
    test_nonblocking_ramp();
    test_tray_state();
    test_settings();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All pipeduck tests passed\n";
    return 0;
}
