#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace pipeduck {

struct VadOptions {
    double threshold = 0.01;
    std::chrono::milliseconds attack{40};
    std::chrono::milliseconds hold{700};
};

bool vad_is_disabled(double threshold);
double vad_start_threshold(double threshold);
double vad_release_threshold(double threshold);

class VadState {
public:
    bool step(double energy, double threshold, std::chrono::milliseconds attack,
              std::chrono::milliseconds hold);
    bool active() const {
        return voice_active_;
    }
    void reset();

private:
    std::chrono::steady_clock::time_point last_above_{};
    std::chrono::steady_clock::time_point above_start_{};
    bool has_last_above_ = false;
    bool has_above_start_ = false;
    bool voice_active_ = false;
};

} // namespace pipeduck
