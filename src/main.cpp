#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
#include "pipeduck/tray.hpp"
#include "pipeduck/tune.hpp"
#include "pipeduck/vad.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop.store(true);
}

void install_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

std::string voice_hint(const pipeduck::AudioIdentity& identity) {
    return identity.looks_like_voice_source() ? " voice?" : "";
}

void print_playback_streams(const std::vector<pipeduck::SinkInput>& inputs) {
    for (const auto& input : inputs) {
        const auto identity = input.identity();
        if (!identity.is_playback_stream()) {
            continue;
        }
        const auto value = [](const std::optional<std::string>& field) {
            return field.has_value() && !field->empty() ? *field : std::string("-");
        };
        std::cout << "- #" << input.index << " sink=" << input.sink << voice_hint(identity)
                  << " app=" << value(identity.application_name)
                  << " bin=" << value(identity.application_process_binary)
                  << " media=" << value(identity.media_name)
                  << " role=" << value(identity.media_role)
                  << " class=" << value(identity.media_class) << "\n";
    }
}

void print_microphones(const pipeduck::PulseCtl& pulse, const pipeduck::Config& config) {
    std::string default_source;
    try {
        default_source = pulse.default_source_name();
    } catch (const std::exception&) {
    }
    for (const auto& source : pulse.sources()) {
        if (source.is_monitor) {
            continue;
        }
        const bool is_default = source.name == default_source;
        const bool is_selected = config.mic_source.has_value() &&
                                 config.mic_source->name.has_value() &&
                                 *config.mic_source->name == source.name;
        std::cout << "- #" << source.index << (is_default ? " default" : "")
                  << (is_selected ? " selected" : "") << " " << source.name << " ("
                  << source.description << ")\n";
    }
}

pipeduck::DuckingSettings resolve_settings(const pipeduck::Config& config,
                                           const pipeduck::CliOptions& cli) {
    pipeduck::DuckingSettings settings = pipeduck::DuckingSettings::from_config(config);
    if (cli.duck_percent.has_value()) {
        settings.duck_percent = *cli.duck_percent;
    }
    if (cli.vad_threshold.has_value()) {
        settings.vad_threshold = *cli.vad_threshold;
    }
    if (cli.mic_vad_threshold.has_value()) {
        settings.mic_vad_threshold = *cli.mic_vad_threshold;
    }
    if (cli.hold_ms.has_value()) {
        settings.hold_ms = *cli.hold_ms;
    }
    if (cli.fade_ms.has_value()) {
        settings.release_fade_ms = *cli.fade_ms;
    }
    if (cli.trigger.has_value()) {
        settings.trigger = *cli.trigger;
    }
    return settings.normalized();
}

void print_status(const pipeduck::PulseCtl& pulse, const pipeduck::Config& config) {
    try {
        const std::string default_sink = pulse.default_sink_name();
        const auto sink = pulse.sink_by_name(default_sink);
        std::cout << "Default sink: " << default_sink;
        if (sink.has_value()) {
            std::cout << " #" << sink->index;
            if (!sink->description.empty()) {
                std::cout << " (" << sink->description << ")";
            }
        }
        std::cout << "\n";
    } catch (const std::exception& error) {
        std::cout << "Default sink: unavailable (" << error.what() << ")\n";
    }

    try {
        std::cout << "Default microphone: " << pulse.default_source_name() << "\n";
    } catch (const std::exception&) {
    }
    const double mic_threshold = config.mic_vad_threshold.value_or(config.vad_threshold);
    std::cout << "Trigger: " << pipeduck::trigger_name(config.trigger) << "\n";
    std::cout << "Ducking: " << config.duck_percent << "%, hold " << config.hold_ms
              << " ms, return fade " << config.release_fade_ms << " ms\n";
    std::cout << "Thresholds: call " << pipeduck::format_double(config.vad_threshold, 4)
              << " (start "
              << pipeduck::format_double(pipeduck::vad_start_threshold(config.vad_threshold), 4)
              << "), microphone " << pipeduck::format_double(mic_threshold, 4) << " (start "
              << pipeduck::format_double(pipeduck::vad_start_threshold(mic_threshold), 4) << ")\n";

    try {
        const auto inputs = pulse.sink_inputs();
        std::cout << "Playback streams: " << inputs.size() << "\n";
        print_playback_streams(inputs);
    } catch (const std::exception& error) {
        std::cout << "Playback streams: unavailable (" << error.what() << ")\n";
    }
}

