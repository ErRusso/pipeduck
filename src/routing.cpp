#include "pipeduck/routing.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "pipeduck/pipewire.hpp"
#include "pipeduck/volume.hpp"

namespace pipeduck {
namespace {

constexpr std::chrono::milliseconds kLinkTimeout{3000};

struct PortEntry {
    std::string full_name;
    std::string channel;
};

std::vector<PortEntry> ports_for_node(const std::string& port_list, const std::string& node_name,
                                      const std::string& port_prefix) {
    std::vector<PortEntry> ports;
    const std::string node_prefix = node_name + ":";
    for (const auto& line : split_lines(port_list)) {
        const std::string full_name = trim(line);
        if (!starts_with(full_name, node_prefix)) {
            continue;
        }
        const std::string port_name = full_name.substr(node_prefix.size());
        if (!starts_with(port_name, port_prefix)) {
            continue;
        }
        ports.push_back({full_name, port_name.substr(port_prefix.size())});
    }
    return ports;
}

std::optional<PortEntry> find_channel(const std::vector<PortEntry>& ports,
                                      const std::string& channel) {
    for (const auto& port : ports) {
        if (port.channel == channel) {
            return port;
        }
    }
    return std::nullopt;
}

std::vector<PortEntry> choose_stereo_ports(const std::vector<PortEntry>& ports) {
    for (const auto& pair : {std::pair{"FL", "FR"}, std::pair{"AUX0", "AUX1"}}) {
        const auto left = find_channel(ports, pair.first);
        const auto right = find_channel(ports, pair.second);
        if (left.has_value() && right.has_value()) {
            return {*left, *right};
        }
    }
    std::vector<PortEntry> result;
    for (std::size_t i = 0; i < std::min<std::size_t>(ports.size(), 2); ++i) {
        result.push_back(ports[i]);
    }
    return result;
}

void system_create_link(const PortLink& link) {
    run_status_checked("pw-link", {link.output, link.input}, kLinkTimeout);
}

void system_unlink_port(const PortLink& link) {
    run_status_checked("pw-link", {"-d", link.output, link.input}, kLinkTimeout);
}

class PulseRoutingSink final : public RoutingSink {
public:
    explicit PulseRoutingSink(std::unique_ptr<VirtualSink> sink) : sink_(std::move(sink)) {}

    const std::string& name() const override {
        return sink_->name();
    }
    void destroy() override {
        sink_->destroy();
    }
    void abandon() override {
        sink_->abandon();
    }

private:
    std::unique_ptr<VirtualSink> sink_;
};

class PulseRoutingVolume final : public RoutingVolume {
public:
    bool available() const override {
        return volume_.native();
    }
    bool set_percent(const std::string& source_name, double percent,
                     bool allow_subprocess_fallback) override {
        return volume_.set_percent(source_name, percent, allow_subprocess_fallback);
    }

private:
    VolumeController volume_;
};

class PulseRoutingBackend final : public RoutingBackend {
public:
    explicit PulseRoutingBackend(bool allow_subprocess_fallback)
        : pulse_(allow_subprocess_fallback) {}

    std::string default_sink_name() override {
        return pulse_.default_sink_name();
    }
    std::optional<Sink> sink_by_name(const std::string& name) override {
        return pulse_.sink_by_name(name);
    }
    std::optional<Sink> sink_by_index(uint32_t index) override {
        return pulse_.sink_by_index(index);
    }
    Sink wait_for_sink(const std::string& name, std::chrono::milliseconds timeout) override {
        return pulse_.wait_for_sink(name, timeout);
    }
    std::vector<SinkInput> sink_inputs() override {
        return pulse_.sink_inputs();
    }
    void set_default_sink(const std::string& name) override {
        pulse_.set_default_sink(name);
    }
    void move_sink_input(uint32_t input_index, const std::string& sink_name) override {
        pulse_.move_sink_input(input_index, sink_name);
    }
    void move_sink_input_to_index(uint32_t input_index, uint32_t sink_index) override {
        pulse_.move_sink_input_to_index(input_index, sink_index);
    }

    std::unique_ptr<RoutingSink> create_sink(const std::string& prefix) override {
        return std::make_unique<PulseRoutingSink>(VirtualSink::create(prefix));
    }
    std::unique_ptr<RoutingVolume> create_volume() override {
        return std::make_unique<PulseRoutingVolume>();
    }

