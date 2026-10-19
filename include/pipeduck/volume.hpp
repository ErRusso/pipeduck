#pragma once

#include <atomic>
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

// Calls `apply` for every step of a smoothstep fade, sleeping step_ms between
// steps. The final value is always applied.
void run_volume_ramp(double from_percent, double to_percent, int duration_ms, int step_ms,
                     const std::atomic<bool>* stop, const std::function<void(double)>& apply);

} // namespace pipeduck
