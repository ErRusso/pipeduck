#include "pipeduck/pulse.hpp"

#include <json-c/json.h>
#include <pulse/pulseaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace pipeduck {
namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

JsonPtr parse_json(const std::string& raw, const char* what) {
    json_tokener* tokener = json_tokener_new();
    if (tokener == nullptr) {
        throw std::runtime_error("failed to allocate JSON parser");
    }
    json_object* root = json_tokener_parse_ex(tokener, raw.c_str(), static_cast<int>(raw.size()));
    const auto error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    if (root == nullptr || error != json_tokener_success) {
        if (root != nullptr) {
            json_object_put(root);
        }
        throw std::runtime_error(std::string("failed to parse ") + what + " JSON");
    }
    return JsonPtr(root, json_object_put);
}

std::string string_field(json_object* object, const char* key) {
    json_object* value = nullptr;
    if (json_object_object_get_ex(object, key, &value) && value != nullptr) {
        const char* text = json_object_get_string(value);
        if (text != nullptr) {
            return text;
        }
    }
    return {};
}

void collect_string_properties(json_object* object, StringMap& properties) {
    if (object == nullptr || !json_object_is_type(object, json_type_object)) {
        return;
    }
    json_object_object_foreach(object, key, value) {
        if (value == nullptr) {
            continue;
        }
        const char* text = json_object_get_string(value);
        if (text != nullptr) {
            properties[key] = text;
        }
    }
}

bool source_is_monitor(json_object* object, const std::string& name) {
    json_object* monitor_of = nullptr;
    if (json_object_object_get_ex(object, "monitor_of_sink", &monitor_of) &&
        monitor_of != nullptr) {
        return true;
    }
    return name.size() >= 8 && name.compare(name.size() - 8, 8, ".monitor") == 0;
}

} // namespace

std::vector<Sink> parse_sinks_json(const std::string& raw) {
    std::vector<Sink> sinks;
    if (trim(raw).empty()) {
        return sinks;
    }
    JsonPtr root = parse_json(raw, "sinks");
    if (!json_object_is_type(root.get(), json_type_array)) {
        throw std::runtime_error("unexpected pactl sinks JSON");
    }
    const std::size_t count = json_object_array_length(root.get());
    for (std::size_t i = 0; i < count; ++i) {
        json_object* item = json_object_array_get_idx(root.get(), static_cast<int>(i));
        if (item == nullptr) {
            continue;
        }
        Sink sink;
        json_object* index = nullptr;
        if (json_object_object_get_ex(item, "index", &index) && index != nullptr) {
            sink.index = static_cast<uint32_t>(json_object_get_int(index));
        }
        sink.name = string_field(item, "name");
        sink.description = string_field(item, "description");
        sinks.push_back(std::move(sink));
    }
    return sinks;
}

std::vector<SinkInput> parse_sink_inputs_json(const std::string& raw) {
    std::vector<SinkInput> inputs;
    if (trim(raw).empty()) {
        return inputs;
    }
    JsonPtr root = parse_json(raw, "sink-inputs");
    if (!json_object_is_type(root.get(), json_type_array)) {
        throw std::runtime_error("unexpected pactl sink-inputs JSON");
    }
    const std::size_t count = json_object_array_length(root.get());
    for (std::size_t i = 0; i < count; ++i) {
        json_object* item = json_object_array_get_idx(root.get(), static_cast<int>(i));
        if (item == nullptr) {
            continue;
        }
        SinkInput input;
        json_object* index = nullptr;
        if (json_object_object_get_ex(item, "index", &index) && index != nullptr) {
            input.index = static_cast<uint32_t>(json_object_get_int(index));
        }
        json_object* sink = nullptr;
        if (json_object_object_get_ex(item, "sink", &sink) && sink != nullptr) {
            input.sink = static_cast<uint32_t>(json_object_get_int(sink));
        }
        json_object* properties = nullptr;
        if (json_object_object_get_ex(item, "properties", &properties)) {
            collect_string_properties(properties, input.properties);
        }
        inputs.push_back(std::move(input));
    }
    return inputs;
}

