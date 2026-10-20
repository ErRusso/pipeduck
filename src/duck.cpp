#include "pipeduck/duck.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#include "pipeduck/identity.hpp"
#include "pipeduck/pipewire.hpp"
#include "pipeduck/pulse.hpp"
#include "pipeduck/routing.hpp"
#include "pipeduck/vad.hpp"

namespace pipeduck {
namespace {

constexpr auto kWaitingPoll = std::chrono::milliseconds(500);
constexpr auto kTickInterval = std::chrono::milliseconds(25);
constexpr auto kSourceCheckInterval = std::chrono::milliseconds(500);
constexpr auto kRouteRefreshInterval = std::chrono::milliseconds(500);
constexpr auto kSinkCheckInterval = std::chrono::milliseconds(500);
constexpr auto kConfigReloadInterval = std::chrono::milliseconds(250);
constexpr auto kVadAttack = std::chrono::milliseconds(40);

std::optional<CallTarget> find_call_target(const PulseCtl& pulse, const ConfiguredSource& source) {
    return find_call_target(pulse.sink_inputs(), source);
}

std::optional<CaptureTarget> find_mic_target(const Config& config) {
    const PulseCtl pulse;
    const auto sources = pulse.sources();
    std::vector<Source> candidates;
    for (const auto& source : sources) {
        if (!source.is_monitor) {
            candidates.push_back(source);
        }
    }
    if (candidates.empty()) {
        return std::nullopt;
    }

    std::optional<Source> selected;
    if (config.mic_source.has_value() && config.mic_source->name.has_value()) {
        for (const auto& source : candidates) {
            if (source.name == *config.mic_source->name) {
                selected = source;
                break;
            }
        }
    }
    if (!selected.has_value()) {
        try {
            const std::string default_source = pulse.default_source_name();
            for (const auto& source : candidates) {
                if (source.name == default_source) {
                    selected = source;
                    break;
                }
            }
        } catch (const std::exception&) {
        }
    }
    if (!selected.has_value()) {
        selected = candidates.front();
    }

    CaptureTarget target;
    target.label = selected->description.empty() ? selected->name : selected->description;
    const auto node_name = selected->properties.find("node.name");
    if (node_name != selected->properties.end() && !node_name->second.empty()) {
        target.node_name = node_name->second;
    } else {
        target.node_name = selected->name;
    }
    target.sink_monitor = false;
    return target;
}

bool same_stream(const CaptureTarget& left, const CaptureTarget& right) {
    if (left.object_serial.has_value() && right.object_serial.has_value()) {
        return *left.object_serial == *right.object_serial;
    }
    return left.node_name == right.node_name;
}

std::string capture_label(const std::optional<CallTarget>& call,
                          const std::optional<CaptureTarget>& mic) {
    std::string label;
    if (call.has_value()) {
        label = "call: " + call->label;
    }
    if (mic.has_value()) {
        if (!label.empty()) {
            label += " + ";
        }
        label += "mic: " + mic->label;
    }
    return label;
}

enum class SessionEnd { StopRequested, SourceChanged, SinkChanged, RoutingFailed };

} // namespace

std::optional<CallTarget> find_call_target(const std::vector<SinkInput>& inputs,
                                           const ConfiguredSource& source) {
    for (const auto& input : inputs) {
        if (!input.identity().matches_configured_source(source)) {
            continue;
        }
        const AudioIdentity identity = input.identity();
        const std::string label = "#" + std::to_string(input.index) + " " + source_label(identity);
        CaptureTarget target;
        target.label = label;
        target.node_name = identity.node_name;
        const auto serial = input.properties.find("object.serial");
        if (serial != input.properties.end() && !serial->second.empty()) {
            target.object_serial = serial->second;
        }
        target.sink_monitor = true;
        CallTarget result;
        result.capture = std::move(target);
        result.label = label;
        return result;
    }
    return std::nullopt;
}

DuckingSettings DuckingSettings::normalized() const {
    DuckingSettings result = *this;
    result.duck_percent = std::clamp(result.duck_percent, 0, 100);
    result.vad_threshold = std::clamp(result.vad_threshold, 0.0025, 0.2);
    result.mic_vad_threshold = std::clamp(result.mic_vad_threshold, 0.0025, 0.2);
    result.hold_ms = std::clamp(result.hold_ms, 0, 4000);
    result.attack_fade_ms = std::clamp(result.attack_fade_ms, 0, 2000);
    result.release_fade_ms = std::clamp(result.release_fade_ms, 0, 3000);
    return result;
}

DuckingSettings DuckingSettings::from_config(const Config& config) {
    DuckingSettings settings;
    settings.duck_percent = config.duck_percent;
    settings.vad_threshold = config.vad_threshold;
    settings.mic_vad_threshold = config.mic_vad_threshold.value_or(config.vad_threshold);
    settings.hold_ms = config.hold_ms;
    settings.attack_fade_ms = config.attack_fade_ms;
    settings.release_fade_ms = config.release_fade_ms;
    settings.trigger = config.trigger;
    return settings.normalized();
}

void DuckingSettings::apply_to(Config& config) const {
    const DuckingSettings settings = normalized();
    config.duck_percent = settings.duck_percent;
    config.vad_threshold = settings.vad_threshold;
    config.mic_vad_threshold = settings.mic_vad_threshold;
    config.hold_ms = settings.hold_ms;
    config.attack_fade_ms = settings.attack_fade_ms;
    config.release_fade_ms = settings.release_fade_ms;
    config.trigger = settings.trigger;
}

SettingsStore::SettingsStore(DuckingSettings settings) : settings_(settings.normalized()) {}

DuckingSettings SettingsStore::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_.normalized();
}

