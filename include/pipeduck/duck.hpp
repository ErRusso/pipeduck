#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "pipeduck/config.hpp"

namespace pipeduck {

struct DuckingSettings {
    int duck_percent = 25;
    double vad_threshold = 0.01;
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

enum class DuckingEventType {
    WaitingForSource,
    Started,
    VoiceActive,
    VoiceInactive,
    Message,
};

struct DuckingEvent {
    DuckingEventType type = DuckingEventType::Message;
    std::string label;
    std::string message;
    double level = 0.0;
    double threshold = 0.0;
    double start_threshold = 0.0;
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