std::vector<Source> parse_sources_json(const std::string& raw) {
    std::vector<Source> sources;
    if (trim(raw).empty()) {
        return sources;
    }
    JsonPtr root = parse_json(raw, "sources");
    if (!json_object_is_type(root.get(), json_type_array)) {
        throw std::runtime_error("unexpected pactl sources JSON");
    }
    const std::size_t count = json_object_array_length(root.get());
    for (std::size_t i = 0; i < count; ++i) {
        json_object* item = json_object_array_get_idx(root.get(), static_cast<int>(i));
        if (item == nullptr) {
            continue;
        }
        Source source;
        json_object* index = nullptr;
        if (json_object_object_get_ex(item, "index", &index) && index != nullptr) {
            source.index = static_cast<uint32_t>(json_object_get_int(index));
        }
        source.name = string_field(item, "name");
        source.description = string_field(item, "description");
        source.is_monitor = source_is_monitor(item, source.name);
        json_object* properties = nullptr;
        if (json_object_object_get_ex(item, "properties", &properties)) {
            collect_string_properties(properties, source.properties);
        }
        sources.push_back(std::move(source));
    }
    return sources;
}

struct PulseCache::Impl {
    enum class Facility { Server, Sinks, Sources, SinkInputs };

    struct ListRequest {
        Impl* self = nullptr;
        Facility facility = Facility::Sinks;
        uint64_t serial = 0;
        std::vector<Sink> sinks;
        std::vector<Source> sources;
        std::vector<SinkInput> sink_inputs;
    };

    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> revision{0};

    mutable std::mutex mutex;
    std::vector<Sink> sink_data;
    std::vector<Source> source_data;
    std::vector<SinkInput> sink_input_data;
    std::string default_sink;
    std::string default_source;

    // Accessed only while the threaded mainloop is locked.
    int pending = 0;
    uint64_t sink_serial = 0;
    uint64_t source_serial = 0;
    uint64_t sink_input_serial = 0;

    static void state_callback(pa_context* context, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        switch (pa_context_get_state(context)) {
        case PA_CONTEXT_READY:
            self->ready.store(true);
            self->failed.store(false);
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            self->ready.store(false);
            self->failed.store(true);
            break;
        default:
            break;
        }
        pa_threaded_mainloop_signal(self->mainloop, 0);
    }

    static void finish_pending(Impl* self) {
        if (self->pending > 0) {
            --self->pending;
        }
        if (self->pending == 0) {
            pa_threaded_mainloop_signal(self->mainloop, 0);
        }
    }

    static void apply_properties(pa_proplist* properties, StringMap& out) {
        if (properties == nullptr) {
            return;
        }
        static const char* const keys[] = {
            "application.name",
            "application.process.binary",
            "application.process.id",
            "application.id",
            "media.name",
            "media.role",
            "media.class",
            "node.name",
            "node.description",
            "device.description",
            "object.serial",
        };
        for (const char* key : keys) {
            const char* value = pa_proplist_gets(properties, key);
            if (value != nullptr) {
                out[key] = value;
            }
        }
    }

