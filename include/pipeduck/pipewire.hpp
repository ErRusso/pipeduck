#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace pipeduck {

struct CaptureTarget {
    std::string label;
    std::optional<std::string> node_name;
    std::optional<std::string> object_serial;
    // true: capture a sink monitor (remote call audio); false: capture a source (microphone).
    bool sink_monitor = true;
};

// A temporary null-audio sink used to route and attenuate non-call playback.
class VirtualSink {
public:
    static std::unique_ptr<VirtualSink> create(const std::string& prefix);
    ~VirtualSink();
    VirtualSink(const VirtualSink&) = delete;
    VirtualSink& operator=(const VirtualSink&) = delete;

    const std::string& name() const {
        return name_;
    }
    void destroy();
    // Leave the server-side node alive to avoid terminating playback streams.
    void abandon();

private:
    VirtualSink();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string name_;
    bool destroyed_ = false;
    bool abandoned_ = false;
};

// Captures one audio stream in a dedicated PipeWire thread and exposes its RMS
// energy. Works both for sink monitors and for microphone sources.
class VoiceActivityMonitor {
public:
    static std::unique_ptr<VoiceActivityMonitor> start(const CaptureTarget& target);
    ~VoiceActivityMonitor();
    VoiceActivityMonitor(const VoiceActivityMonitor&) = delete;
    VoiceActivityMonitor& operator=(const VoiceActivityMonitor&) = delete;

    double energy() const;
    uint64_t frames() const;
    bool audio_seen() const;
    void stop();

private:
    VoiceActivityMonitor();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipeduck
