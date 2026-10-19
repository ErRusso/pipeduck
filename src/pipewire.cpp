#include "pipeduck/pipewire.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <pipewire/version.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "pipeduck/common.hpp"

namespace pipeduck {
namespace {

void ensure_pipewire_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] { pw_init(nullptr, nullptr); });
}

class LoopRuntime {
public:
    explicit LoopRuntime(const std::string& name) {
        ensure_pipewire_initialized();
        loop_ = pw_thread_loop_new(name.c_str(), nullptr);
        if (loop_ == nullptr) {
            throw std::runtime_error("failed to create PipeWire thread loop");
        }
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (context_ == nullptr) {
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            throw std::runtime_error("failed to create PipeWire context");
        }
        if (pw_thread_loop_start(loop_) < 0) {
            pw_context_destroy(context_);
            context_ = nullptr;
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            throw std::runtime_error("failed to start PipeWire thread loop");
        }
        pw_thread_loop_lock(loop_);
        core_ = pw_context_connect(context_, nullptr, 0);
        pw_thread_loop_unlock(loop_);
        if (core_ == nullptr) {
            pw_thread_loop_stop(loop_);
            pw_context_destroy(context_);
            context_ = nullptr;
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            throw std::runtime_error("failed to connect to PipeWire");
        }
    }

    ~LoopRuntime() {
        if (loop_ != nullptr) {
            pw_thread_loop_stop(loop_);
        }
        if (core_ != nullptr) {
            pw_core_disconnect(core_);
            core_ = nullptr;
        }
        if (context_ != nullptr) {
            pw_context_destroy(context_);
            context_ = nullptr;
        }
        if (loop_ != nullptr) {
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
    }

    LoopRuntime(const LoopRuntime&) = delete;
    LoopRuntime& operator=(const LoopRuntime&) = delete;

    pw_thread_loop* loop() const {
        return loop_;
    }
    pw_core* core() const {
        return core_;
    }

    // Must be called with the thread loop locked.
    bool sync_locked() {
        struct SyncData {
            pw_thread_loop* loop;
            int seq = 0;
            bool done = false;
            bool error = false;
        } data{loop_};

        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = [](void* userdata, uint32_t id, int seq) {
            auto* sync = static_cast<SyncData*>(userdata);
            if (id == PW_ID_CORE && seq == sync->seq) {
                sync->done = true;
                pw_thread_loop_signal(sync->loop, false);
            }
        };
        events.error = [](void* userdata, uint32_t, int, int, const char*) {
            auto* sync = static_cast<SyncData*>(userdata);
            sync->error = true;
            pw_thread_loop_signal(sync->loop, false);
        };

        spa_hook listener{};
        pw_core_add_listener(core_, &listener, &events, &data);
        data.seq = pw_core_sync(core_, PW_ID_CORE, 0);
        while (!data.done && !data.error) {
            if (pw_thread_loop_timed_wait(loop_, 2) != 0) {
                break;
            }
        }
        spa_hook_remove(&listener);
        return data.done && !data.error;
    }

private:
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
};

void destroy_created_object(pw_core* core, void* object) {
    if (object == nullptr) {
        return;
    }
#if PW_MAJOR > 1 || (PW_MAJOR == 1 && PW_MINOR >= 6)
    pw_core_destroy(core, object);
#else
    pw_proxy_destroy(static_cast<pw_proxy*>(object));
#endif
}

} // namespace

struct VirtualSink::Impl {
    std::unique_ptr<LoopRuntime> runtime;
    void* object = nullptr;
    bool abandoned = false;

    ~Impl() {
        if (runtime == nullptr || abandoned) {
            // Intentionally leak the connection when abandoning the sink so the
            // server-side node stays alive for as long as this process runs.
            (void)runtime.release();
            return;
        }
        if (object != nullptr) {
            pw_thread_loop_lock(runtime->loop());
            destroy_created_object(runtime->core(), object);
            object = nullptr;
            pw_thread_loop_unlock(runtime->loop());
        }
    }
};

