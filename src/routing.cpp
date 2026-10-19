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
constexpr std::chrono::milliseconds kPortAppearTimeout{2000};

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

void create_link(const PortLink& link) {
    run_status_checked("pw-link", {link.output, link.input}, kLinkTimeout);
}

void unlink_port(const PortLink& link) {
    run_status_checked("pw-link", {"-d", link.output, link.input}, kLinkTimeout);
}

} // namespace

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

struct RouteSession::Impl {
    std::unique_ptr<VirtualSink> virtual_sink;
    std::string virtual_sink_name;
    uint32_t virtual_sink_index = 0;
    std::string real_sink_name;
    uint32_t real_sink_index = 0;
    std::map<uint32_t, uint32_t> moved_inputs;
    std::vector<PortLink> monitor_links;
    std::unique_ptr<VolumeController> volume;
    VolumeRamp ramp;
    bool stopped = false;

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

void RouteSession::set_percent(double percent) {
    if (impl_ == nullptr || impl_->stopped) {
        return;
    }
    impl_->volume->set_percent(impl_->monitor_source_name(), percent);
    volume_percent_ = percent;
}

void RouteSession::start_ramp(double percent, int duration_ms) {
    if (impl_ == nullptr || impl_->stopped) {
        return;
    }
    impl_->ramp.start(volume_percent_, percent, duration_ms);
    if (!impl_->ramp.active()) {
        set_percent(percent);
    }
}

void RouteSession::tick_ramp() {
    if (impl_ == nullptr || impl_->stopped || !impl_->ramp.active()) {
        return;
    }
    const double value = impl_->ramp.update();
    set_percent(value);
}

bool RouteSession::ramp_active() const {
    return impl_ != nullptr && !impl_->stopped && impl_->ramp.active();
}

void RouteSession::ramp_to(double percent, int duration_ms, const std::atomic<bool>* stop) {
    if (impl_ == nullptr || impl_->stopped) {
        return;
    }
    start_ramp(percent, duration_ms);
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
        set_percent(100.0);
    }
}

std::size_t
RouteSession::route_existing_outputs(const std::optional<ConfiguredSource>& voice_source) {
    if (impl_ == nullptr || impl_->stopped) {
        return 0;
    }
    const PulseCtl pulse;
    std::size_t moved = 0;
    for (const auto& input : pulse.sink_inputs()) {
        const AudioIdentity identity = input.identity();
        if (!identity.is_playback_stream()) {
            continue;
        }
        if (voice_source.has_value() && identity.matches_configured_source(*voice_source)) {
            continue;
        }
        if (input.sink == impl_->virtual_sink_index) {
            impl_->moved_inputs.emplace(input.index, impl_->real_sink_index);
            continue;
        }
        if (input.sink != impl_->real_sink_index) {
            continue;
        }
        impl_->moved_inputs.emplace(input.index, input.sink);
        try {
            pulse.move_sink_input(input.index, impl_->virtual_sink_name);
            ++moved;
        } catch (const std::exception& error) {
            std::cerr << "pipeduck: could not route sink-input #" << input.index << ": "
                      << error.what() << "\n";
        }
    }
    return moved;
}

