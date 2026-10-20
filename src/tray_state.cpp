#include "pipeduck/tray_state.hpp"

#include "pipeduck/common.hpp"

namespace pipeduck {
namespace {

bool assign(TrayStatus& status, TrayState state, const std::string& message, bool& changed) {
    if (status.state != state) {
        status.state = state;
        changed = true;
    }
    if (status.message != message) {
        status.message = message;
        changed = true;
    }
    return changed;
}

} // namespace

bool tray_state_visible(TrayState state) {
    return state == TrayState::Waiting || state == TrayState::Neutral || state == TrayState::Ducked;
}

bool tray_state_attention(TrayState state) {
    return state == TrayState::Recovering || state == TrayState::Degraded ||
           state == TrayState::Error;
}

std::string tray_state_value(TrayState state) {
    return tray_state_visible(state) ? "ON" : "OFF";
}

TrayReduction reduce_tray_event(TrayStatus status, const DuckingEvent& event) {
    bool changed = false;
    switch (event.type) {
    case DuckingEventType::WaitingForSource:
        assign(status, TrayState::Waiting,
               event.message.empty() ? "Waiting for configured voice source" : event.message,
               changed);
        break;
    case DuckingEventType::Started:
        assign(status, TrayState::Neutral,
               "Ready: " + event.label + ", threshold=" + format_double(event.threshold, 4) +
                   ", start=" + format_double(event.start_threshold, 4) +
                   ", hold=" + std::to_string(event.hold_ms) + "ms",
               changed);
        break;
    case DuckingEventType::VoiceActive:
        assign(status, TrayState::Ducked,
               "Voice active: level=" + format_double(event.level, 4) + ", ducking " +
                   std::to_string(event.duck_percent) + "%",
               changed);
        break;
    case DuckingEventType::VoiceInactive:
        assign(status, TrayState::Neutral, "Voice inactive: level=" + format_double(event.level, 4),
               changed);
        break;
    case DuckingEventType::RoutingFailed:
        assign(status, TrayState::Recovering, "Routing failed: " + event.message + "; retrying",
               changed);
        break;
    case DuckingEventType::Recovering:
        assign(status, TrayState::Recovering,
               event.message.empty() ? "Rebuilding the route" : event.message, changed);
        break;
    case DuckingEventType::Degraded:
        assign(status, TrayState::Degraded,
               event.message.empty() ? "Ducking is degraded" : event.message, changed);
        break;
    case DuckingEventType::RestoreFailed:
        assign(status, TrayState::Error,
               "Restore incomplete: " + (event.message.empty()
                                             ? std::string("some streams were not restored")
                                             : event.message),
               changed);
        break;
    case DuckingEventType::Message:
        assign(status, status.state, event.message, changed);
        break;
    }
    return {status, changed};
}

TrayReduction reduce_worker_stopped(TrayStatus status) {
    bool changed = false;
    if (status.state == TrayState::Error) {
        // A partial cleanup or unrecoverable failure must stay visible.
        return {status, false};
    }
    assign(status, TrayState::Idle, "Off", changed);
    return {status, changed};
}

TrayReduction reduce_worker_failed(TrayStatus status, const std::string& message) {
    bool changed = false;
    assign(status, TrayState::Error, message, changed);
    return {status, changed};
}

} // namespace pipeduck