VirtualSink::VirtualSink() = default;

VirtualSink::~VirtualSink() = default;

std::unique_ptr<VirtualSink> VirtualSink::create(const std::string& prefix) {
    auto sink = std::unique_ptr<VirtualSink>(new VirtualSink());
    sink->impl_ = std::make_unique<Impl>();
    sink->name_ = unique_instance_name(prefix);
    sink->impl_->runtime = std::make_unique<LoopRuntime>("pipeduck-sink");

    pw_properties* properties =
        pw_properties_new("factory.name", "support.null-audio-sink", "node.name",
                          sink->name_.c_str(), "node.description", "PipeDuck virtual ducking sink",
                          "media.class", "Audio/Sink", "audio.position", "FL,FR", "node.passive",
                          "true", "priority.driver", "1", "priority.session", "1", nullptr);
    if (properties == nullptr) {
        throw std::runtime_error("failed to allocate PipeWire properties");
    }

    pw_thread_loop_lock(sink->impl_->runtime->loop());
    void* object =
        pw_core_create_object(sink->impl_->runtime->core(), "adapter", PW_TYPE_INTERFACE_Node,
                              PW_VERSION_NODE, &properties->dict, 0);
    pw_properties_free(properties);
    const bool synchronized = object != nullptr && sink->impl_->runtime->sync_locked();
    pw_thread_loop_unlock(sink->impl_->runtime->loop());

    if (!synchronized) {
        if (object != nullptr) {
            pw_thread_loop_lock(sink->impl_->runtime->loop());
            destroy_created_object(sink->impl_->runtime->core(), object);
            pw_thread_loop_unlock(sink->impl_->runtime->loop());
        }
        throw std::runtime_error("failed to create the virtual PipeWire sink");
    }
    sink->impl_->object = object;
    return sink;
}

void VirtualSink::destroy() {
    if (impl_ == nullptr || impl_->object == nullptr || destroyed_) {
        return;
    }
    pw_thread_loop_lock(impl_->runtime->loop());
    destroy_created_object(impl_->runtime->core(), impl_->object);
    impl_->object = nullptr;
    impl_->runtime->sync_locked();
    pw_thread_loop_unlock(impl_->runtime->loop());
    destroyed_ = true;
}

struct VoiceActivityMonitor::Impl {
    CaptureTarget target;
    std::unique_ptr<LoopRuntime> runtime;
    pw_stream* stream = nullptr;
    pw_stream_events events{};
    spa_hook listener{};
    spa_audio_format format = SPA_AUDIO_FORMAT_UNKNOWN;
    uint32_t channels = 0;
    std::atomic<double> energy{0.0};
    std::atomic<uint64_t> frames{0};
    std::atomic<bool> audio_seen{false};
    std::atomic<bool> stopped{false};
    double smoothed_energy = 0.0;

    static void on_param_changed(void* data, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<Impl*>(data);
        if (id != SPA_PARAM_Format || param == nullptr) {
            return;
        }
        uint32_t media_type = 0;
        uint32_t media_subtype = 0;
        if (spa_format_parse(param, &media_type, &media_subtype) < 0) {
            return;
        }
        if (media_type != SPA_MEDIA_TYPE_audio || media_subtype != SPA_MEDIA_SUBTYPE_raw) {
            return;
        }
        spa_audio_info_raw info{};
        if (spa_format_audio_raw_parse(param, &info) < 0) {
            return;
        }
        self->format = info.format;
        self->channels = info.channels;
    }