    std::string list_output_ports() override {
        return run_command_checked("pw-link", {"-o"}, kLinkTimeout);
    }
    std::string list_input_ports() override {
        return run_command_checked("pw-link", {"-i"}, kLinkTimeout);
    }
    std::string list_links() override {
        return run_command_checked("pw-link", {"-l"}, kLinkTimeout);
    }
    void create_link(const PortLink& link) override {
        try {
            system_create_link(link);
        } catch (const std::exception& error) {
            throw std::runtime_error("could not link " + link.output + " -> " + link.input + ": " +
                                     error.what());
        }
    }
    void remove_link(const PortLink& link) override {
        try {
            system_unlink_port(link);
        } catch (const std::exception& error) {
            throw std::runtime_error("could not unlink " + link.output + " -> " + link.input +
                                     ": " + error.what());
        }
    }

private:
    PulseCtl pulse_;
};

} // namespace

std::shared_ptr<RoutingBackend> make_pulse_routing_backend(bool allow_subprocess_fallback) {
    return std::make_shared<PulseRoutingBackend>(allow_subprocess_fallback);
}

std::vector<PortLink> monitor_links_for_ports(const std::string& outputs, const std::string& inputs,
                                              const std::string& virtual_sink,
                                              const std::string& real_sink) {
    auto monitor_ports = ports_for_node(outputs, virtual_sink, "monitor_");
    auto playback_ports = ports_for_node(inputs, real_sink, "playback_");

    if (monitor_ports.empty()) {
        throw std::runtime_error("no monitor ports found for virtual sink " + virtual_sink);
    }
    if (playback_ports.empty()) {
        throw std::runtime_error("no playback ports found for sink " + real_sink);
    }

    monitor_ports = choose_stereo_ports(monitor_ports);
    playback_ports = choose_stereo_ports(playback_ports);
    const std::size_t link_count =
        std::min<std::size_t>({monitor_ports.size(), playback_ports.size(), 2});
    if (link_count == 0) {
        throw std::runtime_error("could not pair monitor ports for " + virtual_sink + " with " +
                                 real_sink);
    }

    std::vector<PortLink> links;
    for (std::size_t i = 0; i < link_count; ++i) {
        links.push_back({monitor_ports[i].full_name, playback_ports[i].full_name});
    }
    return links;
}

std::vector<PortLink> parse_link_list(const std::string& listing) {
    std::vector<PortLink> links;
    std::string current;
    for (const auto& raw_line : split_lines(listing)) {
        const std::string line = trim(raw_line);
        if (line.empty()) {
            continue;
        }
        if (starts_with(line, "|->") || starts_with(line, "|<-")) {
            const std::string other = trim(line.substr(3));
            if (current.empty() || other.empty()) {
                continue;
            }
            if (starts_with(line, "|->")) {
                links.push_back({current, other});
            } else {
                links.push_back({other, current});
            }
            continue;
        }
        current = line;
    }
    return links;
}

std::vector<PortLink> missing_links(const std::vector<PortLink>& desired,
                                    const std::vector<PortLink>& existing) {
    std::vector<PortLink> result;
    for (const auto& link : desired) {
        if (std::find(existing.begin(), existing.end(), link) == existing.end()) {
            result.push_back(link);
        }
    }
    return result;
}

LinkPlan plan_link_changes(const std::vector<PortLink>& desired, const std::vector<PortLink>& owned,
                           const std::vector<PortLink>& existing) {
    LinkPlan plan;
    plan.create = missing_links(desired, existing);
    for (const auto& link : owned) {
        if (std::find(desired.begin(), desired.end(), link) == desired.end()) {
            plan.remove.push_back(link);
        }
    }
    return plan;
}

std::string describe_cleanup(const RouteCleanupResult& result) {
    if (result.complete) {
        return "all routed streams restored";
    }
    std::string message =
        result.detail.empty() ? "could not restore every routed stream" : result.detail;
    if (result.inspection_failed) {
        message += " (could not inspect the audio graph)";
    }
    if (!result.failed_inputs.empty()) {
        message += " (streams:";
        for (const uint32_t index : result.failed_inputs) {
            message += " #" + std::to_string(index);
        }
        message += ")";
    }
    if (!result.skipped_inputs.empty()) {
        message += " (skipped:";
        for (const uint32_t index : result.skipped_inputs) {
            message += " #" + std::to_string(index);
        }
        message += ")";
    }
    if (result.sink_left_alive) {
        message += "; the virtual sink was left alive to avoid terminating audio";
    }
    return message;
}

RoutedStream routed_stream_from_input(const SinkInput& input, uint32_t original_sink) {
    RoutedStream stream;
    stream.index = input.index;
    stream.original_sink = original_sink;
    stream.label = source_label(input.identity());
    const auto serial = input.properties.find("object.serial");
    if (serial != input.properties.end()) {
        stream.object_serial = serial->second;
    }
    const auto node = input.properties.find("node.name");
    if (node != input.properties.end()) {
        stream.node_name = node->second;
    }
    return stream;
}

bool RoutedStream::matches(const SinkInput& input) const {
    if (!object_serial.empty()) {
        const auto serial = input.properties.find("object.serial");
        return serial != input.properties.end() && serial->second == object_serial;
    }
    if (!node_name.empty()) {
        const auto node = input.properties.find("node.name");
        return node != input.properties.end() && node->second == node_name;
    }
    // Older servers may not expose a stable serial or node name. The label is
    // the best remaining fingerprint; a mismatch is safer than moving an
    // unrelated stream.
    return !label.empty() && source_label(input.identity()) == label;
}

std::vector<RoutedStream> reconcile_routed_streams(std::vector<RoutedStream> routed,
                                                   const std::vector<SinkInput>& current,
                                                   uint32_t virtual_sink_index) {
    std::vector<RoutedStream> result;
    result.reserve(routed.size());
    for (auto& stream : routed) {
        const auto found =
            std::find_if(current.begin(), current.end(),
                         [&stream](const auto& input) { return input.index == stream.index; });
        if (found == current.end()) {
            continue;
        }
        if (found->sink != virtual_sink_index) {
            continue;
        }
        if (!stream.matches(*found)) {
            continue;
        }
        result.push_back(std::move(stream));
    }
    return result;
}

RestorePassResult
restore_routed_streams(std::vector<RoutedStream>& routed, const std::vector<SinkInput>& current,
                       uint32_t virtual_sink_index,
                       const std::function<bool(uint32_t, uint32_t)>& move_to_index,
                       const std::function<bool(uint32_t, const std::string&)>& move_to_name,
                       const std::string& fallback_sink_name) {
    RestorePassResult result;
    for (auto stream = routed.begin(); stream != routed.end();) {
        const auto found =
            std::find_if(current.begin(), current.end(),
                         [&stream](const auto& input) { return input.index == stream->index; });
        if (found == current.end()) {
            result.skipped.push_back(stream->index);
            stream = routed.erase(stream);
            continue;
        }
        if (!stream->matches(*found)) {
            // The server reused this index for another stream. Never move it.
            result.skipped.push_back(stream->index);
            stream = routed.erase(stream);
            continue;
        }
        if (found->sink != virtual_sink_index) {
            // Already restored or moved by the user.
            result.skipped.push_back(stream->index);
            stream = routed.erase(stream);
            continue;
        }

        ++result.attempted;
        const bool moved = move_to_index(stream->index, stream->original_sink) ||
                           move_to_name(stream->index, fallback_sink_name);
        if (moved) {
            result.restored.push_back(stream->index);
            stream = routed.erase(stream);
        } else {
            result.failed.push_back(stream->index);
            ++stream;
        }
    }
    return result;
}

struct RouteSession::Impl {
    std::shared_ptr<RoutingBackend> backend;
    std::unique_ptr<RoutingSink> virtual_sink;
    std::string virtual_sink_name;
    uint32_t virtual_sink_index = 0;
    std::string real_sink_name;
    uint32_t real_sink_index = 0;
    std::vector<RoutedStream> routed_inputs;
    std::vector<PortLink> monitor_links;
    std::unique_ptr<RoutingVolume> volume;
    VolumeRamp ramp;
    std::chrono::milliseconds restore_timeout{3000};
    std::chrono::milliseconds retry_interval{50};
    std::chrono::milliseconds volume_retry_initial{500};
    std::chrono::milliseconds volume_retry_max{4000};
    // Bounded retry state for monitor-volume updates. The control tick must not
    // hammer libpulse or spawn a subprocess when the backend is lost.
    std::chrono::steady_clock::time_point volume_retry_at{};
    bool volume_retry_scheduled = false;
    int volume_failures = 0;
    // The last fade value the backend rejected. Retried from tick_ramp() so a
    // transient backend loss does not silently leave the wrong volume applied.
    bool pending_volume = false;
    double pending_percent = 100.0;
    bool routing_started = false;
    bool stopped = false;
    RouteCleanupResult cleanup_result;