void SettingsStore::set(const DuckingSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings.normalized();
}

void ensure_route_acknowledged(const std::string& command, bool yes_really_route) {
    if (!yes_really_route) {
        throw std::runtime_error(command +
                                 " changes the live PipeWire graph; use --yes-really-route");
    }
}

void run_until_stopped(const std::atomic<bool>& stop, SettingsStore& settings, bool reload_config,
                       const DuckingCallback& on_event) {
    bool waiting_reported = false;

    const auto report_waiting = [&](const std::string& message) {
        if (!waiting_reported) {
            DuckingEvent event;
            event.type = DuckingEventType::WaitingForSource;
            event.message = message;
            on_event(event);
            waiting_reported = true;
        }
    };

    while (!stop.load()) {
        if (reload_config) {
            try {
                settings.set(DuckingSettings::from_config(Config::load_or_default()));
            } catch (const std::exception&) {
                // Keep the last good settings while the file is being edited.
            }
        }

        Config config;
        try {
            config = Config::load_or_default();
        } catch (const std::exception& error) {
            report_waiting("Waiting for a valid config: " + std::string(error.what()));
            std::this_thread::sleep_for(kWaitingPoll);
            continue;
        }
        const DuckingSettings current = settings.get();
        const Trigger trigger = current.trigger;

        const bool wants_call = trigger == Trigger::Call || trigger == Trigger::Both;
        const bool wants_mic = trigger == Trigger::Mic || trigger == Trigger::Both;

        std::optional<CallTarget> call;
        std::optional<CaptureTarget> mic;
        std::optional<ConfiguredSource> exclusion;

        if (config.voice_source.has_value()) {
            try {
                auto maybe_call = find_call_target(PulseCtl{}, *config.voice_source);
                if (maybe_call.has_value()) {
                    // The configured call stream is excluded from ducking even in
                    // microphone-only mode, and its sink becomes the routing target.
                    exclusion = config.voice_source;
                    if (wants_call) {
                        call = std::move(maybe_call);
                    }
                }
            } catch (const std::exception&) {
            }
        }
        if (wants_call && trigger == Trigger::Call && !call.has_value()) {
            report_waiting("Waiting for the configured voice-call stream");
            std::this_thread::sleep_for(kWaitingPoll);
            continue;
        }

        std::string mic_note;
        if (wants_mic) {
            try {
                mic = find_mic_target(config);
                if (config.mic_source.has_value() && config.mic_source->name.has_value() &&
                    mic.has_value()) {
                    const auto sources = PulseCtl{}.sources();
                    bool configured_found = false;
                    for (const auto& source : sources) {
                        if (!source.is_monitor && source.name == *config.mic_source->name) {
                            configured_found = true;
                            break;
                        }
                    }
                    if (!configured_found) {
                        mic_note = "Configured microphone is not available; using " + mic->label;
                    }
                }
            } catch (const std::exception& error) {
                if (trigger == Trigger::Mic) {
                    report_waiting(std::string("Waiting for a microphone: ") + error.what());
                    std::this_thread::sleep_for(kWaitingPoll);
                    continue;
                }
                mic_note = std::string("Microphone unavailable: ") + error.what();
            }
        }

        if (!call.has_value() && !mic.has_value()) {
            report_waiting("Waiting for a voice source");
            std::this_thread::sleep_for(kWaitingPoll);
            continue;
        }
        waiting_reported = false;

        if (!mic_note.empty()) {
            DuckingEvent event;
            event.type = DuckingEventType::Message;
            event.message = mic_note;
            on_event(event);
        }

        std::optional<RouteSession> session;
        try {
            RoutingEngine engine(exclusion, config.mic_source);
            session.emplace(engine.start());
        } catch (const std::exception& error) {
            DuckingEvent event;
            event.type = DuckingEventType::Message;
            event.message = std::string("Routing failed: ") + error.what();
            on_event(event);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::unique_ptr<VoiceActivityMonitor> call_monitor;
        std::unique_ptr<VoiceActivityMonitor> mic_monitor;
        try {
            if (call.has_value()) {
                call_monitor = VoiceActivityMonitor::start(call->capture);
            }
            if (mic.has_value()) {
                mic_monitor = VoiceActivityMonitor::start(*mic);
            }
        } catch (const std::exception& error) {
            DuckingEvent event;
            event.type = DuckingEventType::Message;
            event.message = std::string("Voice capture failed: ") + error.what();
            on_event(event);
            session->set_neutral();
            session->stop();
            std::this_thread::sleep_for(kWaitingPoll);
            continue;
        }

        const std::string label = capture_label(call, mic);
        {
            DuckingEvent event;
            event.type = DuckingEventType::Started;
            event.label = label;
            event.threshold = current.vad_threshold;
            event.start_threshold = vad_start_threshold(current.vad_threshold);
            event.mic_threshold = current.mic_vad_threshold;
            event.mic_start_threshold = vad_start_threshold(current.mic_vad_threshold);
            event.hold_ms = current.hold_ms;
            on_event(event);
        }

        VadState call_vad;
        VadState mic_vad;
        uint64_t last_call_frames = 0;
        uint64_t last_mic_frames = 0;
        bool ducked = false;
        std::optional<int> applied_duck_percent;
        auto next_source_check = std::chrono::steady_clock::now();
        auto next_route_refresh = std::chrono::steady_clock::now();
        auto next_sink_check = std::chrono::steady_clock::now();
        auto next_config_reload = std::chrono::steady_clock::now();
        SessionEnd end = SessionEnd::StopRequested;

        while (!stop.load()) {
            const auto now = std::chrono::steady_clock::now();

            if (reload_config && now >= next_config_reload) {
                try {
                    const Config fresh = Config::load_or_default();
                    settings.set(DuckingSettings::from_config(fresh));
                    if (fresh.trigger != config.trigger ||
                        fresh.voice_source != config.voice_source ||
                        fresh.mic_source != config.mic_source) {
                        end = SessionEnd::SourceChanged;
                        break;
                    }
                } catch (const std::exception&) {
                }
                next_config_reload = now + kConfigReloadInterval;
            }

            if (now >= next_source_check) {
                if (call.has_value()) {
                    const auto current_call =
                        config.voice_source.has_value()
                            ? find_call_target(PulseCtl{}, *config.voice_source)
                            : std::nullopt;
                    if (!current_call.has_value() ||
                        !same_stream(current_call->capture, call->capture)) {
                        end = SessionEnd::SourceChanged;
                        break;
                    }
                } else if (trigger == Trigger::Both && config.voice_source.has_value()) {
                    // A call may start after mic ducking is already running.
                    const auto current_call = find_call_target(PulseCtl{}, *config.voice_source);
                    if (current_call.has_value()) {
                        end = SessionEnd::SourceChanged;
                        break;
                    }
                }
                next_source_check = now + kSourceCheckInterval;
            }

            if (now >= next_route_refresh) {
                try {
                    session->route_existing_outputs(exclusion);
                } catch (const std::exception&) {
                }
                next_route_refresh = now + kRouteRefreshInterval;
            }

            if (now >= next_sink_check) {
                try {
                    const std::string default_sink = PulseCtl{}.default_sink_name();
                    if (!default_sink.empty() && default_sink != session->real_sink_name() &&
                        default_sink != session->virtual_sink_name()) {
                        end = SessionEnd::SinkChanged;
                        break;
                    }
                } catch (const std::exception&) {
                }
                next_sink_check = now + kSinkCheckInterval;
            }

            const DuckingSettings active_settings = settings.get();
            bool voice_active = false;
            double level = 0.0;

            if (call_monitor != nullptr && call_monitor->audio_seen()) {
                const uint64_t frames = call_monitor->frames();
                const double energy = frames == last_call_frames ? 0.0 : call_monitor->energy();
                last_call_frames = frames;
                if (call_vad.step(energy, active_settings.vad_threshold, kVadAttack,
                                  std::chrono::milliseconds(active_settings.hold_ms))) {
                    voice_active = true;
                }
                level = std::max(level, energy);
            }
            if (mic_monitor != nullptr && mic_monitor->audio_seen()) {
                const uint64_t frames = mic_monitor->frames();
                const double energy = frames == last_mic_frames ? 0.0 : mic_monitor->energy();
                last_mic_frames = frames;
                if (mic_vad.step(energy, active_settings.mic_vad_threshold, kVadAttack,
                                 std::chrono::milliseconds(active_settings.hold_ms))) {
                    voice_active = true;
                }
                level = std::max(level, energy);
            }

            if (voice_active != ducked) {
                if (voice_active) {
                    // Cancel any release fade in progress and fade down from the
                    // volume we are at right now, not from 100%.
                    session->start_ramp(active_settings.duck_percent,
                                        active_settings.attack_fade_ms);
                    applied_duck_percent = active_settings.duck_percent;
                    DuckingEvent event;
                    event.type = DuckingEventType::VoiceActive;
                    event.level = level;
                    event.duck_percent = active_settings.duck_percent;
                    on_event(event);
                } else {
                    session->start_ramp(100.0, active_settings.release_fade_ms);
                    applied_duck_percent.reset();
                    DuckingEvent event;
                    event.type = DuckingEventType::VoiceInactive;
                    event.level = level;
                    on_event(event);
                }
                ducked = voice_active;
            } else if (ducked && applied_duck_percent != active_settings.duck_percent) {
                session->start_ramp(active_settings.duck_percent, active_settings.attack_fade_ms);
                applied_duck_percent = active_settings.duck_percent;
                DuckingEvent event;
                event.type = DuckingEventType::VoiceActive;
                event.level = level;
                event.duck_percent = active_settings.duck_percent;
                on_event(event);
            }

            // Drive the fade from the control tick instead of sleeping inside
            // it. This keeps VAD sampling alive during attack and release.
            session->tick_ramp();

            std::this_thread::sleep_for(kTickInterval);
        }

        if (call_monitor != nullptr) {
            call_monitor->stop();
        }
        if (mic_monitor != nullptr) {
            mic_monitor->stop();
        }

        const DuckingSettings final_settings = settings.get();
        const int fade = std::min(std::max(final_settings.release_fade_ms, 0), 400);
        if (ducked || session->volume_percent() < 99.9) {
            session->ramp_to(100.0, fade, nullptr);
        }
        session->set_neutral();
        session->stop();

        if (stop.load() || end == SessionEnd::StopRequested) {
            break;
        }
    }
}

} // namespace pipeduck
