#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pipeduck/config.hpp"
#include "pipeduck/pipewire.hpp"
#include "pipeduck/pulse.hpp"

namespace pipeduck {

struct DuckingSettings {
    int duck_percent = 25;
    double vad_threshold = 0.01;
    double mic_vad_threshold = 0.01;
    int hold_ms = 700;
    int attack_fade_ms = 80;
    int release_fade_ms = 320;
    Trigger trigger = Trigger::Call;

    DuckingSettings normalized() const;
    static DuckingSettings from_config(const Config& config);
    void apply_to(Config& config) const;
};

class SettingsStore {
public:
    explicit SettingsStore(DuckingSettings settings);
    DuckingSettings get() const;
    void set(const DuckingSettings& settings);

private:
    mutable std::mutex mutex_;
    DuckingSettings settings_;
};

struct CallTarget {
    CaptureTarget capture;
    std::string label;
};

// Finds the configured call stream and builds a diagnostic label for it. This
// overload is separate from PulseCtl so the matching/label logic can be tested
// without a live audio server.
std::optional<CallTarget> find_call_target(const std::vector<SinkInput>& inputs,
                                           const ConfiguredSource& source);

enum class DuckingEventType {
    WaitingForSource,
    Started,
    VoiceActive,
    VoiceInactive,
    // A route session could not be created. The worker will retry with backoff.
    RoutingFailed,
    // An established session noticed that its graph disappeared and is about to
    // rebuild it.
    Recovering,
    // The session is still running but part of its functionality is unavailable
    // (for example, monitor volume updates keep failing).
    Degraded,
    // Cleanup could not restore every routed stream. The virtual sink is left
    // alive and the caller must not report a clean stop.
    RestoreFailed,
    Message,
};

struct DuckingEvent {
    DuckingEventType type = DuckingEventType::Message;
    std::string label;
    std::string message;
    double level = 0.0;
    double threshold = 0.0;
    double start_threshold = 0.0;
    double mic_threshold = 0.0;
    double mic_start_threshold = 0.0;
    int hold_ms = 0;
    int duck_percent = 0;
};

using DuckingCallback = std::function<void(const DuckingEvent&)>;

// Runs the ducking control loop until `stop` becomes true. When
// `reload_config` is set, config.toml is polled so the tray and tuners apply
// changes live. Throws on unrecoverable errors.
void run_until_stopped(const std::atomic<bool>& stop, SettingsStore& settings, bool reload_config,
                       const DuckingCallback& on_event);

void ensure_route_acknowledged(const std::string& command, bool yes_really_route);

} // namespace pipeduck
