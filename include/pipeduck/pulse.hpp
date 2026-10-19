#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pipeduck/common.hpp"
#include "pipeduck/identity.hpp"

namespace pipeduck {

struct Sink {
    uint32_t index = 0;
    std::string name;
    std::string description;
};

struct Source {
    uint32_t index = 0;
    std::string name;
    std::string description;
    bool is_monitor = false;
    StringMap properties;
};

struct SinkInput {
    uint32_t index = 0;
    uint32_t sink = 0;
    StringMap properties;

    AudioIdentity identity() const {
        return AudioIdentity::from_props(properties);
    }
};

std::vector<Sink> parse_sinks_json(const std::string& raw);
std::vector<SinkInput> parse_sink_inputs_json(const std::string& raw);
std::vector<Source> parse_sources_json(const std::string& raw);

class PulseCtl {
public:
    std::string default_sink_name() const;
    std::string default_source_name() const;
    void set_default_sink(const std::string& sink_name) const;

    std::vector<Sink> sinks() const;
    std::optional<Sink> sink_by_name(const std::string& name) const;
    std::optional<Sink> sink_by_index(uint32_t index) const;
    Sink wait_for_sink(const std::string& name, std::chrono::milliseconds timeout) const;

    std::vector<SinkInput> sink_inputs() const;
    std::vector<SinkInput> sink_inputs_on_sink(uint32_t sink_index) const;

    std::vector<Source> sources() const;
    std::optional<Source> source_by_name(const std::string& name) const;
    std::optional<Source> source_by_index(uint32_t index) const;

    void move_sink_input(uint32_t input_index, const std::string& sink_name) const;
    void move_sink_input_to_index(uint32_t input_index, uint32_t sink_index) const;
    void set_source_volume_percent(const std::string& source_name, double percent) const;

private:
    static constexpr std::chrono::milliseconds kTimeout{5000};
};

} // namespace pipeduck
