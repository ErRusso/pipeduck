#include "pipeduck/tray.hpp"

#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pipeduck/common.hpp"
#include "pipeduck/config.hpp"
#include "pipeduck/duck.hpp"
#include "pipeduck/icons.hpp"
#include "pipeduck/pulse.hpp"
#include "pipeduck_icons.hpp"

namespace pipeduck {
namespace {

constexpr const char* kSniPath = "/StatusNotifierItem";
constexpr const char* kSniInterface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kMenuInterface = "com.canonical.dbusmenu";

enum MenuId {
    kIdInfoHeader = 1,
    kIdInfoDucking = 2,
    kIdInfoDetails = 3,
    kIdInfoSource = 4,
    kIdInfoMic = 5,
    kIdInfoTuning = 6,
    kIdToggle = 10,
    kIdTriggerMenu = 20,
    kIdTriggerCall = 21,
    kIdTriggerMic = 22,
    kIdTriggerBoth = 23,
    kIdSourceMenu = 30,
    kIdMicMenu = 40,
    kIdTuner = 50,
    kIdQuit = 60,
    kIdMicError = 999998,
    kIdSourceError = 999999,
    kIdSourceBase = 1000000,
    kIdMicBase = 2000000,
    kIdSourceGroupBase = 3000000,
    kIdMicGroupBase = 4000000,
};

const char* kSniXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <method name="ProvideXdgActivationToken">
      <arg name="token" type="s" direction="in"/>
    </method>
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus">
      <arg name="status" type="s"/>
    </signal>
    <signal name="NewMenu"/>
  </interface>
</node>
)XML";

const char* kMenuXml = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parent" type="i"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg name="id" type="i"/>
      <arg name="timestamp" type="u"/>
    </signal>
  </interface>
</node>
)XML";

enum class TrayRunState { Idle, Waiting, Starting, Neutral, Ducked, Stopping, Error };

struct QueuedEvent {
    uint64_t generation = 0;
    DuckingEvent event;
};

struct TrayApp {
    explicit TrayApp(DuckingSettings initial_settings) : settings(initial_settings) {}

    GMainLoop* loop = nullptr;
    GDBusConnection* bus = nullptr;
    GDBusNodeInfo* sni_info = nullptr;
    GDBusNodeInfo* menu_info = nullptr;
    guint sni_registration = 0;
    guint menu_registration = 0;
    guint registration_timer = 0;
    guint idle_source = 0;
    guint refresh_timer = 0;
    guint name_owner = 0;
    std::string service_name;
    bool unique_registration = false;

    SettingsStore settings;
    std::thread worker;
    std::shared_ptr<std::atomic<bool>> worker_stop;
    std::atomic<bool> worker_active{false};
    uint64_t generation = 0;
    bool watcher_warning = false;

    std::mutex queue_mutex;
    std::vector<QueuedEvent> queue;
    std::atomic<bool> idle_scheduled{false};

    TrayRunState state = TrayRunState::Idle;
    std::string message = "Ready";
    std::optional<std::string> source_label;
    std::optional<std::string> mic_label;
    bool quitting = false;
    uint32_t menu_revision = 1;
    uint64_t last_cache_revision = 0;
    std::optional<std::filesystem::file_time_type> last_config_mtime;

    int lock_fd = -1;

    ~TrayApp() {
        stop_worker();
        if (registration_timer != 0) {
            g_source_remove(registration_timer);
        }
        if (idle_source != 0) {
            g_source_remove(idle_source);
        }
        if (refresh_timer != 0) {
            g_source_remove(refresh_timer);
        }
        if (name_owner != 0) {
            g_bus_unown_name(name_owner);
        }
        if (bus != nullptr) {
            if (sni_registration != 0) {
                g_dbus_connection_unregister_object(bus, sni_registration);
            }
            if (menu_registration != 0) {
                g_dbus_connection_unregister_object(bus, menu_registration);
            }
            g_object_unref(bus);
        }
        if (sni_info != nullptr) {
            g_dbus_node_info_unref(sni_info);
        }
        if (menu_info != nullptr) {
            g_dbus_node_info_unref(menu_info);
        }
        if (lock_fd >= 0) {
            close(lock_fd);
        }
    }

    bool visible_state() const {
        return state == TrayRunState::Waiting || state == TrayRunState::Starting ||
               state == TrayRunState::Neutral || state == TrayRunState::Ducked;
    }

    std::string state_value() const {
        return visible_state() ? "ON" : "OFF";
    }

    std::string title() const {
        return "Ducking " + state_value();
    }

    std::string status() const {
        if (quitting)
            return "Passive";
        if (state == TrayRunState::Error)
            return "NeedsAttention";
        return "Active";
    }

    std::string tooltip_description() const {
        return title() + "\nDetails: " + message +
               "\nSource: " + source_label.value_or("not selected") +
               "\nMicrophone: " + mic_label.value_or("default");
    }

    DuckingSettings current_settings() const {
        try {
            return DuckingSettings::from_config(Config::load_or_default());
        } catch (const std::exception&) {
            return settings.get();
        }
    }

    bool can_toggle() const {
        return !quitting && state != TrayRunState::Starting && state != TrayRunState::Stopping;
    }

    bool tuner_enabled() const {
#ifdef PIPEDUCK_GUI
        return true;
#else
        return false;
#endif
    }

    std::string tuner_label() const {
#ifdef PIPEDUCK_GUI
        return "Tuner: open";
#else
        return "Tuner: GUI build only";
#endif
    }

