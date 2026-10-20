#include "pipeduck/volume.hpp"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "pipeduck/common.hpp"

namespace pipeduck {
namespace {} // namespace

struct VolumeController::Impl {
    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    bool ready = false;
    bool failed = false;

    static void state_callback(pa_context* context, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        switch (pa_context_get_state(context)) {
        case PA_CONTEXT_READY:
            self->ready = true;
            self->failed = false;
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            self->ready = false;
            self->failed = true;
            break;
        default:
            break;
        }
        pa_threaded_mainloop_signal(self->mainloop, 0);
    }

    Impl() {
        mainloop = pa_threaded_mainloop_new();
        if (mainloop == nullptr) {
            return;
        }
        if (pa_threaded_mainloop_start(mainloop) < 0) {
            pa_threaded_mainloop_free(mainloop);
            mainloop = nullptr;
            return;
        }
        pa_threaded_mainloop_lock(mainloop);
        context = pa_context_new(pa_threaded_mainloop_get_api(mainloop), "pipeduck");
        if (context == nullptr) {
            pa_threaded_mainloop_unlock(mainloop);
            return;
        }
        pa_context_set_state_callback(context, state_callback, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            failed = true;
        }
        while (!ready && !failed) {
            pa_threaded_mainloop_wait(mainloop);
        }
        pa_threaded_mainloop_unlock(mainloop);
    }

    ~Impl() {
        if (mainloop != nullptr) {
            pa_threaded_mainloop_lock(mainloop);
        }
        if (context != nullptr) {
            pa_context_disconnect(context);
            pa_context_unref(context);
            context = nullptr;
        }
        if (mainloop != nullptr) {
            pa_threaded_mainloop_unlock(mainloop);
            pa_threaded_mainloop_stop(mainloop);
            pa_threaded_mainloop_free(mainloop);
            mainloop = nullptr;
        }
    }

    bool available() const {
        return mainloop != nullptr && context != nullptr && ready;
    }

    bool set_native(const std::string& source_name, double percent) {
        if (!available()) {
            return false;
        }
        const double clamped = std::clamp(percent, 0.0, 150.0);
        pa_volume_t volume =
            static_cast<pa_volume_t>(std::llround(clamped / 100.0 * PA_VOLUME_NORM));
        pa_cvolume channel_volume;
        pa_cvolume_set(&channel_volume, 1, volume);
        pa_threaded_mainloop_lock(mainloop);
        if (!ready) {
            pa_threaded_mainloop_unlock(mainloop);
            return false;
        }
        pa_operation* operation = pa_context_set_source_volume_by_name(
            context, source_name.c_str(), &channel_volume, nullptr, nullptr);
        const bool ok = operation != nullptr;
        if (operation != nullptr) {
            pa_operation_unref(operation);
        } else {
            failed = true;
        }
        pa_threaded_mainloop_unlock(mainloop);
        return ok;
    }
};

VolumeController::VolumeController() : impl_(std::make_unique<Impl>()) {}

VolumeController::~VolumeController() = default;

bool VolumeController::native() const {
    return impl_ != nullptr && impl_->available();
}

bool VolumeController::set_percent(const std::string& source_name, double percent,
                                   bool allow_subprocess_fallback) {
    if (source_name.empty()) {
        return false;
    }
    if (impl_ != nullptr && impl_->set_native(source_name, percent)) {
        return true;
    }
    if (!allow_subprocess_fallback) {
        return false;
    }
    try {
        const double clamped = std::clamp(percent, 0.0, 150.0);
        const int raw = static_cast<int>(std::lround(clamped / 100.0 * 65536.0));
        run_status_checked("pactl", {"set-source-volume", source_name, std::to_string(raw)});
        return true;
    } catch (const std::exception& error) {
        std::cerr << "pipeduck: could not set monitor volume: " << error.what() << "\n";
        return false;
    }
}

double smoothstep(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

void VolumeRamp::start(double from_percent, double to_percent, int duration_ms,
                       Clock::time_point now) {
    start_ = std::clamp(from_percent, 0.0, 150.0);
    target_ = std::clamp(to_percent, 0.0, 150.0);
    current_ = start_;
    duration_ms_ = std::max(duration_ms, 0);
    started_ = now;
    active_ = duration_ms_ > 0 && std::abs(target_ - start_) >= 0.01;
    if (!active_) {
        current_ = target_;
    }
}

double VolumeRamp::update(Clock::time_point now) {
    if (!active_) {
        return current_;
    }
    if (duration_ms_ <= 0) {
        current_ = target_;
        active_ = false;
        return current_;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count();
    const double progress =
        std::clamp(static_cast<double>(elapsed) / static_cast<double>(duration_ms_), 0.0, 1.0);
    current_ = start_ + (target_ - start_) * smoothstep(progress);
    if (progress >= 1.0) {
        current_ = target_;
        active_ = false;
    }
    return current_;
}

void VolumeRamp::cancel() {
    active_ = false;
}

void run_volume_ramp(double from_percent, double to_percent, int duration_ms, int step_ms,
                     const std::atomic<bool>* stop, const std::function<void(double)>& apply) {
    if (!apply) {
        return;
    }
    const double from = std::clamp(from_percent, 0.0, 150.0);
    const double to = std::clamp(to_percent, 0.0, 150.0);
    if (duration_ms <= 0 || step_ms <= 0 || std::abs(to - from) < 0.01) {
        apply(to);
        return;
    }

    const int steps = std::max(1, duration_ms / step_ms);
    for (int step = 1; step <= steps; ++step) {
        if (stop != nullptr && stop->load()) {
            break;
        }
        const double t = static_cast<double>(step) / static_cast<double>(steps);
        const double value = from + (to - from) * smoothstep(t);
        apply(value);
        if (step != steps) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        }
    }
    apply(to);
}

} // namespace pipeduck