    static void on_process(void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || self->stream == nullptr) {
            return;
        }
        pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
        if (buffer == nullptr) {
            return;
        }
        struct spa_buffer* audio_buffer = buffer->buffer;
        if (audio_buffer == nullptr || audio_buffer->n_datas == 0) {
            pw_stream_queue_buffer(self->stream, buffer);
            return;
        }
        struct spa_data* audio_data = &audio_buffer->datas[0];
        if (audio_data->chunk == nullptr || audio_data->data == nullptr) {
            pw_stream_queue_buffer(self->stream, buffer);
            return;
        }
        const uint32_t offset = audio_data->chunk->offset;
        const uint32_t size = audio_data->chunk->size;
        if (size == 0 || offset >= audio_data->maxsize) {
            pw_stream_queue_buffer(self->stream, buffer);
            return;
        }
        const auto* base = static_cast<const uint8_t*>(audio_data->data);
        const std::size_t available =
            static_cast<std::size_t>(audio_data->maxsize) - static_cast<std::size_t>(offset);
        const std::size_t length = std::min<std::size_t>(size, available);

        double sum_squares = 0.0;
        std::size_t count = 0;
        const auto accumulate = [&](double sample) {
            sum_squares += sample * sample;
            ++count;
        };

        switch (self->format) {
        case SPA_AUDIO_FORMAT_F32_LE:
            for (std::size_t i = 0; i + 4 <= length; i += 4) {
                float value;
                std::memcpy(&value, base + offset + i, sizeof(value));
                accumulate(value);
            }
            break;
        case SPA_AUDIO_FORMAT_F32_BE: {
            for (std::size_t i = 0; i + 4 <= length; i += 4) {
                uint32_t bits = (static_cast<uint32_t>(base[offset + i]) << 24) |
                                (static_cast<uint32_t>(base[offset + i + 1]) << 16) |
                                (static_cast<uint32_t>(base[offset + i + 2]) << 8) |
                                static_cast<uint32_t>(base[offset + i + 3]);
                float value;
                std::memcpy(&value, &bits, sizeof(value));
                accumulate(value);
            }
            break;
        }
        case SPA_AUDIO_FORMAT_S16_LE:
        case SPA_AUDIO_FORMAT_S16_BE: {
            for (std::size_t i = 0; i + 2 <= length; i += 2) {
                uint16_t raw = 0;
                if (self->format == SPA_AUDIO_FORMAT_S16_LE) {
                    raw = static_cast<uint16_t>(base[offset + i]) |
                          (static_cast<uint16_t>(base[offset + i + 1]) << 8);
                } else {
                    raw = (static_cast<uint16_t>(base[offset + i]) << 8) |
                          static_cast<uint16_t>(base[offset + i + 1]);
                }
                int16_t value = static_cast<int16_t>(raw);
                accumulate(static_cast<double>(value) / 32767.0);
            }
            break;
        }
        case SPA_AUDIO_FORMAT_S32_LE:
        case SPA_AUDIO_FORMAT_S32_BE: {
            for (std::size_t i = 0; i + 4 <= length; i += 4) {
                uint32_t raw = 0;
                if (self->format == SPA_AUDIO_FORMAT_S32_LE) {
                    raw = static_cast<uint32_t>(base[offset + i]) |
                          (static_cast<uint32_t>(base[offset + i + 1]) << 8) |
                          (static_cast<uint32_t>(base[offset + i + 2]) << 16) |
                          (static_cast<uint32_t>(base[offset + i + 3]) << 24);
                } else {
                    raw = (static_cast<uint32_t>(base[offset + i]) << 24) |
                          (static_cast<uint32_t>(base[offset + i + 1]) << 16) |
                          (static_cast<uint32_t>(base[offset + i + 2]) << 8) |
                          static_cast<uint32_t>(base[offset + i + 3]);
                }
                int32_t value = static_cast<int32_t>(raw);
                accumulate(static_cast<double>(value) / 2147483647.0);
            }
            break;
        }
        default:
            break;
        }