    void request_toggle();
    void start_worker();
    void stop_worker();
    void post_event(const DuckingEvent& event);
    void dispatch_events();
    void notify_changed();
    void poll_external_state();
    void request_quit();
    void open_tuner();
    void select_source(uint32_t index);
    void select_mic(uint32_t index);
    void set_trigger(Trigger trigger);
    void register_with_watcher();
};

std::string config_source_label() {
    try {
        const Config config = Config::load_or_default();
        if (config.voice_source.has_value()) {
            return config.voice_source->label.value_or("configured source");
        }
    } catch (const std::exception&) {
    }
    return "not selected";
}

std::string config_mic_label() {
    try {
        const Config config = Config::load_or_default();
        if (config.mic_source.has_value()) {
            if (config.mic_source->label.has_value()) {
                return *config.mic_source->label;
            }
            return config.mic_source->name.value_or("default");
        }
    } catch (const std::exception&) {
    }
    return "default";
}

GVariant* empty_icon_pixmap() {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    return g_variant_builder_end(&builder);
}

GVariant* icon_pixmap_variant(bool on) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    const auto& pixmaps = on ? icons::tray_pixmaps_on : icons::tray_pixmaps;
    for (const auto& pixmap : pixmaps) {
        GVariant* bytes =
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixmap.data,
                                      static_cast<gsize>(pixmap.width * pixmap.height * 4), 1);
        g_variant_builder_add(&builder, "(ii@ay)", pixmap.width, pixmap.height, bytes);
    }
    return g_variant_builder_end(&builder);
}

GVariant* make_item(int id, const std::string& label, bool enabled = true,
                    std::optional<bool> checked = std::nullopt, GVariant* children = nullptr) {
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&properties, "{sv}", "label", g_variant_new_string(label.c_str()));
    g_variant_builder_add(&properties, "{sv}", "enabled", g_variant_new_boolean(enabled));
    g_variant_builder_add(&properties, "{sv}", "visible", g_variant_new_boolean(TRUE));
    if (checked.has_value()) {
        g_variant_builder_add(&properties, "{sv}", "toggle-type",
                              g_variant_new_string("checkmark"));
        g_variant_builder_add(&properties, "{sv}", "toggle-state",
                              g_variant_new_int32(*checked ? 1 : 0));
    }
    if (children != nullptr) {
        g_variant_builder_add(&properties, "{sv}", "children-display",
                              g_variant_new_string("submenu"));
    }

    GVariantBuilder empty_children;
    g_variant_builder_init(&empty_children, G_VARIANT_TYPE("av"));
    GVariantBuilder item;
    g_variant_builder_init(&item, G_VARIANT_TYPE("(ia{sv}av)"));
    g_variant_builder_add(&item, "i", id);
    g_variant_builder_add_value(&item, g_variant_builder_end(&properties));
    g_variant_builder_add_value(
        &item, children != nullptr ? children : g_variant_builder_end(&empty_children));
    return g_variant_builder_end(&item);
}

GVariant* make_children(const std::vector<GVariant*>& items) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("av"));
    for (GVariant* item : items) {
        g_variant_builder_add(&builder, "v", item);
    }
    return g_variant_builder_end(&builder);
}

std::vector<GVariant*> trigger_children(const Config& config) {
    std::vector<GVariant*> items;
    items.push_back(make_item(kIdTriggerCall, "Call audio (remote voice)", true,
                              config.trigger == Trigger::Call));
    items.push_back(
        make_item(kIdTriggerMic, "My microphone", true, config.trigger == Trigger::Mic));
    items.push_back(make_item(kIdTriggerBoth, "Call audio or microphone", true,
                              config.trigger == Trigger::Both));
    return items;
}

std::string application_group_key(const AudioIdentity& identity) {
    const auto first = [](const std::optional<std::string>& value) -> std::optional<std::string> {
        if (value.has_value() && !value->empty()) {
            return value;
        }
        return std::nullopt;
    };
    if (const auto name = first(identity.application_name)) {
        return *name;
    }
    if (const auto binary = first(identity.application_process_binary)) {
        return *binary;
    }
    if (const auto media = first(identity.media_name)) {
        return *media;
    }
    return "Unknown application";
}

std::string stream_label(const SinkInput& input) {
    const auto identity = input.identity();
    std::string label = "#" + std::to_string(input.index);
    if (identity.looks_like_voice_source()) {
        label += " voice?";
    }
    if (identity.media_name.has_value() && !identity.media_name->empty()) {
        label += " " + *identity.media_name;
    } else {
        label += " " + source_label(identity);
    }
    return label;
}

std::string microphone_group_key(const Source& source) {
    return source.description.empty() ? source.name : source.description;
}

std::vector<GVariant*> source_children() {
    std::vector<GVariant*> items;
    try {
        const PulseCtl pulse;
        const auto inputs = pulse.sink_inputs();

        std::map<std::string, std::vector<const SinkInput*>> groups;
        for (const auto& input : inputs) {
            if (!input.identity().is_playback_stream()) {
                continue;
            }
            groups[application_group_key(input.identity())].push_back(&input);
        }

        int group_ordinal = 0;
        for (auto& [application, streams] : groups) {
            std::sort(streams.begin(), streams.end(),
                      [](const SinkInput* left, const SinkInput* right) {
                          const auto left_name = left->identity().media_name.value_or("");
                          const auto right_name = right->identity().media_name.value_or("");
                          if (left_name != right_name) {
                              return left_name < right_name;
                          }
                          return left->index < right->index;
                      });

            if (streams.size() == 1) {
                const auto* input = streams.front();
                items.push_back(make_item(static_cast<int>(kIdSourceBase + input->index),
                                          stream_label(*input)));
                continue;
            }

            std::vector<GVariant*> children;
            children.reserve(streams.size());
            for (const auto* input : streams) {
                children.push_back(make_item(static_cast<int>(kIdSourceBase + input->index),
                                             stream_label(*input)));
            }
            const std::string label =
                application + " (" + std::to_string(streams.size()) + " streams)";
            items.push_back(make_item(static_cast<int>(kIdSourceGroupBase + group_ordinal), label,
                                      true, std::nullopt, make_children(children)));
            ++group_ordinal;
        }
    } catch (const std::exception& error) {
        items.push_back(
            make_item(kIdSourceError, std::string("Cannot read sources: ") + error.what(), false));
    }
    if (items.empty()) {
        items.push_back(make_item(kIdSourceError, "No playback streams visible", false));
    }
    return items;
}

