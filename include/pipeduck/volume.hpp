#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace pipeduck {

// Sets the volume of a PulseAudio source (the virtual sink's monitor source).
// Uses libpulse's asynchronous API when possible so volume ramps do not spawn a
// process every few milliseconds, with a pactl fallback otherwise.
class VolumeController {
public:
    VolumeController();
    ~VolumeController();
    VolumeController(const VolumeController&) = delete;
    VolumeController& operator=(const VolumeController&) = delete;

    bool native() const;
    void set_percent(const std::string& source_name, double percent);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

double smoothstep(double value);

// A non-blocking smoothstep fade. Callers drive it from their own control tick
// with update(), which makes it possible to interrupt a release fade as soon as
// speech is detected again.
class VolumeRamp {
public:
    using Clock = std::chrono::steady_clock;

    void start(double from_percent, double to_percent, int duration_ms,
               Clock::time_point now = Clock::now());
    double update(Clock::time_point now = Clock::now());
    void cancel();

    bool active() const {
        return active_;
    }
    double current() const {
        return current_;
    }
    double target() const {
        return target_;
    }

private:
    double start_ = 100.0;
    double target_ = 100.0;
    double current_ = 100.0;
    int duration_ms_ = 0;
    Clock::time_point started_{};
    bool active_ = false;
};

// Calls `apply` for every step of a smoothstep fade, sleeping step_ms between
// steps. The final value is always applied.
void run_volume_ramp(double from_percent, double to_percent, int duration_ms, int step_ms,
                     const std::atomic<bool>* stop, const std::function<void(double)>& apply);

} // namespace pipeduck
