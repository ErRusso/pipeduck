#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
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

// A process-wide, subscription-driven cache of the PulseAudio/PipeWire graph.
// The cache is refreshed from libpulse events instead of spawning `pactl`, so
// snapshots are cheap enough to use from the tray's D-Bus thread and from the
// ducking control loop. If libpulse is unavailable, PulseCtl falls back to
// `pactl` and this cache simply reports unavailable().
class PulseCache {
public:
    static PulseCache& shared();
    ~PulseCache();
    PulseCache(const PulseCache&) = delete;
    PulseCache& operator=(const PulseCache&) = delete;

    bool available() const;
    // Increases whenever the cached graph changes.
    uint64_t revision() const;

    std::string default_sink_name() const;
    std::string default_source_name() const;

    std::vector<Sink> sinks() const;
    std::optional<Sink> sink_by_name(const std::string& name) const;
    std::optional<Sink> sink_by_index(uint32_t index) const;

    std::vector<SinkInput> sink_inputs() const;
    std::vector<SinkInput> sink_inputs_on_sink(uint32_t sink_index) const;

    std::vector<Source> sources() const;
    std::optional<Source> source_by_name(const std::string& name) const;
    std::optional<Source> source_by_index(uint32_t index) const;

    bool move_sink_input(uint32_t input_index, const std::string& sink_name) const;
    bool move_sink_input_to_index(uint32_t input_index, uint32_t sink_index) const;
    bool set_default_sink(const std::string& sink_name) const;
    bool set_source_volume_percent(const std::string& source_name, double percent) const;

private:
    PulseCache();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PulseCtl {
public:
    // When `allow_subprocess_fallback` is false, graph queries never spawn
    // `pactl`; they fail fast when the libpulse event cache is unavailable.
    // Use the non-blocking mode from the real-time ducking control loop.
    explicit PulseCtl(bool allow_subprocess_fallback = true)
        : allow_fallback_(allow_subprocess_fallback) {}

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

    // Revision of the event-driven graph cache, or 0 when libpulse is unavailable.
    static uint64_t cache_revision();

private:
    void require_fallback() const;

    bool allow_fallback_ = true;
    static constexpr std::chrono::milliseconds kTimeout{5000};
};

} // namespace pipeduck