std::vector<GVariant*> mic_children(const Config& config) {
    std::vector<GVariant*> items;
    std::string default_source;
    try {
        default_source = PulseCtl{}.default_source_name();
    } catch (const std::exception&) {
    }
    try {
        const PulseCtl pulse;
        const auto sources = pulse.sources();

        std::map<std::string, std::vector<const Source*>> groups;
        for (const auto& source : sources) {
            if (source.is_monitor) {
                continue;
            }
            groups[microphone_group_key(source)].push_back(&source);
        }

        int group_ordinal = 0;
        for (auto& [device, device_sources] : groups) {
            std::sort(device_sources.begin(), device_sources.end(),
                      [](const Source* left, const Source* right) {
                          if (left->name != right->name) {
                              return left->name < right->name;
                          }
                          return left->index < right->index;
                      });

            const auto selected = [&config](const Source& source) {
                return config.mic_source.has_value() && config.mic_source->name.has_value() &&
                       *config.mic_source->name == source.name;
            };

            if (device_sources.size() == 1) {
                const auto& source = *device_sources.front();
                std::string label = "#" + std::to_string(source.index) + " " + source.name;
                if (source.name == default_source) {
                    label += " (default)";
                }
                if (!source.description.empty()) {
                    label += " - " + source.description;
                }
                items.push_back(make_item(static_cast<int>(kIdMicBase + source.index), label, true,
                                          selected(source)));
                continue;
            }

            std::vector<GVariant*> children;
            children.reserve(device_sources.size());
            for (const auto* source : device_sources) {
                std::string label = "#" + std::to_string(source->index) + " " + source->name;
                if (source->name == default_source) {
                    label += " (default)";
                }
                children.push_back(make_item(static_cast<int>(kIdMicBase + source->index), label,
                                             true, selected(*source)));
            }
            const std::string label =
                device + " (" + std::to_string(device_sources.size()) + " inputs)";
            items.push_back(make_item(static_cast<int>(kIdMicGroupBase + group_ordinal), label,
                                      true, std::nullopt, make_children(children)));
            ++group_ordinal;
        }
    } catch (const std::exception& error) {
        items.push_back(
            make_item(kIdMicError, std::string("Cannot read microphones: ") + error.what(), false));
    }
    if (items.empty()) {
        items.push_back(make_item(kIdMicError, "No microphones visible", false));
    }
    return items;
}

GVariant* build_root_layout(TrayApp& app) {
    Config config;
    try {
        config = Config::load_or_default();
    } catch (const std::exception&) {
        config = Config::defaults();
    }
    const DuckingSettings settings = app.current_settings();
    std::vector<GVariant*> items;

    items.push_back(make_item(kIdInfoHeader, "Info:", false));
    items.push_back(make_item(kIdInfoDucking, "Ducking: " + app.state_value(), false));
    items.push_back(make_item(kIdInfoDetails, "Details: " + app.message, false));
    items.push_back(
        make_item(kIdInfoSource, "Source: " + app.source_label.value_or("not selected"), false));
    items.push_back(
        make_item(kIdInfoMic, "Microphone: " + app.mic_label.value_or("default"), false));
    items.push_back(make_item(kIdInfoTuning,
                              "Tuning: Duck " + std::to_string(settings.duck_percent) +
                                  "%, Call sens " + format_double(settings.vad_threshold, 4) +
                                  ", Mic sens " + format_double(settings.mic_vad_threshold, 4) +
                                  ", Hold " + std::to_string(settings.hold_ms) + "ms, Return " +
                                  std::to_string(settings.release_fade_ms) + "ms",
                              false));
    items.push_back(make_item(kIdToggle, "Ducking", app.can_toggle(), app.visible_state()));
    items.push_back(make_item(kIdTriggerMenu, "Trigger: " + trigger_name(config.trigger), true,
                              std::nullopt, make_children(trigger_children(config))));
    items.push_back(make_item(kIdSourceMenu, "Source: choose", true, std::nullopt,
                              make_children(source_children())));
    items.push_back(make_item(kIdMicMenu, "Microphone: choose", true, std::nullopt,
                              make_children(mic_children(config))));
    items.push_back(make_item(kIdTuner, app.tuner_label(), app.tuner_enabled()));
    items.push_back(make_item(kIdQuit, "Quit", !app.quitting));

    // DBusMenu GetLayout returns a single root item (id 0) whose children are
    // the top-level menu entries. This is what libdbusmenu, ksni and the GNOME
    // AppIndicator extension expect.
    return make_item(0, "", true, std::nullopt, make_children(items));
}