        if (count > 0) {
            const double rms = std::sqrt(sum_squares / static_cast<double>(count));
            // Light smoothing removes inter-buffer jitter without hiding speech.
            self->smoothed_energy = self->smoothed_energy * 0.45 + rms * 0.55;
            self->energy.store(self->smoothed_energy, std::memory_order_relaxed);
            self->audio_seen.store(true, std::memory_order_relaxed);
            self->frames.fetch_add(1, std::memory_order_relaxed);
        }
        pw_stream_queue_buffer(self->stream, buffer);
    }

    ~Impl() {
        if (stream != nullptr && runtime != nullptr) {
            pw_thread_loop_lock(runtime->loop());
            pw_stream_disconnect(stream);
            pw_stream_destroy(stream);
            stream = nullptr;
            pw_thread_loop_unlock(runtime->loop());
        }
    }
};

VoiceActivityMonitor::VoiceActivityMonitor() = default;

VoiceActivityMonitor::~VoiceActivityMonitor() = default;

std::unique_ptr<VoiceActivityMonitor> VoiceActivityMonitor::start(const CaptureTarget& target) {
    auto monitor = std::unique_ptr<VoiceActivityMonitor>(new VoiceActivityMonitor());
    monitor->impl_ = std::make_unique<Impl>();
    Impl& impl = *monitor->impl_;
    impl.target = target;
    impl.runtime = std::make_unique<LoopRuntime>("pipeduck-capture");

    pw_properties* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
        "Communication", PW_KEY_MEDIA_CLASS, "Stream/Input/Audio", "node.name",
        "pipeduck-voice-capture", "node.description", "PipeDuck voice activity capture", nullptr);
    if (properties == nullptr) {
        throw std::runtime_error("failed to allocate PipeWire stream properties");
    }
    if (target.object_serial.has_value() && !target.object_serial->empty()) {
        pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target.object_serial->c_str());
    } else if (target.node_name.has_value() && !target.node_name->empty()) {
        pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target.node_name->c_str());
    }
    if (target.sink_monitor) {
        pw_properties_set(properties, PW_KEY_STREAM_CAPTURE_SINK, "true");
    }

    pw_thread_loop_lock(impl.runtime->loop());
    impl.stream = pw_stream_new(impl.runtime->core(), "pipeduck voice capture", properties);
    if (impl.stream != nullptr) {
        impl.events.version = PW_VERSION_STREAM_EVENTS;
        impl.events.param_changed = &Impl::on_param_changed;
        impl.events.process = &Impl::on_process;
        pw_stream_add_listener(impl.stream, &impl.listener, &impl.events, &impl);

        uint8_t buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32_LE;
        const spa_pod* params[1] = {
            spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
        const auto flags = static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
        const int result =
            pw_stream_connect(impl.stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1);
        if (result < 0) {
            pw_stream_destroy(impl.stream);
            impl.stream = nullptr;
        }
    }
    pw_thread_loop_unlock(impl.runtime->loop());

    if (impl.stream == nullptr) {
        throw std::runtime_error("failed to connect the PipeWire voice capture stream");
    }
    return monitor;
}

double VoiceActivityMonitor::energy() const {
    return impl_ != nullptr ? impl_->energy.load(std::memory_order_relaxed) : 0.0;
}

uint64_t VoiceActivityMonitor::frames() const {
    return impl_ != nullptr ? impl_->frames.load(std::memory_order_relaxed) : 0;
}

bool VoiceActivityMonitor::audio_seen() const {
    return impl_ != nullptr && impl_->audio_seen.load(std::memory_order_relaxed);
}

void VoiceActivityMonitor::stop() {
    if (impl_ == nullptr || impl_->stopped.exchange(true)) {
        return;
    }
    if (impl_->stream != nullptr) {
        pw_thread_loop_lock(impl_->runtime->loop());
        pw_stream_disconnect(impl_->stream);
        pw_stream_destroy(impl_->stream);
        impl_->stream = nullptr;
        pw_thread_loop_unlock(impl_->runtime->loop());
    }
    impl_->runtime.reset();
}

} // namespace pipeduck
