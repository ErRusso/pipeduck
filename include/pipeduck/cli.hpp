#pragma once

#include <optional>
#include <string>

#include "pipeduck/config.hpp"

namespace pipeduck {

enum class Command {
    Tray,
    Status,
    ConfigPath,
    InitConfig,
    Sources,
    SelectSource,
    MicSources,
    SelectMic,
    Tune,
    TuneGui,
    Route,
    RouteOnce,
    Help,
    Version,
};

struct CliOptions {
    Command command = Command::Tray;
    std::optional<int> duck_percent;
    std::optional<double> vad_threshold;
    std::optional<int> hold_ms;
    std::optional<int> fade_ms;
    std::optional<Trigger> trigger;
    std::optional<unsigned> sink_input_index;
    std::optional<std::string> source_query;
    unsigned seconds = 5;
    bool yes_really_route = false;
};

CliOptions parse_cli(int argc, char** argv);
std::string usage_text();

} // namespace pipeduck