GVariant* find_item(GVariant* item, int wanted) {
    int32_t id = 0;
    GVariant* props = nullptr;
    GVariant* children = nullptr;
    g_variant_get(item, "(i@a{sv}@av)", &id, &props, &children);
    if (id == wanted) {
        g_variant_unref(children);
        g_variant_unref(props);
        return g_variant_ref(item);
    }

    GVariantIter iterator;
    g_variant_iter_init(&iterator, children);
    GVariant* child = nullptr;
    while ((child = g_variant_iter_next_value(&iterator)) != nullptr) {
        GVariant* nested = nullptr;
        g_variant_get(child, "v", &nested);
        g_variant_unref(child);
        if (nested == nullptr) {
            continue;
        }
        GVariant* found = find_item(nested, wanted);
        g_variant_unref(nested);
        if (found != nullptr) {
            g_variant_unref(children);
            g_variant_unref(props);
            return found;
        }
    }
    g_variant_unref(children);
    g_variant_unref(props);
    return nullptr;
}

GVariant* build_submenu_layout(TrayApp& app, int parent) {
    // The root layout contains every submenu, including per-application and
    // per-device groups. Return the requested subtree so hosts that fetch
    // submenus lazily see the same grouped children.
    GVariant* root = build_root_layout(app);
    GVariant* found = find_item(root, parent);
    g_variant_unref(root);
    if (found != nullptr) {
        return found;
    }
    std::vector<GVariant*> empty;
    return make_item(parent, "", true, std::nullopt, make_children(empty));
}

GVariant* find_props_in_children(GVariant* children, int wanted);

GVariant* find_props_in_item(GVariant* item, int wanted) {
    int32_t id = 0;
    GVariant* props = nullptr;
    GVariant* children = nullptr;
    g_variant_get(item, "(i@a{sv}@av)", &id, &props, &children);
    if (id == wanted) {
        g_variant_unref(children);
        return props;
    }
    GVariant* found = find_props_in_children(children, wanted);
    g_variant_unref(children);
    g_variant_unref(props);
    return found;
}

void collect_item_properties(GVariant* item, GVariantBuilder& builder) {
    int32_t id = 0;
    GVariant* props = nullptr;
    GVariant* children = nullptr;
    g_variant_get(item, "(i@a{sv}@av)", &id, &props, &children);
    g_variant_builder_add(&builder, "(i@a{sv})", id, props);

    GVariantIter iterator;
    g_variant_iter_init(&iterator, children);
    GVariant* child = nullptr;
    while ((child = g_variant_iter_next_value(&iterator)) != nullptr) {
        GVariant* nested = nullptr;
        g_variant_get(child, "v", &nested);
        g_variant_unref(child);
        if (nested != nullptr) {
            collect_item_properties(nested, builder);
            g_variant_unref(nested);
        }
    }
    g_variant_unref(children);
}

GVariant* find_props_in_children(GVariant* children, int wanted) {
    GVariantIter iterator;
    g_variant_iter_init(&iterator, children);
    GVariant* child = nullptr;
    while ((child = g_variant_iter_next_value(&iterator)) != nullptr) {
        GVariant* item = nullptr;
        g_variant_get(child, "v", &item);
        g_variant_unref(child);
        if (item == nullptr) {
            continue;
        }
        int32_t id = 0;
        GVariant* props = nullptr;
        GVariant* nested = nullptr;
        g_variant_get(item, "(i@a{sv}@av)", &id, &props, &nested);
        if (id == wanted) {
            g_variant_unref(nested);
            g_variant_unref(item);
            return props;
        }
        GVariant* found = find_props_in_children(nested, wanted);
        g_variant_unref(nested);
        g_variant_unref(props);
        g_variant_unref(item);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

void select_source_impl(uint32_t index) {
    const PulseCtl pulse;
    const auto inputs = pulse.sink_inputs();
    const auto input = std::find_if(inputs.begin(), inputs.end(), [index](const auto& candidate) {
        return candidate.index == index;
    });
    if (input == inputs.end()) {
        throw std::runtime_error("sink-input #" + std::to_string(index) + " not found");
    }
    const auto identity = input->identity();
    if (!identity.is_playback_stream()) {
        throw std::runtime_error("sink-input is not a playback stream");
    }
    Config config = Config::load_or_default();
    config.voice_source = configured_source_from_identity(source_label(identity), identity);
    config.save();
}

void select_mic_impl(uint32_t index) {
    const PulseCtl pulse;
    const auto sources = pulse.sources();
    const auto source =
        std::find_if(sources.begin(), sources.end(),
                     [index](const auto& candidate) { return candidate.index == index; });
    if (source == sources.end()) {
        throw std::runtime_error("source #" + std::to_string(index) + " not found");
    }
    if (source->is_monitor) {
        throw std::runtime_error("monitor sources cannot be used as microphones");
    }
    Config config = Config::load_or_default();
    config.mic_source = ConfiguredMic{source->name, source->description};
    config.save();
}

void TrayApp::select_source(uint32_t index) {
    try {
        select_source_impl(index);
        source_label = config_source_label();
        message = "Source saved: " + source_label.value_or("unknown");
        if (state == TrayRunState::Error) {
            state = TrayRunState::Idle;
        }
        notify_changed();
    } catch (const std::exception& error) {
        state = TrayRunState::Error;
        message = std::string("Could not save source: ") + error.what();
        notify_changed();
    }
}

void TrayApp::select_mic(uint32_t index) {
    try {
        select_mic_impl(index);
        mic_label = config_mic_label();
        message = "Microphone saved: " + mic_label.value_or("default");
        if (state == TrayRunState::Error) {
            state = TrayRunState::Idle;
        }
        notify_changed();
    } catch (const std::exception& error) {
        state = TrayRunState::Error;
        message = std::string("Could not save microphone: ") + error.what();
        notify_changed();
    }
}

void TrayApp::set_trigger(Trigger trigger) {
    try {
        Config config = Config::load_or_default();
        config.trigger = trigger;
        config.save();
        message = std::string("Trigger: ") + trigger_name(trigger);
        notify_changed();
    } catch (const std::exception& error) {
        state = TrayRunState::Error;
        message = std::string("Could not save trigger: ") + error.what();
        notify_changed();
    }
}

void TrayApp::post_event(const DuckingEvent& event) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        queue.push_back({generation, event});
    }
    if (!idle_scheduled.exchange(true)) {
        idle_source = g_idle_add(
            [](gpointer data) -> gboolean {
                static_cast<TrayApp*>(data)->dispatch_events();
                return G_SOURCE_REMOVE;
            },
            this);
    }
}