    void finish_list(ListRequest* request) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            switch (request->facility) {
            case Facility::Sinks:
                if (request->serial == sink_serial) {
                    sink_data = std::move(request->sinks);
                }
                break;
            case Facility::Sources:
                if (request->serial == source_serial) {
                    source_data = std::move(request->sources);
                }
                break;
            case Facility::SinkInputs:
                if (request->serial == sink_input_serial) {
                    sink_input_data = std::move(request->sink_inputs);
                }
                break;
            case Facility::Server:
                break;
            }
            revision.fetch_add(1);
        }
        delete request;
        finish_pending(this);
    }

    static void sink_info_callback(pa_context*, const pa_sink_info* info, int eof, void* userdata) {
        auto* request = static_cast<ListRequest*>(userdata);
        if (eof) {
            request->self->finish_list(request);
            return;
        }
        if (info == nullptr) {
            return;
        }
        Sink sink;
        sink.index = info->index;
        sink.name = info->name != nullptr ? info->name : "";
        sink.description = info->description != nullptr ? info->description : "";
        request->sinks.push_back(std::move(sink));
    }

    static void source_info_callback(pa_context*, const pa_source_info* info, int eof,
                                     void* userdata) {
        auto* request = static_cast<ListRequest*>(userdata);
        if (eof) {
            request->self->finish_list(request);
            return;
        }
        if (info == nullptr) {
            return;
        }
        Source source;
        source.index = info->index;
        source.name = info->name != nullptr ? info->name : "";
        source.description = info->description != nullptr ? info->description : "";
        source.is_monitor = info->monitor_of_sink != PA_INVALID_INDEX;
        apply_properties(info->proplist, source.properties);
        request->sources.push_back(std::move(source));
    }

    static void sink_input_info_callback(pa_context*, const pa_sink_input_info* info, int eof,
                                         void* userdata) {
        auto* request = static_cast<ListRequest*>(userdata);
        if (eof) {
            request->self->finish_list(request);
            return;
        }
        if (info == nullptr) {
            return;
        }
        SinkInput input;
        input.index = info->index;
        input.sink = info->sink;
        apply_properties(info->proplist, input.properties);
        request->sink_inputs.push_back(std::move(input));
    }

    static void server_info_callback(pa_context*, const pa_server_info* info, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (info != nullptr) {
            std::lock_guard<std::mutex> lock(self->mutex);
            const std::string sink =
                info->default_sink_name != nullptr ? info->default_sink_name : "";
            const std::string source =
                info->default_source_name != nullptr ? info->default_source_name : "";
            if (sink != self->default_sink || source != self->default_source) {
                self->default_sink = sink;
                self->default_source = source;
                self->revision.fetch_add(1);
            }
        }
        finish_pending(self);
    }

    static void subscribe_callback(pa_context*, pa_subscription_event_type_t type, uint32_t,
                                   void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (!self->ready.load()) {
            return;
        }
        switch (type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
        case PA_SUBSCRIPTION_EVENT_SERVER:
            self->request_server();
            break;
        case PA_SUBSCRIPTION_EVENT_SINK:
            self->request_sinks();
            break;
        case PA_SUBSCRIPTION_EVENT_SOURCE:
            self->request_sources();
            break;
        case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
            self->request_sink_inputs();
            break;
        default:
            break;
        }
    }

    void request_server() {
        ++pending;
        pa_operation* operation =
            pa_context_get_server_info(context, &Impl::server_info_callback, this);
        if (operation == nullptr) {
            finish_pending(this);
            return;
        }
        pa_operation_unref(operation);
    }

    void request_sinks() {
        const uint64_t serial = ++sink_serial;
        auto* request = new ListRequest{this, Facility::Sinks, serial, {}, {}, {}};
        ++pending;
        pa_operation* operation =
            pa_context_get_sink_info_list(context, &Impl::sink_info_callback, request);
        if (operation == nullptr) {
            delete request;
            finish_pending(this);
            return;
        }
        pa_operation_unref(operation);
    }

    void request_sources() {
        const uint64_t serial = ++source_serial;
        auto* request = new ListRequest{this, Facility::Sources, serial, {}, {}, {}};
        ++pending;
        pa_operation* operation =
            pa_context_get_source_info_list(context, &Impl::source_info_callback, request);
        if (operation == nullptr) {
            delete request;
            finish_pending(this);
            return;
        }
        pa_operation_unref(operation);
    }

    void request_sink_inputs() {
        const uint64_t serial = ++sink_input_serial;
        auto* request = new ListRequest{this, Facility::SinkInputs, serial, {}, {}, {}};
        ++pending;
        pa_operation* operation =
            pa_context_get_sink_input_info_list(context, &Impl::sink_input_info_callback, request);
        if (operation == nullptr) {
            delete request;
            finish_pending(this);
            return;
        }
        pa_operation_unref(operation);
    }

    Impl() {
        mainloop = pa_threaded_mainloop_new();
        if (mainloop == nullptr) {
            failed.store(true);
            return;
        }
        if (pa_threaded_mainloop_start(mainloop) < 0) {
            pa_threaded_mainloop_free(mainloop);
            mainloop = nullptr;
            failed.store(true);
            return;
        }
        pa_threaded_mainloop_lock(mainloop);
        context = pa_context_new(pa_threaded_mainloop_get_api(mainloop), "pipeduck-cache");
        if (context == nullptr) {
            pa_threaded_mainloop_unlock(mainloop);
            failed.store(true);
            return;
        }
        pa_context_set_state_callback(context, &Impl::state_callback, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            failed.store(true);
        }
        while (!ready.load() && !failed.load()) {
            pa_threaded_mainloop_wait(mainloop);
        }
        if (ready.load() && !failed.load()) {
            pa_context_set_subscribe_callback(context, &Impl::subscribe_callback, this);
            const auto mask = static_cast<pa_subscription_mask_t>(
                PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_SINK |
                PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK_INPUT);
            if (pa_operation* operation = pa_context_subscribe(context, mask, nullptr, nullptr)) {
                pa_operation_unref(operation);
            } else {
                failed.store(true);
            }
            if (!failed.load()) {
                request_server();
                request_sinks();
                request_sources();
                request_sink_inputs();
                while (pending > 0 && !failed.load()) {
                    pa_threaded_mainloop_wait(mainloop);
                }
            }
        }
        pa_threaded_mainloop_unlock(mainloop);
        if (failed.load()) {
            ready.store(false);
        }
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
        return mainloop != nullptr && context != nullptr && ready.load() && !failed.load();
    }
};

