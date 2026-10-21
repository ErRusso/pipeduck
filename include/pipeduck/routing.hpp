#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
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
    std::chrono::milliseconds port_appear_timeout{2000};
    std::chrono::milliseconds restore_timeout{3000};
    std::chrono::milliseconds retry_interval{50};
    // Bounded backoff for monitor-volume updates when the backend is lost.
    std::chrono::milliseconds volume_retry_initial{500};
    std::chrono::milliseconds volume_retry_max{4000};
};

struct PortLink {
    std::string output;
    std::string input;
    bool operator==(const PortLink&) const = default;
};

std::vector<PortLink> monitor_links_for_ports(const std::string& outputs, const std::string& inputs,
                                              const std::string& virtual_sink,
                                              const std::string& real_sink);

// Parses `pw-link -l` output into output -> input pairs. Both `|->` rows under
// an output port and `|<-` rows under an input port are accepted.
std::vector<PortLink> parse_link_list(const std::string& listing);

// Links that are desired but not already present. Used to make startup retries
// idempotent: a link created by an earlier attempt is not created twice.
std::vector<PortLink> missing_links(const std::vector<PortLink>& desired,
                                    const std::vector<PortLink>& existing);

struct LinkPlan {
    std::vector<PortLink> create;
    std::vector<PortLink> remove;
};

// Reconcile the links a session owns with the links the server reports. Links
// that this session created but no longer desires are marked for removal.
LinkPlan plan_link_changes(const std::vector<PortLink>& desired, const std::vector<PortLink>& owned,
                           const std::vector<PortLink>& existing);

// A stream moved to the virtual sink, with enough identity to avoid restoring a
// different stream if the server reuses its sink-input index.
struct RoutedStream {
    uint32_t index = 0;
    uint32_t original_sink = 0;
    std::string object_serial;
    std::string node_name;
    std::string label;

    bool matches(const SinkInput& input) const;
};

RoutedStream routed_stream_from_input(const SinkInput& input, uint32_t original_sink);

// Removes entries that no longer identify a stream currently attached to the
// virtual sink. This prunes vanished streams and rejects reused indices.
std::vector<RoutedStream> reconcile_routed_streams(std::vector<RoutedStream> routed,
                                                   const std::vector<SinkInput>& current,
                                                   uint32_t virtual_sink_index);

struct RestorePassResult {
    std::size_t attempted = 0;
    std::vector<uint32_t> restored;
    std::vector<uint32_t> failed;
    std::vector<uint32_t> skipped;
};

// One deterministic restoration pass. Callbacks return true on success. The
// second callback is the fallback move-by-name path.
RestorePassResult
restore_routed_streams(std::vector<RoutedStream>& routed, const std::vector<SinkInput>& current,
                       uint32_t virtual_sink_index,
                       const std::function<bool(uint32_t, uint32_t)>& move_to_index,
                       const std::function<bool(uint32_t, const std::string&)>& move_to_name,
                       const std::string& fallback_sink_name);

struct RouteCleanupResult {
    bool complete = true;
    bool sink_left_alive = false;
    bool inspection_failed = false;
    std::vector<uint32_t> failed_inputs;
    std::vector<uint32_t> skipped_inputs;
    std::string detail;
};

// Human-readable one-line diagnostic for a cleanup result.
std::string describe_cleanup(const RouteCleanupResult& result);

// Minimal seam around the graph operations used by routing. Production code
// uses make_pulse_routing_backend(); tests provide a local fake instead of
// touching a live audio server.
class RoutingSink {
public:
    virtual ~RoutingSink() = default;
    virtual const std::string& name() const = 0;
    virtual void destroy() = 0;
    virtual void abandon() = 0;
};

class RoutingVolume {
public:
    virtual ~RoutingVolume() = default;
    // False when the underlying backend is gone. The session recreates the
    // volume backend at the next bounded retry instead of retrying every tick.
    virtual bool available() const = 0;
    virtual bool set_percent(const std::string& source_name, double percent,
                             bool allow_subprocess_fallback) = 0;
};

class RoutingBackend {
public:
    virtual ~RoutingBackend() = default;

    virtual std::string default_sink_name() = 0;
    virtual std::optional<Sink> sink_by_name(const std::string& name) = 0;
    virtual std::optional<Sink> sink_by_index(uint32_t index) = 0;
    virtual Sink wait_for_sink(const std::string& name, std::chrono::milliseconds timeout) = 0;
    virtual std::vector<SinkInput> sink_inputs() = 0;
    virtual void set_default_sink(const std::string& name) = 0;
    virtual void move_sink_input(uint32_t input_index, const std::string& sink_name) = 0;
    virtual void move_sink_input_to_index(uint32_t input_index, uint32_t sink_index) = 0;

    virtual std::unique_ptr<RoutingSink> create_sink(const std::string& prefix) = 0;
    virtual std::unique_ptr<RoutingVolume> create_volume() = 0;

    virtual std::string list_output_ports() = 0;
    virtual std::string list_input_ports() = 0;
    virtual std::string list_links() = 0;
    virtual void create_link(const PortLink& link) = 0;
    virtual void remove_link(const PortLink& link) = 0;
};

std::shared_ptr<RoutingBackend> make_pulse_routing_backend(bool allow_subprocess_fallback = true);

class RoutingEngine;

enum class GraphHealth {
    Healthy,
    SinkMissing,
    BackendUnavailable,
};

class RouteSession {
public:
    ~RouteSession();
    RouteSession(const RouteSession&) = delete;
    RouteSession& operator=(const RouteSession&) = delete;
    RouteSession(RouteSession&&) noexcept;

    // Immediately set the virtual monitor volume in percent. By default this
    // never spawns a subprocess; callers that can tolerate a blocking fallback
    // (startup/shutdown) may opt in.
    bool set_percent(double percent, bool allow_subprocess_fallback = false);
    // Start a non-blocking smoothstep fade. Call tick_ramp() from the control loop.
    bool start_ramp(double percent, int duration_ms);
    // Advance the active fade to the current time. Safe to call when idle.
    // Returns false if the volume backend rejected the update.
    bool tick_ramp();
    bool ramp_active() const;
    // Fade smoothly to `percent`. Blocks for at most duration_ms.
    void ramp_to(double percent, int duration_ms, const std::atomic<bool>* stop = nullptr);
    void set_neutral(int fade_ms = 0);

    // Move newly appearing non-call playback streams to the virtual sink.
    std::size_t route_existing_outputs(const std::optional<ConfiguredSource>& voice_source);

    // Re-create monitor links that disappeared while routing. Returns false when
    // the graph cannot be repaired (for example, the virtual sink vanished).
    bool ensure_monitor_links();

    // Cheap health check for the control loop. Uses the routing backend only,
    // so it never spawns a subprocess.
    GraphHealth check_graph();

    // Restore every moved stream and tear the virtual sink down. Safe to call
    // multiple times. If a stream cannot be moved away, the sink is left alive
    // and the result is marked incomplete.
    RouteCleanupResult stop();
    const RouteCleanupResult& cleanup_result() const;

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
                  std::optional<ConfiguredMic> mic_source = std::nullopt,
                  std::shared_ptr<RoutingBackend> backend = nullptr);

    RouteSession start(const RoutingOptions& options = {});

    // Excludes the voice-call stream from ducking when it can be identified.
    const std::optional<ConfiguredSource>& voice_source() const {
        return voice_source_;
    }

private:
    std::optional<ConfiguredSource> voice_source_;
    std::optional<ConfiguredMic> mic_source_;
    std::shared_ptr<RoutingBackend> backend_;
};

} // namespace pipeduck