void select_source(const pipeduck::PulseCtl& pulse, unsigned index) {
    const auto inputs = pulse.sink_inputs();
    const auto input = std::find_if(inputs.begin(), inputs.end(), [index](const auto& candidate) {
        return candidate.index == index;
    });
    if (input == inputs.end()) {
        throw std::runtime_error("sink-input #" + std::to_string(index) + " not found");
    }
    const auto identity = input->identity();
    if (!identity.is_playback_stream()) {
        throw std::runtime_error("sink-input #" + std::to_string(index) +
                                 " is not a playback stream");
    }
    const std::string label = pipeduck::source_label(identity);
    pipeduck::Config config = pipeduck::Config::load_or_default();
    config.voice_source = pipeduck::configured_source_from_identity(label, identity);
    config.save();
    std::cout << "Saved voice-call source: " << label << "\n";
    std::cout << "Config: " << pipeduck::config_path().string() << "\n";
    if (config.trigger == pipeduck::Trigger::Mic) {
        std::cout << "Hint: trigger is set to mic; use --trigger both to duck for calls too.\n";
    }
}

void select_mic(const pipeduck::PulseCtl& pulse, const std::string& query) {
    std::optional<pipeduck::Source> selected;
    bool query_is_number =
        !query.empty() && std::all_of(query.begin(), query.end(),
                                      [](unsigned char c) { return std::isdigit(c) != 0; });
    if (query_is_number) {
        selected = pulse.source_by_index(static_cast<uint32_t>(std::stoul(query)));
    }
    if (!selected.has_value()) {
        for (const auto& source : pulse.sources()) {
            if (!source.is_monitor && source.name == query) {
                selected = source;
                break;
            }
        }
    }
    if (!selected.has_value()) {
        const std::string needle = pipeduck::to_lower(query);
        for (const auto& source : pulse.sources()) {
            if (source.is_monitor) {
                continue;
            }
            if (pipeduck::to_lower(source.description).find(needle) != std::string::npos) {
                selected = source;
                break;
            }
        }
    }
    if (!selected.has_value()) {
        throw std::runtime_error("microphone '" + query + "' not found");
    }
    if (selected->is_monitor) {
        throw std::runtime_error("'" + query + "' is a monitor source, not a microphone");
    }

    pipeduck::Config config = pipeduck::Config::load_or_default();
    config.mic_source = pipeduck::ConfiguredMic{selected->name, selected->description};
    config.save();
    std::cout << "Saved microphone: " << selected->name;
    if (!selected->description.empty()) {
        std::cout << " (" << selected->description << ")";
    }
    std::cout << "\nConfig: " << pipeduck::config_path().string() << "\n";
    if (config.trigger == pipeduck::Trigger::Call) {
        std::cout << "Hint: trigger is set to call; use --trigger mic or --trigger both "
                     "to duck for your voice.\n";
    }
}

