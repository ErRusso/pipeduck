#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pipeduck/config.hpp"
#include "pipeduck/pulse.hpp"

namespace pipeduck {

struct RoutingOptions {
    std::string virtual_sink_prefix = "pipeduck";
    std::chrono::milliseconds sink_appear_timeout{2000};
};

struct PortLink {
    std::string output;
    std::string input;
    bool operator==(const PortLink&) const = default;
};

std::vector<PortLink> monitor_links_for_ports(const std::string& outputs, const std::string& inputs,
                                              const std::string& virtual_sink,
                                              const std::string& real_sink);

class RoutingEngine;

class RouteSession {
public:
    ~RouteSession();
    RouteSession(const RouteSession&) = delete;
    RouteSession& operator=(const RouteSession&) = delete;
    RouteSession(RouteSession&&) noexcept;

    // Immediately set the virtual monitor volume in percent.
    void set_percent(double percent);
    // Start a non-blocking smoothstep fade. Call tick_ramp() from the control loop.
    void start_ramp(double percent, int duration_ms);
    // Advance the active fade to the current time. Safe to call when idle.
    void tick_ramp();
    bool ramp_active() const;
    // Fade smoothly to `percent`. Blocks for at most duration_ms.
    void ramp_to(double percent, int duration_ms, const std::atomic<bool>* stop = nullptr);
    void set_neutral(int fade_ms = 0);

    // Move newly appearing non-call playback streams to the virtual sink.
    std::size_t route_existing_outputs(const std::optional<ConfiguredSource>& voice_source);

    // Restore every moved stream and tear the virtual sink down. Safe to call
    // multiple times. If a stream cannot be moved away, the sink is left alive.
    void stop();

    double volume_percent() const {
        return volume_percent_;
    }
    const std::string& virtual_sink_name() const;
    const std::string& real_sink_name() const;

private:
    friend class RoutingEngine;
    RouteSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    double volume_percent_ = 100.0;
};

class RoutingEngine {
public:
    RoutingEngine(std::optional<ConfiguredSource> voice_source,
                  std::optional<ConfiguredMic> mic_source = std::nullopt);

    RouteSession start(const RoutingOptions& options = {});

    // Excludes the voice-call stream from ducking when it can be identified.
    const std::optional<ConfiguredSource>& voice_source() const {
        return voice_source_;
    }

private:
    std::optional<ConfiguredSource> voice_source_;
    std::optional<ConfiguredMic> mic_source_;
};

} // namespace pipeduck