void TrayApp::dispatch_events() {
    idle_scheduled.store(false);
    idle_source = 0;
    std::vector<QueuedEvent> events;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        events.swap(queue);
    }
    bool changed = false;
    for (const auto& queued : events) {
        if (queued.generation != generation) {
            continue;
        }
        const auto& event = queued.event;
        if (event.type == DuckingEventType::Message && event.message == "__stopped__") {
            worker_active.store(false);
            if (worker.joinable()) {
                worker.join();
            }
            state = TrayRunState::Idle;
            message = "Off";
            changed = true;
            continue;
        }
        if (event.type == DuckingEventType::Message && starts_with(event.message, "__fatal__")) {
            worker_active.store(false);
            if (worker.joinable()) {
                worker.join();
            }
            state = TrayRunState::Error;
            message = event.message.substr(std::strlen("__fatal__"));
            changed = true;
            continue;
        }
        switch (event.type) {
        case DuckingEventType::WaitingForSource:
            state = TrayRunState::Waiting;
            message = event.message.empty() ? "Waiting for configured voice source" : event.message;
            changed = true;
            break;
        case DuckingEventType::Started:
            state = TrayRunState::Neutral;
            message = "Ready: " + event.label + ", threshold=" + format_double(event.threshold, 4) +
                      ", start=" + format_double(event.start_threshold, 4) +
                      ", hold=" + std::to_string(event.hold_ms) + "ms";
            changed = true;
            break;
        case DuckingEventType::VoiceActive:
            state = TrayRunState::Ducked;
            message = "Voice active: level=" + format_double(event.level, 4) + ", ducking " +
                      std::to_string(event.duck_percent) + "%";
            changed = true;
            break;
        case DuckingEventType::VoiceInactive:
            state = TrayRunState::Neutral;
            message = "Voice inactive: level=" + format_double(event.level, 4);
            changed = true;
            break;
        case DuckingEventType::Message:
            message = event.message;
            changed = true;
            break;
        }
    }
    if (changed) {
        notify_changed();
    }
}

void TrayApp::start_worker() {
    if (worker_active.load()) {
        return;
    }
    ++generation;
    state = TrayRunState::Starting;
    message = "Starting ducking...";
    notify_changed();

    worker_stop = std::make_shared<std::atomic<bool>>(false);
    worker_active.store(true);
    const uint64_t worker_generation = generation;
    const auto stop_flag = worker_stop;
    worker = std::thread([this, stop_flag, worker_generation] {
        (void)worker_generation;
        try {
            run_until_stopped(*stop_flag, settings, true,
                              [this](const DuckingEvent& event) { post_event(event); });
            DuckingEvent stopped;
            stopped.type = DuckingEventType::Message;
            stopped.message = "__stopped__";
            post_event(stopped);
        } catch (const std::exception& error) {
            DuckingEvent event;
            event.type = DuckingEventType::Message;
            event.message = std::string("__fatal__") + error.what();
            post_event(event);
        }
    });
}

void TrayApp::stop_worker() {
    if (worker.joinable()) {
        if (worker_stop != nullptr) {
            worker_stop->store(true);
        }
        worker.join();
    }
    worker_active.store(false);
    if (quitting) {
        return;
    }
    state = TrayRunState::Idle;
    message = "Off";
}

void TrayApp::request_toggle() {
    if (!can_toggle()) {
        return;
    }
    if (worker_active.load()) {
        state = TrayRunState::Stopping;
        message = "Stopping ducking...";
        notify_changed();
        stop_worker();
        ++generation;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue.clear();
        }
        message = "Off";
        notify_changed();
    } else {
        start_worker();
    }
}

