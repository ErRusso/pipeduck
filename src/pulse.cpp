#include "pipeduck/pulse.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
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

std::string PulseCtl::default_sink_name() const {
    return trim(run_command_checked("pactl", {"get-default-sink"}, kTimeout));
}

std::string PulseCtl::default_source_name() const {
    return trim(run_command_checked("pactl", {"get-default-source"}, kTimeout));
}

void PulseCtl::set_default_sink(const std::string& sink_name) const {
    run_status_checked("pactl", {"set-default-sink", sink_name}, kTimeout);
}

std::vector<Sink> PulseCtl::sinks() const {
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
    run_status_checked("pactl", {"move-sink-input", std::to_string(input_index), sink_name},
                       kTimeout);
}

void PulseCtl::move_sink_input_to_index(uint32_t input_index, uint32_t sink_index) const {
    run_status_checked("pactl",
                       {"move-sink-input", std::to_string(input_index), std::to_string(sink_index)},
                       kTimeout);
}

void PulseCtl::set_source_volume_percent(const std::string& source_name, double percent) const {
    const double clamped = std::clamp(percent, 0.0, 150.0);
    // PulseAudio's raw volume scale: 65536 == 100%.
    const int raw = static_cast<int>(std::lround(clamped / 100.0 * 65536.0));
    run_status_checked("pactl", {"set-source-volume", source_name, std::to_string(raw)}, kTimeout);
}

} // namespace pipeduck