int run_route(const pipeduck::CliOptions& cli, bool once) {
    pipeduck::ensure_route_acknowledged(once ? "route-once" : "route", cli.yes_really_route);
    const pipeduck::Config config = pipeduck::Config::load_or_default();
    const pipeduck::DuckingSettings settings = resolve_settings(config, cli);

    if (once) {
        pipeduck::RoutingEngine engine(config.voice_source, config.mic_source);
        auto session = engine.start();
        session.set_percent(settings.duck_percent);
        std::cout << "Routing active through " << session.virtual_sink_name() << " at "
                  << settings.duck_percent << "% for " << cli.seconds << " seconds.\n";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cli.seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            session.route_existing_outputs(config.voice_source);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        session.set_neutral();
        session.stop();
        std::cout << "Restored.\n";
        return 0;
    }

    install_signal_handlers();
    pipeduck::SettingsStore store(settings);
    std::cout << "Ducking is active. Press Ctrl+C to stop.\n";
    pipeduck::run_until_stopped(g_stop, store, false, [](const pipeduck::DuckingEvent& event) {
        switch (event.type) {
        case pipeduck::DuckingEventType::WaitingForSource:
            std::cout << (event.message.empty() ? "Waiting for a voice source..." : event.message)
                      << std::endl;
            break;
        case pipeduck::DuckingEventType::Started:
            std::cout << "Routing active. Source=" << event.label
                      << " threshold=" << pipeduck::format_double(event.threshold, 4)
                      << " start=" << pipeduck::format_double(event.start_threshold, 4)
                      << " hold=" << event.hold_ms << "ms" << std::endl;
            break;
        case pipeduck::DuckingEventType::VoiceActive:
            std::cout << "VOICE ACTIVE level=" << pipeduck::format_double(event.level, 4)
                      << " -> ducking " << event.duck_percent << "%" << std::endl;
            break;
        case pipeduck::DuckingEventType::VoiceInactive:
            std::cout << "VOICE INACTIVE level=" << pipeduck::format_double(event.level, 4)
                      << " -> smooth return" << std::endl;
            break;
        case pipeduck::DuckingEventType::Message:
            std::cout << event.message << std::endl;
            break;
        }
    });
    std::cout << "Stopped and restored.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    using namespace pipeduck;
    try {
        const CliOptions cli = parse_cli(argc, argv);
        const PulseCtl pulse;

        switch (cli.command) {
        case Command::Help:
            std::cout << usage_text();
            return 0;
        case Command::Version:
            std::cout << "pipeduck " << PIPEDUCK_VERSION << "\n";
            return 0;
        case Command::ConfigPath:
            std::cout << config_path().string() << "\n";
            return 0;
        case Command::InitConfig: {
            const auto path = config_path();
            if (std::filesystem::exists(path)) {
                std::cout << "Config exists: " << path.string() << "\n";
            } else {
                Config::defaults().save();
                std::cout << "Created config: " << path.string() << "\n";
            }
            return 0;
        }
        case Command::Status: {
            print_status(pulse, Config::load_or_default());
            return 0;
        }
        case Command::Sources: {
            std::cout << "Current playback streams:\n";
            print_playback_streams(pulse.sink_inputs());
            std::cout << "\nSave a voice-call source: pipeduck select-source <index>\n";
            return 0;
        }
        case Command::SelectSource:
            select_source(pulse, *cli.sink_input_index);
            return 0;
        case Command::MicSources: {
            const Config config = Config::load_or_default();
            std::cout << "Microphones:\n";
            print_microphones(pulse, config);
            std::cout << "\nSave a microphone: pipeduck select-mic <name|index>\n";
            return 0;
        }
        case Command::SelectMic:
            select_mic(pulse, *cli.source_query);
            return 0;
        case Command::Tune:
            return run_terminal_tuner();
        case Command::TuneGui:
#ifdef PIPEDUCK_GUI
            return run_gui_tuner();
#else
            throw std::runtime_error(
                "this build has no graphical tuner; use 'pipeduck tune' or rebuild with "
                "-DPIPEDUCK_GUI=ON");
#endif
        case Command::Route:
            return run_route(cli, false);
        case Command::RouteOnce:
            return run_route(cli, true);
        case Command::Tray: {
            Config config = Config::load_or_default();
            const DuckingSettings settings = resolve_settings(config, cli);
            settings.apply_to(config);
            config.save();
            return run_tray(TrayOptions{settings});
        }
        }
    } catch (const std::exception& error) {
        std::cerr << "pipeduck: " << error.what() << "\n";
        return 1;
    }
    return 1;
}