void TrayApp::request_quit() {
    if (quitting) {
        return;
    }
    quitting = true;
    state = TrayRunState::Stopping;
    message = "Quitting...";
    notify_changed();
    stop_worker();
    g_timeout_add(
        120,
        [](gpointer data) -> gboolean {
            auto* app = static_cast<TrayApp*>(data);
            if (app->loop != nullptr) {
                g_main_loop_quit(app->loop);
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

void TrayApp::open_tuner() {
    message = "Opening tuner...";
    notify_changed();
#ifdef PIPEDUCK_GUI
    const auto exe = std::filesystem::read_symlink("/proc/self/exe");
    const pid_t first = fork();
    if (first == 0) {
        const pid_t second = fork();
        if (second == 0) {
            execl(exe.c_str(), exe.c_str(), "tune-gui", nullptr);
            _exit(127);
        }
        _exit(second < 0 ? 1 : 0);
    }
    if (first > 0) {
        int status = 0;
        waitpid(first, &status, 0);
    }
    message = "Tuner opened";
#else
    message = "The tuner GUI is not included in this build; use 'pipeduck tune'";
#endif
    notify_changed();
}

void TrayApp::notify_changed() {
    ++menu_revision;
    if (bus == nullptr) {
        return;
    }
    g_dbus_connection_emit_signal(bus, nullptr, kSniPath, kSniInterface, "NewTitle", nullptr,
                                  nullptr);
    g_dbus_connection_emit_signal(bus, nullptr, kSniPath, kSniInterface, "NewIcon", nullptr,
                                  nullptr);
    g_dbus_connection_emit_signal(bus, nullptr, kSniPath, kSniInterface, "NewToolTip", nullptr,
                                  nullptr);
    g_dbus_connection_emit_signal(bus, nullptr, kSniPath, kSniInterface, "NewStatus",
                                  g_variant_new("(s)", status().c_str()), nullptr);
    g_dbus_connection_emit_signal(bus, nullptr, kMenuPath, kMenuInterface, "LayoutUpdated",
                                  g_variant_new("(ui)", menu_revision, 0), nullptr);
}

void TrayApp::poll_external_state() {
    bool changed = false;

    const uint64_t revision = PulseCtl::cache_revision();
    if (revision != last_cache_revision) {
        last_cache_revision = revision;
        changed = true;
    }

    std::error_code error;
    const auto mtime = std::filesystem::last_write_time(config_path(), error);
    if (!error && (!last_config_mtime.has_value() || *last_config_mtime != mtime)) {
        last_config_mtime = mtime;
        source_label = config_source_label();
        mic_label = config_mic_label();
        changed = true;
    }

    if (changed) {
        notify_changed();
    }
}

void TrayApp::register_with_watcher() {
    if (bus == nullptr) {
        return;
    }
    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(
        bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", unique_registration || service_name.empty()
                                 ? g_dbus_connection_get_unique_name(bus)
                                 : service_name.c_str()),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    if (result != nullptr) {
        g_variant_unref(result);
        return;
    }
    if (error != nullptr) {
        if (!watcher_warning) {
            std::cerr << "pipeduck: no StatusNotifierWatcher yet (" << error->message
                      << "); will retry\n";
            watcher_warning = true;
        }
        g_error_free(error);
    }
    if (registration_timer == 0) {
        registration_timer = g_timeout_add_seconds(
            3,
            [](gpointer data) -> gboolean {
                auto* app = static_cast<TrayApp*>(data);
                app->registration_timer = 0;
                app->register_with_watcher();
                return G_SOURCE_REMOVE;
            },
            this);
    }
}

void on_sni_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                        const gchar* method_name, GVariant* parameters,
                        GDBusMethodInvocation* invocation, gpointer user_data) {
    (void)parameters;
    auto* app = static_cast<TrayApp*>(user_data);
    if (std::strcmp(method_name, "Activate") == 0 ||
        std::strcmp(method_name, "SecondaryActivate") == 0) {
        app->request_toggle();
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (std::strcmp(method_name, "ContextMenu") == 0) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (std::strcmp(method_name, "Scroll") == 0) {
        app->request_toggle();
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (std::strcmp(method_name, "ProvideXdgActivationToken") == 0) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

GVariant* on_sni_get_property(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                              const gchar* property_name, GError**, gpointer user_data) {
    auto* app = static_cast<TrayApp*>(user_data);
    const bool on = app->visible_state();
    if (std::strcmp(property_name, "Category") == 0) {
        return g_variant_new_string("ApplicationStatus");
    }
    if (std::strcmp(property_name, "Id") == 0) {
        return g_variant_new_string("pipeduck");
    }
    if (std::strcmp(property_name, "Title") == 0) {
        return g_variant_new_string(app->title().c_str());
    }
    if (std::strcmp(property_name, "Status") == 0) {
        return g_variant_new_string(app->status().c_str());
    }
    if (std::strcmp(property_name, "WindowId") == 0) {
        return g_variant_new_int32(0);
    }
    if (std::strcmp(property_name, "IconName") == 0 ||
        std::strcmp(property_name, "OverlayIconName") == 0 ||
        std::strcmp(property_name, "AttentionIconName") == 0 ||
        std::strcmp(property_name, "AttentionMovieName") == 0) {
        return g_variant_new_string("");
    }
    if (std::strcmp(property_name, "IconPixmap") == 0) {
        return icon_pixmap_variant(on);
    }
    if (std::strcmp(property_name, "OverlayIconPixmap") == 0 ||
        std::strcmp(property_name, "AttentionIconPixmap") == 0) {
        return empty_icon_pixmap();
    }
    if (std::strcmp(property_name, "ToolTip") == 0) {
        return g_variant_new("(s@a(iiay)ss)", "", icon_pixmap_variant(on), app->title().c_str(),
                             app->tooltip_description().c_str());
    }
    if (std::strcmp(property_name, "ItemIsMenu") == 0) {
        return g_variant_new_boolean(FALSE);
    }
    if (std::strcmp(property_name, "Menu") == 0) {
        return g_variant_new_object_path(kMenuPath);
    }
    return nullptr;
}

void handle_menu_event(TrayApp& app, int id) {
    if (id == kIdToggle) {
        app.request_toggle();
    } else if (id == kIdTriggerCall) {
        app.set_trigger(Trigger::Call);
    } else if (id == kIdTriggerMic) {
        app.set_trigger(Trigger::Mic);
    } else if (id == kIdTriggerBoth) {
        app.set_trigger(Trigger::Both);
    } else if (id == kIdTuner) {
        app.open_tuner();
    } else if (id == kIdQuit) {
        app.request_quit();
    } else if (id >= kIdSourceBase && id < kIdMicBase) {
        app.select_source(static_cast<uint32_t>(id - kIdSourceBase));
    } else if (id >= kIdMicBase && id < kIdSourceGroupBase) {
        app.select_mic(static_cast<uint32_t>(id - kIdMicBase));
    }
    // Group items (kIdSourceGroupBase..) are submenu parents and have no action.
}

void handle_menu_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                             const gchar* method_name, GVariant* parameters,
                             GDBusMethodInvocation* invocation, gpointer user_data) {
    auto* app = static_cast<TrayApp*>(user_data);

    if (std::strcmp(method_name, "GetLayout") == 0) {
        int32_t parent = 0;
        int32_t depth = -1;
        GVariant* property_names = nullptr;
        // The current DBusMenu signature is (iias). Tolerate older two-argument
        // callers as well, since several tray implementations still send (ii).
        if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(iias)"))) {
            g_variant_get(parameters, "(ii@as)", &parent, &depth, &property_names);
        } else {
            g_variant_get(parameters, "(ii)", &parent, &depth);
        }
        GVariant* layout =
            parent == 0 ? build_root_layout(*app) : build_submenu_layout(*app, parent);
        if (property_names != nullptr) {
            g_variant_unref(property_names);
        }
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(u@(ia{sv}av))", app->menu_revision, layout));
        return;
    }

    if (std::strcmp(method_name, "GetGroupProperties") == 0) {
        GVariant* ids = nullptr;
        GVariant* names = nullptr;
        g_variant_get(parameters, "(@ai@as)", &ids, &names);
        GVariant* layout = build_root_layout(*app);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
        GVariantIter iterator;
        g_variant_iter_init(&iterator, ids);
        int32_t id = 0;
        while (g_variant_iter_next(&iterator, "i", &id)) {
            GVariant* props = find_props_in_item(layout, id);
            if (props != nullptr) {
                g_variant_builder_add(&builder, "(i@a{sv})", id, props);
            }
        }
        if (g_variant_n_children(ids) == 0) {
            collect_item_properties(layout, builder);
        }
        g_variant_unref(ids);
        g_variant_unref(names);
        g_variant_unref(layout);
        GVariant* array = g_variant_builder_end(&builder);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a(ia{sv}))", array));
        return;
    }

    if (std::strcmp(method_name, "GetProperty") == 0) {
        int32_t id = 0;
        const char* property = nullptr;
        g_variant_get(parameters, "(i&s)", &id, &property);
        GVariant* layout = build_root_layout(*app);
        GVariant* props = find_props_in_item(layout, id);
        GVariant* value = nullptr;
        if (props != nullptr) {
            value = g_variant_lookup_value(props, property, nullptr);
            g_variant_unref(props);
        }
        g_variant_unref(layout);
        if (value == nullptr) {
            g_dbus_method_invocation_return_dbus_error(invocation, "com.canonical.dbusmenu.Error",
                                                       "Unknown property");
            return;
        }
        GVariant* result = g_variant_new("(v)", value);
        g_variant_unref(value);
        g_dbus_method_invocation_return_value(invocation, result);
        return;
    }

    if (std::strcmp(method_name, "Event") == 0) {
        int32_t id = 0;
        const char* event_id = nullptr;
        GVariant* data = nullptr;
        uint32_t timestamp = 0;
        g_variant_get(parameters, "(i&svu)", &id, &event_id, &data, &timestamp);
        if (std::strcmp(event_id, "clicked") == 0) {
            handle_menu_event(*app, id);
        }
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (std::strcmp(method_name, "EventGroup") == 0) {
        GVariant* events = nullptr;
        g_variant_get(parameters, "(@a(isvu))", &events);
        GVariantIter iterator;
        g_variant_iter_init(&iterator, events);
        int32_t id = 0;
        const char* event_id = nullptr;
        GVariant* data = nullptr;
        uint32_t timestamp = 0;
        while (g_variant_iter_next(&iterator, "(i&svu)", &id, &event_id, &data, &timestamp)) {
            if (std::strcmp(event_id, "clicked") == 0) {
                handle_menu_event(*app, id);
            }
            g_variant_unref(data);
        }
        g_variant_unref(events);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (std::strcmp(method_name, "AboutToShow") == 0) {
        // Return false like ksni/Qt: libdbusmenu ignores the return value and
        // hosts such as GNOME rebuild the open menu when it is true, which
        // closes submenus the moment they are opened. Our menu is generated
        // fresh for every GetLayout call anyway.
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
    }

    if (std::strcmp(method_name, "AboutToShowGroup") == 0) {
        GVariantBuilder updates;
        g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai@ai)", g_variant_builder_end(&updates),
                                      g_variant_builder_end(&errors)));
        return;
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

void on_menu_method_call(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
                         const gchar* interface_name, const gchar* method_name,
                         GVariant* parameters, GDBusMethodInvocation* invocation,
                         gpointer user_data) {
    try {
        handle_menu_method_call(connection, sender, object_path, interface_name, method_name,
                                parameters, invocation, user_data);
    } catch (const std::exception& error) {
        g_dbus_method_invocation_return_dbus_error(invocation, "com.canonical.dbusmenu.Error",
                                                   error.what());
    }
}

GVariant* on_menu_get_property(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                               const gchar* property_name, GError**, gpointer) {
    if (std::strcmp(property_name, "Version") == 0) {
        return g_variant_new_uint32(3);
    }
    if (std::strcmp(property_name, "TextDirection") == 0) {
        return g_variant_new_string("ltr");
    }
    if (std::strcmp(property_name, "Status") == 0) {
        return g_variant_new_string("normal");
    }
    if (std::strcmp(property_name, "IconThemePath") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        if (const auto path = icons::icon_theme_path()) {
            g_variant_builder_add(&builder, "s", path->c_str());
        }
        return g_variant_builder_end(&builder);
    }
    return nullptr;
}

bool status_notifier_watcher_is_ashell() {
    const auto result =
        run_command("busctl", {"--user", "--no-pager", "status", "org.kde.StatusNotifierWatcher"},
                    std::chrono::milliseconds(2000));
    if (result.exit_code != 0) {
        return false;
    }
    for (const auto& raw_line : split_lines(result.out)) {
        const std::string line = trim(raw_line);
        if (line == "Comm=ashell" || line == "CommandLine=ashell" ||
            starts_with(line, "CommandLine=ashell ")) {
            return true;
        }
    }
    return false;
}

bool use_unique_sni_name() {
    if (const auto value = getenv_string("PIPEDUCK_SNI_UNIQUE_NAME")) {
        const std::string lower = to_lower(*value);
        return lower != "0" && lower != "false" && lower != "no";
    }
    return status_notifier_watcher_is_ashell();
}

std::string lock_path_string() {
    const auto path = runtime_dir() / "pipeduck" / "tray.lock";
    std::filesystem::create_directories(path.parent_path());
    return path.string();
}

int acquire_instance_lock() {
    const std::string path = lock_path_string();
    const int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw std::runtime_error("could not open the tray lock file " + path);
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        std::ifstream input(path);
        std::string pid;
        std::getline(input, pid);
        close(fd);
        throw std::runtime_error("pipeduck is already running" +
                                 (pid.empty() ? std::string() : " as process " + pid));
    }
    ftruncate(fd, 0);
    const std::string pid = std::to_string(getpid()) + "\n";
    if (write(fd, pid.data(), pid.size()) < 0) {
        // The lock itself is enough; the PID is informational.
    }
    return fd;
}

} // namespace

int run_tray(const TrayOptions& options) {
    const int lock_fd = acquire_instance_lock();

    TrayApp app(options.settings);
    app.lock_fd = lock_fd;
    app.source_label = config_source_label();
    app.mic_label = config_mic_label();
    app.last_cache_revision = PulseCtl::cache_revision();
    {
        std::error_code error;
        const auto mtime = std::filesystem::last_write_time(config_path(), error);
        if (!error) {
            app.last_config_mtime = mtime;
        }
    }
    app.loop = g_main_loop_new(nullptr, FALSE);
    app.refresh_timer = g_timeout_add(
        400,
        [](gpointer data) -> gboolean {
            static_cast<TrayApp*>(data)->poll_external_state();
            return G_SOURCE_CONTINUE;
        },
        &app);

    GError* error = nullptr;
    app.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (app.bus == nullptr) {
        const std::string message = error != nullptr ? error->message : "no session bus";
        if (error != nullptr) {
            g_error_free(error);
        }
        throw std::runtime_error("cannot connect to the D-Bus session bus: " + message);
    }

    app.sni_info = g_dbus_node_info_new_for_xml(kSniXml, &error);
    if (app.sni_info == nullptr) {
        throw std::runtime_error(std::string("invalid SNI interface: ") +
                                 (error != nullptr ? error->message : "unknown"));
    }
    app.menu_info = g_dbus_node_info_new_for_xml(kMenuXml, &error);
    if (app.menu_info == nullptr) {
        throw std::runtime_error(std::string("invalid menu interface: ") +
                                 (error != nullptr ? error->message : "unknown"));
    }

    GDBusInterfaceVTable sni_vtable{};
    sni_vtable.method_call = on_sni_method_call;
    sni_vtable.get_property = on_sni_get_property;
    app.sni_registration = g_dbus_connection_register_object(
        app.bus, kSniPath, app.sni_info->interfaces[0], &sni_vtable, &app, nullptr, &error);
    if (app.sni_registration == 0) {
        throw std::runtime_error(std::string("cannot export the tray icon: ") +
                                 (error != nullptr ? error->message : "unknown"));
    }

    GDBusInterfaceVTable menu_vtable{};
    menu_vtable.method_call = on_menu_method_call;
    menu_vtable.get_property = on_menu_get_property;
    app.menu_registration = g_dbus_connection_register_object(
        app.bus, kMenuPath, app.menu_info->interfaces[0], &menu_vtable, &app, nullptr, &error);
    if (app.menu_registration == 0) {
        throw std::runtime_error(std::string("cannot export the tray menu: ") +
                                 (error != nullptr ? error->message : "unknown"));
    }

    app.unique_registration = use_unique_sni_name();
    app.service_name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    app.name_owner = g_bus_own_name_on_connection(
        app.bus, app.service_name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
        [](GDBusConnection*, const gchar* name, gpointer data) {
            auto* tray_app = static_cast<TrayApp*>(data);
            tray_app->service_name = name;
            tray_app->register_with_watcher();
        },
        nullptr, &app, nullptr);
    if (app.name_owner == 0) {
        app.register_with_watcher();
    }
    std::cout << "pipeduck is running. Left click the tray icon to toggle ducking.\n";

    g_unix_signal_add(
        SIGINT,
        [](gpointer data) -> gboolean {
            static_cast<TrayApp*>(data)->request_quit();
            return G_SOURCE_CONTINUE;
        },
        &app);
    g_unix_signal_add(
        SIGTERM,
        [](gpointer data) -> gboolean {
            static_cast<TrayApp*>(data)->request_quit();
            return G_SOURCE_CONTINUE;
        },
        &app);

    g_main_loop_run(app.loop);
    app.stop_worker();
    g_main_loop_unref(app.loop);
    app.loop = nullptr;
    return 0;
}

} // namespace pipeduck