PulseCache::PulseCache() : impl_(std::make_unique<Impl>()) {}

PulseCache::~PulseCache() = default;

PulseCache& PulseCache::shared() {
    static PulseCache cache;
    return cache;
}

bool PulseCache::available() const {
    return impl_ != nullptr && impl_->available();
}

uint64_t PulseCache::revision() const {
    return impl_ != nullptr ? impl_->revision.load() : 0;
}

std::string PulseCache::default_sink_name() const {
    if (!available()) {
        throw std::runtime_error("PulseAudio event cache is unavailable");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->default_sink;
}

std::string PulseCache::default_source_name() const {
    if (!available()) {
        throw std::runtime_error("PulseAudio event cache is unavailable");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->default_source;
}

std::vector<Sink> PulseCache::sinks() const {
    if (!available()) {
        throw std::runtime_error("PulseAudio event cache is unavailable");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sink_data;
}

std::optional<Sink> PulseCache::sink_by_name(const std::string& name) const {
    for (const auto& sink : sinks()) {
        if (sink.name == name) {
            return sink;
        }
    }
    return std::nullopt;
}

std::optional<Sink> PulseCache::sink_by_index(uint32_t index) const {
    for (const auto& sink : sinks()) {
        if (sink.index == index) {
            return sink;
        }
    }
    return std::nullopt;
}

std::vector<SinkInput> PulseCache::sink_inputs() const {
    if (!available()) {
        throw std::runtime_error("PulseAudio event cache is unavailable");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sink_input_data;
}

std::vector<SinkInput> PulseCache::sink_inputs_on_sink(uint32_t sink_index) const {
    std::vector<SinkInput> result;
    for (auto& input : sink_inputs()) {
        if (input.sink == sink_index) {
            result.push_back(std::move(input));
        }
    }
    return result;
}

std::vector<Source> PulseCache::sources() const {
    if (!available()) {
        throw std::runtime_error("PulseAudio event cache is unavailable");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->source_data;
}

std::optional<Source> PulseCache::source_by_name(const std::string& name) const {
    for (const auto& source : sources()) {
        if (source.name == name) {
            return source;
        }
    }
    return std::nullopt;
}

std::optional<Source> PulseCache::source_by_index(uint32_t index) const {
    for (const auto& source : sources()) {
        if (source.index == index) {
            return source;
        }
    }
    return std::nullopt;
}

bool PulseCache::move_sink_input(uint32_t input_index, const std::string& sink_name) const {
    if (!available()) {
        return false;
    }
    uint32_t sink_index = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found =
            std::find_if(impl_->sink_data.begin(), impl_->sink_data.end(),
                         [&sink_name](const Sink& sink) { return sink.name == sink_name; });
        if (found == impl_->sink_data.end()) {
            return false;
        }
        sink_index = found->index;
    }
    return move_sink_input_to_index(input_index, sink_index);
}

bool PulseCache::move_sink_input_to_index(uint32_t input_index, uint32_t sink_index) const {
    if (!available()) {
        return false;
    }
    pa_threaded_mainloop_lock(impl_->mainloop);
    if (!impl_->ready.load()) {
        pa_threaded_mainloop_unlock(impl_->mainloop);
        return false;
    }
    pa_operation* operation = pa_context_move_sink_input_by_index(impl_->context, input_index,
                                                                  sink_index, nullptr, nullptr);
    const bool ok = operation != nullptr;
    if (operation != nullptr) {
        pa_operation_unref(operation);
    }
    pa_threaded_mainloop_unlock(impl_->mainloop);
    return ok;
}

bool PulseCache::set_default_sink(const std::string& sink_name) const {
    if (!available()) {
        return false;
    }
    pa_threaded_mainloop_lock(impl_->mainloop);
    if (!impl_->ready.load()) {
        pa_threaded_mainloop_unlock(impl_->mainloop);
        return false;
    }
    pa_operation* operation =
        pa_context_set_default_sink(impl_->context, sink_name.c_str(), nullptr, nullptr);
    const bool ok = operation != nullptr;
    if (operation != nullptr) {
        pa_operation_unref(operation);
    }
    pa_threaded_mainloop_unlock(impl_->mainloop);
    return ok;
}

bool PulseCache::set_source_volume_percent(const std::string& source_name, double percent) const {
    if (!available()) {
        return false;
    }
    const double clamped = std::clamp(percent, 0.0, 150.0);
    pa_volume_t volume = static_cast<pa_volume_t>(std::llround(clamped / 100.0 * PA_VOLUME_NORM));
    pa_cvolume channel_volume;
    pa_cvolume_set(&channel_volume, 1, volume);
    pa_threaded_mainloop_lock(impl_->mainloop);
    if (!impl_->ready.load()) {
        pa_threaded_mainloop_unlock(impl_->mainloop);
        return false;
    }
    pa_operation* operation = pa_context_set_source_volume_by_name(
        impl_->context, source_name.c_str(), &channel_volume, nullptr, nullptr);
    const bool ok = operation != nullptr;
    if (operation != nullptr) {
        pa_operation_unref(operation);
    }
    pa_threaded_mainloop_unlock(impl_->mainloop);
    return ok;
}

void PulseCtl::require_fallback() const {
    if (!allow_fallback_) {
        throw std::runtime_error("PulseAudio backend is unavailable");
    }
}

std::string PulseCtl::default_sink_name() const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available()) {
        try {
            const std::string name = cache.default_sink_name();
            if (!name.empty()) {
                return name;
            }
        } catch (const std::exception&) {
        }
    }
    require_fallback();
    return trim(run_command_checked("pactl", {"get-default-sink"}, kTimeout));
}

std::string PulseCtl::default_source_name() const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available()) {
        try {
            const std::string name = cache.default_source_name();
            if (!name.empty()) {
                return name;
            }
        } catch (const std::exception&) {
        }
    }
    require_fallback();
    return trim(run_command_checked("pactl", {"get-default-source"}, kTimeout));
}

