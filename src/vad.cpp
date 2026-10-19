#include "pipeduck/vad.hpp"

#include <cmath>
#include <limits>

namespace pipeduck {

bool vad_is_disabled(double threshold) {
    return threshold >= 0.2;
}

double vad_start_threshold(double threshold) {
    if (vad_is_disabled(threshold)) {
        return std::numeric_limits<double>::infinity();
    }
    return threshold * 2.0;
}

double vad_release_threshold(double threshold) {
    if (vad_is_disabled(threshold)) {
        return std::numeric_limits<double>::infinity();
    }
    return threshold * 0.75;
}

void VadState::reset() {
    has_last_above_ = false;
    has_above_start_ = false;
    voice_active_ = false;
}

bool VadState::step(double energy, double threshold, std::chrono::milliseconds attack,
                    std::chrono::milliseconds hold) {
    if (vad_is_disabled(threshold)) {
        reset();
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const double start_threshold = vad_start_threshold(threshold);
    const double release_threshold = vad_release_threshold(threshold);
    const bool above = voice_active_ ? energy > release_threshold : energy > start_threshold;

    if (above) {
        last_above_ = now;
        has_last_above_ = true;
        if (!voice_active_) {
            if (attack.count() <= 0) {
                voice_active_ = true;
                has_above_start_ = false;
            } else if (!has_above_start_) {
                above_start_ = now;
                has_above_start_ = true;
            } else if (now - above_start_ >= attack) {
                voice_active_ = true;
                has_above_start_ = false;
            }
        }
    } else {
        has_above_start_ = false;
        if (voice_active_) {
            if (hold.count() <= 0) {
                voice_active_ = false;
                has_last_above_ = false;
            } else if (has_last_above_ && now - last_above_ >= hold) {
                voice_active_ = false;
                has_last_above_ = false;
            }
        }
    }
    return voice_active_;
}

} // namespace pipeduck