    std::string monitor_source_name() const {
        return virtual_sink_name + ".monitor";
    }
};

RouteSession::RouteSession() : impl_(std::make_unique<Impl>()) {}

RouteSession::RouteSession(RouteSession&&) noexcept = default;

RouteSession::~RouteSession() {
    try {
        stop();
    } catch (...) {
    }
}

const std::string& RouteSession::virtual_sink_name() const {
    return impl_->virtual_sink_name;
}

const std::string& RouteSession::real_sink_name() const {
    return impl_->real_sink_name;
}

bool RouteSession::set_percent(double percent, bool allow_subprocess_fallback) {
    if (impl_ == nullptr || impl_->stopped || impl_->volume == nullptr) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!allow_subprocess_fallback && impl_->volume_retry_scheduled &&
        now < impl_->volume_retry_at) {
        // Still in the bounded backoff window: skip without touching the
        // backend. The caller sees a failure and reports a degraded session.
        return false;
    }
    if (!allow_subprocess_fallback && !impl_->volume->available() && impl_->backend != nullptr) {
        // Recreate the volume context at the retry boundary so a backend that
        // came back is picked up without restarting the whole session.
        impl_->volume = impl_->backend->create_volume();
        if (impl_->volume == nullptr) {
            return false;
        }
    }
    if (!impl_->volume->set_percent(impl_->monitor_source_name(), percent,
                                    allow_subprocess_fallback)) {
        if (!allow_subprocess_fallback) {
            ++impl_->volume_failures;
            const int doublings = std::min(impl_->volume_failures - 1, 8);
            auto delay = impl_->volume_retry_initial * (1 << doublings);
            if (delay > impl_->volume_retry_max) {
                delay = impl_->volume_retry_max;
            }
            impl_->volume_retry_at = now + delay;
            impl_->volume_retry_scheduled = true;
        }
        return false;
    }
    impl_->volume_failures = 0;
    impl_->volume_retry_scheduled = false;
    volume_percent_ = percent;
    impl_->pending_volume = false;
    return true;
}