void PulseCtl::set_default_sink(const std::string& sink_name) const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available() && cache.set_default_sink(sink_name)) {
        return;
    }
    require_fallback();
    run_status_checked("pactl", {"set-default-sink", sink_name}, kTimeout);
}

std::vector<Sink> PulseCtl::sinks() const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available()) {
        try {
            return cache.sinks();
        } catch (const std::exception&) {
        }
    }
    require_fallback();
    return parse_sinks_json(
        run_command_checked("pactl", {"--format=json", "list", "sinks"}, kTimeout));
}

std::optional<Sink> PulseCtl::sink_by_name(const std::string& name) const {
    for (auto& sink : sinks()) {
        if (sink.name == name) {
            return sink;
        }
    }
    return std::nullopt;
}

std::optional<Sink> PulseCtl::sink_by_index(uint32_t index) const {
    for (auto& sink : sinks()) {
        if (sink.index == index) {
            return sink;
        }
    }
    return std::nullopt;
}

Sink PulseCtl::wait_for_sink(const std::string& name, std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (const auto sink = sink_by_name(name)) {
            return *sink;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("sink " + name + " did not appear in time");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

std::vector<SinkInput> PulseCtl::sink_inputs() const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available()) {
        try {
            return cache.sink_inputs();
        } catch (const std::exception&) {
        }
    }
    require_fallback();
    return parse_sink_inputs_json(
        run_command_checked("pactl", {"--format=json", "list", "sink-inputs"}, kTimeout));
}

