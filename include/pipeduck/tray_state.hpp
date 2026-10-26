#pragma once

#include <optional>
#include <string>

#include "pipeduck/duck.hpp"

namespace pipeduck {

// Tray lifecycle. Only states in which a route session is active (or the worker
// is legitimately waiting for a configured source) report Ducking ON.
enum class TrayState {
    Idle,
    Waiting,
    Starting,
    Neutral,
    Ducked,
    Recovering,
    Degraded,
    Stopping,
    Error,
};

struct TrayStatus {
    TrayState state = TrayState::Idle;
    std::string message = "Ready";
    std::optional<std::string> source_label;
    std::optional<std::string> mic_label;
};

struct TrayReduction {
    TrayStatus status;
    bool changed = false;
};

bool tray_state_visible(TrayState state);
bool tray_state_attention(TrayState state);
std::string tray_state_value(TrayState state);

// The Ducking toggle is usable unless the tray is quitting or a worker
// transition is already in flight. In particular an error or an incomplete
// restore must never be a dead end.
bool tray_can_toggle(TrayState state, bool quitting);

// Routing identity (trigger, source, microphone, profile) is read-only while a
// route session is active. Waiting for the configured source keeps it editable
// so a stream that disappeared and was re-created can be replaced, and a stale
// worker flag must not keep the menu locked after the worker already exited.
bool tray_editing_locked(TrayState state, bool worker_active, bool quitting);

// Pure event-to-state reducer. A successful routing event is the only thing
// that clears a degraded/recovering state.
TrayReduction reduce_tray_event(TrayStatus status, const DuckingEvent& event);

// Worker terminated on its own. An error state is preserved so partial cleanup
// or an unrecoverable failure is not reported as a clean "Off".
TrayReduction reduce_worker_stopped(TrayStatus status);

// Worker terminated with an unrecoverable error.
TrayReduction reduce_worker_failed(TrayStatus status, const std::string& message);

} // namespace pipeduck