void RouteSession::stop() {
    if (impl_ == nullptr || impl_->stopped) {
        return;
    }
    if (impl_->virtual_sink == nullptr) {
        impl_->stopped = true;
        impl_->volume.reset();
        return;
    }
    if (impl_->volume != nullptr) {
        impl_->volume->set_percent(impl_->monitor_source_name(), 100.0);
        volume_percent_ = 100.0;
    }
    impl_->stopped = true;
    const PulseCtl pulse;
    // Never restore the captured default here: the user may have switched
    // outputs while ducking was active. Only the streams we moved are restored.

    try {
        for (const auto& input : pulse.sink_inputs_on_sink(impl_->virtual_sink_index)) {
            impl_->moved_inputs.emplace(input.index, impl_->real_sink_index);
        }
    } catch (const std::exception& error) {
        std::cerr << "pipeduck: could not inspect routed streams: " << error.what() << "\n";
    }

    for (const auto& [input_index, original_sink] : impl_->moved_inputs) {
        try {
            pulse.move_sink_input_to_index(input_index, original_sink);
        } catch (const std::exception&) {
            try {
                pulse.move_sink_input(input_index, impl_->real_sink_name);
            } catch (const std::exception& error) {
                std::cerr << "pipeduck: could not restore sink-input #" << input_index << ": "
                          << error.what() << "\n";
            }
        }
    }

    bool empty = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        try {
            const auto remaining = pulse.sink_inputs_on_sink(impl_->virtual_sink_index);
            if (remaining.empty()) {
                empty = true;
                break;
            }
            for (const auto& input : remaining) {
                impl_->moved_inputs.emplace(input.index, impl_->real_sink_index);
                try {
                    pulse.move_sink_input(input.index, impl_->real_sink_name);
                } catch (const std::exception&) {
                }
            }
        } catch (const std::exception&) {
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!empty) {
        std::cerr << "pipeduck: warning: virtual sink " << impl_->virtual_sink_name
                  << " still has streams; leaving it alive to avoid terminating audio\n";
        impl_->virtual_sink->abandon();
        impl_->volume.reset();
        return;
    }

    for (const auto& link : impl_->monitor_links) {
        try {
            unlink_port(link);
        } catch (const std::exception& error) {
            std::cerr << "pipeduck: could not remove monitor link: " << error.what() << "\n";
        }
    }
    impl_->monitor_links.clear();
    try {
        impl_->virtual_sink->destroy();
    } catch (const std::exception& error) {
        std::cerr << "pipeduck: could not destroy virtual sink: " << error.what() << "\n";
    }
    impl_->volume.reset();
}

RoutingEngine::RoutingEngine(std::optional<ConfiguredSource> voice_source,
                             std::optional<ConfiguredMic> mic_source)
    : voice_source_(std::move(voice_source)), mic_source_(std::move(mic_source)) {}

RouteSession RoutingEngine::start(const RoutingOptions& options) {
    if (voice_source_.has_value() && !voice_source_->has_match_fields()) {
        throw std::runtime_error("configured voice source contains no stable match fields");
    }

    const PulseCtl pulse;
    const std::string original_default = pulse.default_sink_name();

    // Always route against the current default output. The voice-call stream is
    // excluded from ducking, but it no longer pins routing to a stale device.
    const std::optional<Sink> real_sink = pulse.sink_by_name(original_default);
    if (!real_sink.has_value()) {
        throw std::runtime_error("default sink " + original_default + " not found");
    }

    RouteSession session;
    RouteSession::Impl& impl = *session.impl_;
    impl.real_sink_name = real_sink->name;
    impl.real_sink_index = real_sink->index;
    impl.volume = std::make_unique<VolumeController>();
    impl.virtual_sink = VirtualSink::create(options.virtual_sink_prefix);
    impl.virtual_sink_name = impl.virtual_sink->name();
    impl.virtual_sink_index =
        pulse.wait_for_sink(impl.virtual_sink_name, options.sink_appear_timeout).index;

    try {
        pulse.set_default_sink(original_default);
    } catch (const std::exception& error) {
        throw std::runtime_error("failed to restore the default sink: " +
                                 std::string(error.what()));
    }

    const auto deadline = std::chrono::steady_clock::now() + kPortAppearTimeout;
    for (;;) {
        const std::string outputs = run_command_checked("pw-link", {"-o"}, kLinkTimeout);
        const std::string inputs = run_command_checked("pw-link", {"-i"}, kLinkTimeout);
        try {
            const auto links = monitor_links_for_ports(outputs, inputs, impl.virtual_sink_name,
                                                       impl.real_sink_name);
            for (const auto& link : links) {
                create_link(link);
                impl.monitor_links.push_back(link);
            }
            break;
        } catch (const std::exception& error) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(
                    "PipeWire ports for the virtual sink did not become available: " +
                    std::string(error.what()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    session.set_percent(100.0);
    session.route_existing_outputs(voice_source_);
    return session;
}

} // namespace pipeduck