std::vector<SinkInput> PulseCtl::sink_inputs_on_sink(uint32_t sink_index) const {
    std::vector<SinkInput> result;
    for (auto& input : sink_inputs()) {
        if (input.sink == sink_index) {
            result.push_back(std::move(input));
        }
    }
    return result;
}

std::vector<Source> PulseCtl::sources() const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available()) {
        try {
            return cache.sources();
        } catch (const std::exception&) {
        }
    }
    require_fallback();
    return parse_sources_json(
        run_command_checked("pactl", {"--format=json", "list", "sources"}, kTimeout));
}

std::optional<Source> PulseCtl::source_by_name(const std::string& name) const {
    for (auto& source : sources()) {
        if (source.name == name) {
            return source;
        }
    }
    return std::nullopt;
}

std::optional<Source> PulseCtl::source_by_index(uint32_t index) const {
    for (auto& source : sources()) {
        if (source.index == index) {
            return source;
        }
    }
    return std::nullopt;
}

void PulseCtl::move_sink_input(uint32_t input_index, const std::string& sink_name) const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available() && cache.move_sink_input(input_index, sink_name)) {
        return;
    }
    require_fallback();
    run_status_checked("pactl", {"move-sink-input", std::to_string(input_index), sink_name},
                       kTimeout);
}

void PulseCtl::move_sink_input_to_index(uint32_t input_index, uint32_t sink_index) const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available() && cache.move_sink_input_to_index(input_index, sink_index)) {
        return;
    }
    require_fallback();
    run_status_checked("pactl",
                       {"move-sink-input", std::to_string(input_index), std::to_string(sink_index)},
                       kTimeout);
}

void PulseCtl::set_source_volume_percent(const std::string& source_name, double percent) const {
    PulseCache& cache = PulseCache::shared();
    if (cache.available() && cache.set_source_volume_percent(source_name, percent)) {
        return;
    }
    const double clamped = std::clamp(percent, 0.0, 150.0);
    // PulseAudio's raw volume scale: 65536 == 100%.
    const int raw = static_cast<int>(std::lround(clamped / 100.0 * 65536.0));
    require_fallback();
    run_status_checked("pactl", {"set-source-volume", source_name, std::to_string(raw)}, kTimeout);
}

uint64_t PulseCtl::cache_revision() {
    PulseCache& cache = PulseCache::shared();
    return cache.available() ? cache.revision() : 0;
}

} // namespace pipeduck
