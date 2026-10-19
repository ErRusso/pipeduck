#include "pipeduck/cli.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace pipeduck {
namespace {

int parse_int_arg(const std::string& value, const std::string& option) {
    try {
        std::size_t consumed = 0;
        const int result = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error(option + " expects an integer, got '" + value + "'");
    }
}

double parse_double_arg(const std::string& value, const std::string& option) {
    try {
        std::size_t consumed = 0;
        const double result = std::stod(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error(option + " expects a number, got '" + value + "'");
    }
}

std::string need_value(int argc, char** argv, int& index, const std::string& option) {
    if (index + 1 >= argc) {
        throw std::runtime_error(option + " expects a value");
    }
    return argv[++index];
}

} // namespace

CliOptions parse_cli(int argc, char** argv) {
    CliOptions options;
    bool command_seen = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h") {
            options.command = Command::Help;
            return options;
        }
        if (argument == "--version" || argument == "-V") {
            options.command = Command::Version;
            return options;
        }
        if (argument == "--duck-percent") {
            options.duck_percent = parse_int_arg(need_value(argc, argv, index, argument), argument);
            continue;
        }
        if (argument == "--vad-threshold") {
            options.vad_threshold =
                parse_double_arg(need_value(argc, argv, index, argument), argument);
            continue;
        }
        if (argument == "--hold-ms") {
            options.hold_ms = parse_int_arg(need_value(argc, argv, index, argument), argument);
            continue;
        }
        if (argument == "--fade-ms") {
            options.fade_ms = parse_int_arg(need_value(argc, argv, index, argument), argument);
            continue;
        }
        if (argument == "--trigger") {
            options.trigger = parse_trigger(need_value(argc, argv, index, argument));
            continue;
        }
        if (argument == "--seconds") {
            const int seconds = parse_int_arg(need_value(argc, argv, index, argument), argument);
            if (seconds <= 0) {
                throw std::runtime_error("--seconds must be positive");
            }
            options.seconds = static_cast<unsigned>(seconds);
            continue;
        }
        if (argument == "--yes-really-route") {
            options.yes_really_route = true;
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            throw std::runtime_error("unknown option '" + argument + "'");
        }

        if (command_seen) {
            if (options.command == Command::SelectSource) {
                options.sink_input_index =
                    static_cast<unsigned>(parse_int_arg(argument, "select-source"));
                continue;
            }
            if (options.command == Command::SelectMic) {
                options.source_query = argument;
                continue;
            }
            throw std::runtime_error("unexpected argument '" + argument + "'");
        }

        command_seen = true;
        if (argument == "tray")
            options.command = Command::Tray;
        else if (argument == "status")
            options.command = Command::Status;
        else if (argument == "config-path")
            options.command = Command::ConfigPath;
        else if (argument == "init-config")
            options.command = Command::InitConfig;
        else if (argument == "sources")
            options.command = Command::Sources;
        else if (argument == "select-source")
            options.command = Command::SelectSource;
        else if (argument == "mic-sources")
            options.command = Command::MicSources;
        else if (argument == "select-mic")
            options.command = Command::SelectMic;
        else if (argument == "tune")
            options.command = Command::Tune;
        else if (argument == "tune-gui")
            options.command = Command::TuneGui;
        else if (argument == "route")
            options.command = Command::Route;
        else if (argument == "route-once")
            options.command = Command::RouteOnce;
        else
            throw std::runtime_error("unknown command '" + argument + "'");
    }

    if (options.command == Command::SelectSource && !options.sink_input_index.has_value()) {
        throw std::runtime_error("select-source expects a sink-input index");
    }
    if (options.command == Command::SelectMic && !options.source_query.has_value()) {
        throw std::runtime_error("select-mic expects a source name or index");
    }
    return options;
}

std::string usage_text() {
    return R"(pipeduck - automatic audio ducking for PipeWire

Usage:
  pipeduck [command] [options]

Commands:
  tray                 Start the system tray (default)
  status               Show the default sink, playback streams and microphones
  sources              List playback streams selectable as call audio
  select-source ID     Save a playback stream as the voice-call source
  mic-sources          List microphones selectable for voice ducking
  select-mic NAME|ID   Save a microphone (name, or index from mic-sources)
  tune                 Open the terminal tuner
  tune-gui             Open the GTK4 tuner (if built with GUI support)
  route                Route streams and duck until Ctrl+C (live graph change)
  route-once           Route for a few seconds (diagnostic, live graph change)
  config-path          Print the config file path
  init-config          Create a default config if none exists

Options:
  --duck-percent N     Volume for non-call audio while ducking (0-100)
  --vad-threshold F    Base RMS threshold; voice starts above about 2x this value
  --hold-ms N          Keep ducking active for N ms after speech stops
  --fade-ms N          Smooth return time in ms (overrides release_fade_ms)
  --trigger MODE       call, mic or both
  --seconds N          Duration for route-once (default: 5)
  --yes-really-route   Required acknowledgement for route and route-once
  -h, --help           Show this help
  -V, --version        Show the version

Examples:
  pipeduck --trigger both
  pipeduck sources && pipeduck select-source 546
  pipeduck select-mic alsa_input.usb-Mic-00.pro-input-0
  pipeduck tune
)";
}

} // namespace pipeduck