bool RouteSession::start_ramp(double percent, int duration_ms) {
    if (impl_ == nullptr || impl_->stopped) {
        return false;
    }
    impl_->ramp.start(volume_percent_, percent, duration_ms);
    if (!impl_->ramp.active()) {
        const bool applied = set_percent(percent);
        if (!applied) {
            impl_->pending_percent = percent;
            impl_->pending_volume = true;
        }
        return applied;
    }
    impl_->pending_volume = false;
    return true;
}

bool RouteSession::tick_ramp() {
    if (impl_ == nullptr || impl_->stopped) {
        return true;
    }
    if (!impl_->ramp.active()) {
        // Retry a value that the backend rejected, even after the fade itself
        // has finished, so recovery is not limited to the fade duration.
        if (!impl_->pending_volume) {
            return true;
        }
        return set_percent(impl_->pending_percent);
    }
    const double value = impl_->ramp.update();
    const bool applied = set_percent(value);
    if (!applied) {
        impl_->pending_percent = value;
        impl_->pending_volume = true;
    }
    return applied;
}

bool RouteSession::ramp_active() const {
    return impl_ != nullptr && !impl_->stopped && impl_->ramp.active();
}

void RouteSession::ramp_to(double percent, int duration_ms, const std::atomic<bool>* stop) {
    if (impl_ == nullptr || impl_->stopped) {
        return;
    }
    if (!start_ramp(percent, duration_ms)) {
        set_percent(percent, true);
        return;
    }
    while (impl_->ramp.active()) {
        if (stop != nullptr && stop->load()) {
            impl_->ramp.cancel();
            break;
        }
        tick_ramp();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void RouteSession::set_neutral(int fade_ms) {
    if (fade_ms > 0) {
        ramp_to(100.0, fade_ms);
    } else {
        set_percent(100.0, true);
    }
}

const RouteCleanupResult& RouteSession::cleanup_result() const {
    static const RouteCleanupResult empty;
    return impl_ != nullptr ? impl_->cleanup_result : empty;
}

std::size_t
RouteSession::route_existing_outputs(const std::optional<ConfiguredSource>& voice_source) {
    if (impl_ == nullptr || impl_->stopped || impl_->backend == nullptr) {
        return 0;
    }
    const auto inputs = impl_->backend->sink_inputs();
    impl_->routed_inputs = reconcile_routed_streams(std::move(impl_->routed_inputs), inputs,
                                                    impl_->virtual_sink_index);
    std::size_t moved = 0;
    for (const auto& input : inputs) {
        const AudioIdentity identity = input.identity();
        if (!identity.is_playback_stream()) {
            continue;
        }
        if (voice_source.has_value() && identity.matches_configured_source(*voice_source)) {
            continue;
        }
        if (input.sink == impl_->virtual_sink_index) {
            const auto known = std::find_if(
                impl_->routed_inputs.begin(), impl_->routed_inputs.end(),
                [&input](const RoutedStream& stream) { return stream.index == input.index; });
            if (known == impl_->routed_inputs.end()) {
                // A stream that was already on our sink (for example, left by a
                // previous session) is restored to the real sink on teardown.
                impl_->routed_inputs.push_back(
                    routed_stream_from_input(input, impl_->real_sink_index));
            }
            continue;
        }
        if (input.sink != impl_->real_sink_index) {
            continue;
        }
        try {
            impl_->backend->move_sink_input(input.index, impl_->virtual_sink_name);
            // Record only after the move succeeds, so a failed move is never
            // later presented as a successfully restored stream.
            impl_->routed_inputs.push_back(routed_stream_from_input(input, input.sink));
            ++moved;
        } catch (const std::exception& error) {
            std::cerr << "pipeduck: could not route sink-input #" << input.index << ": "
                      << error.what() << "\n";
        }
    }
    return moved;
}

bool RouteSession::ensure_monitor_links() {
    if (impl_ == nullptr || impl_->stopped || impl_->virtual_sink == nullptr ||
        impl_->backend == nullptr) {
        return false;
    }
    try {
        const std::string listing = impl_->backend->list_links();
        const auto existing = parse_link_list(listing);
        const auto plan = plan_link_changes(impl_->monitor_links, impl_->monitor_links, existing);
        for (const auto& link : plan.create) {
            impl_->backend->create_link(link);
        }
        return true;
    } catch (const std::exception& error) {
        std::cerr << "pipeduck: could not verify monitor links: " << error.what() << "\n";
        return false;
    }
}

GraphHealth RouteSession::check_graph() {
    if (impl_ == nullptr || impl_->stopped || impl_->backend == nullptr) {
        return GraphHealth::BackendUnavailable;
    }
    try {
        // Check both the real output and the virtual sink by their stable
        // names. This is cheap (event-driven cache) and never spawns a process
        // when the backend was created in non-blocking mode.
        if (!impl_->backend->sink_by_name(impl_->real_sink_name).has_value() ||
            !impl_->backend->sink_by_name(impl_->virtual_sink_name).has_value()) {
            return GraphHealth::SinkMissing;
        }
    } catch (const std::exception&) {
        return GraphHealth::BackendUnavailable;
    }
    return GraphHealth::Healthy;
}

RouteCleanupResult RouteSession::stop() {
    if (impl_ == nullptr) {
        return {};
    }
    if (impl_->stopped) {
        return impl_->cleanup_result;
    }
    impl_->stopped = true;

    RouteCleanupResult result;
    if (impl_->virtual_sink == nullptr) {
        impl_->volume.reset();
        impl_->cleanup_result = result;
        return result;
    }

    if (impl_->volume != nullptr) {
        impl_->volume->set_percent(impl_->monitor_source_name(), 100.0, true);
        volume_percent_ = 100.0;
    }

    const auto backend = impl_->backend;
    if (backend == nullptr) {
        impl_->volume.reset();
        impl_->cleanup_result = result;
        return result;
    }
    const auto move_to_index = [backend, this](uint32_t index, uint32_t sink_index) {
        try {
            // Sink indices can be reused too. Only use the original index while
            // it still refers to the sink the session started with; otherwise
            // fall back to the recorded name.
            const auto sink = backend->sink_by_index(sink_index);
            if (!sink.has_value() || sink->name != impl_->real_sink_name) {
                return false;
            }
            backend->move_sink_input_to_index(index, sink_index);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    const auto move_to_name = [backend](uint32_t index, const std::string& sink_name) {
        try {
            backend->move_sink_input(index, sink_name);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    const auto add_unique = [](std::vector<uint32_t>& values, uint32_t value) {
        if (std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(value);
        }
    };

    // Never restore the captured default here: the user may have switched
    // outputs while ducking was active. Only the streams we moved are restored.
    const auto deadline = std::chrono::steady_clock::now() + impl_->restore_timeout;
    std::vector<uint32_t> remaining;
    for (;;) {
        std::vector<SinkInput> current;
        try {
            current = backend->sink_inputs();
            result.inspection_failed = false;
            result.detail.clear();
        } catch (const std::exception& error) {
            result.inspection_failed = true;
            result.detail = std::string("could not inspect routed streams: ") + error.what();
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(impl_->retry_interval);
            continue;
        }

        impl_->routed_inputs = reconcile_routed_streams(std::move(impl_->routed_inputs), current,
                                                        impl_->virtual_sink_index);
        const RestorePassResult pass =
            restore_routed_streams(impl_->routed_inputs, current, impl_->virtual_sink_index,
                                   move_to_index, move_to_name, impl_->real_sink_name);
        for (const uint32_t index : pass.skipped) {
            add_unique(result.skipped_inputs, index);
        }
        for (const uint32_t index : pass.failed) {
            add_unique(result.failed_inputs, index);
        }

        remaining.clear();
        for (const auto& input : current) {
            if (input.sink == impl_->virtual_sink_index) {
                add_unique(remaining, input.index);
            }
        }
        if (impl_->routed_inputs.empty() && remaining.empty()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(impl_->retry_interval);
    }

    if (result.inspection_failed) {
        try {
            if (!backend->sink_by_name(impl_->virtual_sink_name).has_value()) {
                // The backend restarted and our sink is gone, so no stream can
                // still be attached to it. There is nothing left to restore.
                result.inspection_failed = false;
                result.complete = true;
                result.detail = "the virtual sink disappeared with the audio backend";
                impl_->volume.reset();
                impl_->cleanup_result = result;
                return result;
            }
        } catch (const std::exception&) {
            // The backend is unreachable and we cannot prove the sink is gone.
            // Keep the safety rule and leave it alone.
        }
    }

    if (!impl_->routed_inputs.empty() || !remaining.empty() || result.inspection_failed) {
        result.complete = false;
        result.sink_left_alive = true;
        for (const uint32_t index : remaining) {
            add_unique(result.failed_inputs, index);
        }
        if (result.detail.empty()) {
            result.detail = "virtual sink still has " + std::to_string(remaining.size()) +
                            " stream(s) that could not be moved";
        }
        std::cerr << "pipeduck: warning: " << describe_cleanup(result) << "\n";
        impl_->virtual_sink->abandon();
        impl_->volume.reset();
        impl_->cleanup_result = result;
        return result;
    }

    std::string unlink_error;
    for (const auto& link : impl_->monitor_links) {
        try {
            backend->remove_link(link);
        } catch (const std::exception& error) {
            unlink_error = error.what();
        }
    }
    impl_->monitor_links.clear();
    try {
        impl_->virtual_sink->destroy();
    } catch (const std::exception& error) {
        result.complete = false;
        result.sink_left_alive = true;
        result.detail = std::string("could not destroy the virtual sink: ") + error.what();
    }
    if (!unlink_error.empty() && result.detail.empty()) {
        // Destroying the sink removes its links anyway, so this is diagnostic
        // only and does not make the cleanup incomplete.
        result.detail = "could not remove a monitor link: " + unlink_error;
    }
    impl_->volume.reset();
    impl_->cleanup_result = result;
    return result;
}

RoutingEngine::RoutingEngine(std::optional<ConfiguredSource> voice_source,
                             std::optional<ConfiguredMic> mic_source,
                             std::shared_ptr<RoutingBackend> backend)
    : voice_source_(std::move(voice_source)), mic_source_(std::move(mic_source)),
      backend_(std::move(backend)) {}

RouteSession RoutingEngine::start(const RoutingOptions& options) {
    if (voice_source_.has_value() && !voice_source_->has_match_fields()) {
        throw std::runtime_error("configured voice source contains no stable match fields");
    }

    const auto backend = backend_ != nullptr ? backend_ : make_pulse_routing_backend();
    const std::string original_default = backend->default_sink_name();

    // Always route against the current default output. The voice-call stream is
    // excluded from ducking, but it no longer pins routing to a stale device.
    const std::optional<Sink> real_sink = backend->sink_by_name(original_default);
    if (!real_sink.has_value()) {
        throw std::runtime_error("default sink " + original_default + " not found");
    }

    RouteSession session;
    RouteSession::Impl& impl = *session.impl_;
    impl.backend = backend;
    impl.restore_timeout = options.restore_timeout;
    impl.retry_interval = options.retry_interval;
    impl.volume_retry_initial = options.volume_retry_initial;
    impl.volume_retry_max = options.volume_retry_max;
    impl.real_sink_name = real_sink->name;
    impl.real_sink_index = real_sink->index;
    impl.volume = backend->create_volume();
    impl.virtual_sink = backend->create_sink(options.virtual_sink_prefix);
    if (impl.virtual_sink == nullptr || impl.volume == nullptr) {
        throw std::runtime_error("could not initialize the virtual sink");
    }
    impl.virtual_sink_name = impl.virtual_sink->name();
    impl.virtual_sink_index =
        backend->wait_for_sink(impl.virtual_sink_name, options.sink_appear_timeout).index;

    try {
        backend->set_default_sink(original_default);
    } catch (const std::exception& error) {
        throw std::runtime_error("failed to restore the default sink: " +
                                 std::string(error.what()));
    }

    const auto deadline = std::chrono::steady_clock::now() + options.port_appear_timeout;
    std::string last_error;
    for (;;) {
        try {
            const std::string outputs = backend->list_output_ports();
            const std::string inputs = backend->list_input_ports();
            const auto desired = monitor_links_for_ports(outputs, inputs, impl.virtual_sink_name,
                                                         impl.real_sink_name);

            std::vector<PortLink> existing;
            try {
                existing = parse_link_list(backend->list_links());
            } catch (const std::exception& error) {
                // Creating a link that already exists is harmless enough to retry
                // on the next pass; keep the original error for diagnostics.
                last_error = std::string("could not list existing links: ") + error.what();
            }

            const auto plan = plan_link_changes(desired, impl.monitor_links, existing);

            // Keep links we still need, and drop ownership only after a link was
            // actually removed. A failed removal stays owned so teardown retries
            // it instead of leaking the link.
            std::vector<PortLink> next_owned;
            for (const auto& link : impl.monitor_links) {
                if (std::find(desired.begin(), desired.end(), link) != desired.end()) {
                    next_owned.push_back(link);
                }
            }
            for (const auto& link : plan.remove) {
                try {
                    backend->remove_link(link);
                } catch (const std::exception&) {
                    next_owned.push_back(link);
                }
            }
            impl.monitor_links = std::move(next_owned);
            for (const auto& link : plan.create) {
                backend->create_link(link);
                // Record each link as soon as it exists, so a later failure and
                // retry never loses track of a partial attempt.
                if (std::find(impl.monitor_links.begin(), impl.monitor_links.end(), link) ==
                    impl.monitor_links.end()) {
                    impl.monitor_links.push_back(link);
                }
            }
            impl.routing_started = true;
            break;
        } catch (const std::exception& error) {
            last_error = error.what();
            if (std::chrono::steady_clock::now() >= deadline) {
                // A startup failure must not leave partially created links behind.
                for (const auto& link : impl.monitor_links) {
                    try {
                        backend->remove_link(link);
                    } catch (const std::exception&) {
                    }
                }
                impl.monitor_links.clear();
                throw std::runtime_error("could not route the virtual sink through " +
                                         impl.real_sink_name + ": " + last_error);
            }
            std::this_thread::sleep_for(options.retry_interval);
        }
    }

    if (!session.set_percent(100.0, true)) {
        throw std::runtime_error("could not initialize the virtual sink monitor volume");
    }
    session.route_existing_outputs(voice_source_);
    return session;
}

} // namespace pipeduck
