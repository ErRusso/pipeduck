#include "pipeduck/tune.hpp"

#ifdef PIPEDUCK_GUI

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "pipeduck/config.hpp"
#include "pipeduck/duck.hpp"
#include "pipeduck/icons.hpp"
#include "pipeduck/vad.hpp"

namespace pipeduck {
namespace {

constexpr double kThresholdMin = 0.0025;
constexpr double kThresholdMax = 0.2;

struct TunerContext {
    GtkWidget* sensitivity = nullptr;
    GtkWidget* duck = nullptr;
    GtkWidget* hold = nullptr;
    GtkWidget* attack_fade = nullptr;
    GtkWidget* release_fade = nullptr;
    GtkWidget* trigger = nullptr;
    GtkWidget* sensitivity_value = nullptr;
    GtkWidget* duck_value = nullptr;
    GtkWidget* hold_value = nullptr;
    GtkWidget* attack_value = nullptr;
    GtkWidget* release_value = nullptr;
    bool updating = false;
};

double scale_value(GtkWidget* scale) {
    return gtk_range_get_value(GTK_RANGE(scale));
}

double sensitivity_percent(double threshold) {
    const double normalized =
        1.0 - ((std::clamp(threshold, kThresholdMin, kThresholdMax) - kThresholdMin) /
               (kThresholdMax - kThresholdMin));
    return std::round(normalized * 100.0);
}

double threshold_from_percent(double percent) {
    const double normalized = std::clamp(percent, 0.0, 100.0) / 100.0;
    return kThresholdMin + (1.0 - normalized) * (kThresholdMax - kThresholdMin);
}

std::string sensitivity_label(double threshold) {
    const double percent = sensitivity_percent(threshold);
    if (vad_is_disabled(threshold)) {
        return std::to_string(static_cast<int>(percent)) + "% - OFF";
    }
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%.0f%% - threshold %.4f - start %.4f", percent,
                  threshold, vad_start_threshold(threshold));
    return buffer;
}

DuckingSettings settings_from_widgets(TunerContext& context) {
    DuckingSettings settings;
    settings.vad_threshold = threshold_from_percent(scale_value(context.sensitivity));
    settings.duck_percent = static_cast<int>(std::lround(scale_value(context.duck)));
    settings.hold_ms = static_cast<int>(std::lround(scale_value(context.hold) / 50.0) * 50);
    settings.attack_fade_ms =
        static_cast<int>(std::lround(scale_value(context.attack_fade) / 10.0) * 10);
    settings.release_fade_ms =
        static_cast<int>(std::lround(scale_value(context.release_fade) / 10.0) * 10);
    const int trigger_index = gtk_drop_down_get_selected(GTK_DROP_DOWN(context.trigger));
    settings.trigger = trigger_index == 1   ? Trigger::Mic
                       : trigger_index == 2 ? Trigger::Both
                                            : Trigger::Call;
    return settings.normalized();
}

void refresh_labels(TunerContext& context, const DuckingSettings& settings) {
    gtk_label_set_text(GTK_LABEL(context.sensitivity_value),
                       sensitivity_label(settings.vad_threshold).c_str());
    const std::string duck = std::to_string(settings.duck_percent) + "%";
    gtk_label_set_text(GTK_LABEL(context.duck_value), duck.c_str());
    const std::string hold = std::to_string(settings.hold_ms) + " ms";
    gtk_label_set_text(GTK_LABEL(context.hold_value), hold.c_str());
    const std::string attack = std::to_string(settings.attack_fade_ms) + " ms";
    gtk_label_set_text(GTK_LABEL(context.attack_value), attack.c_str());
    const std::string release = std::to_string(settings.release_fade_ms) + " ms";
    gtk_label_set_text(GTK_LABEL(context.release_value), release.c_str());
}

void save_and_refresh(TunerContext& context) {
    if (context.updating) {
        return;
    }
    const DuckingSettings settings = settings_from_widgets(context);
    refresh_labels(context, settings);
    try {
        Config config = Config::load_or_default();
        settings.apply_to(config);
        config.save();
    } catch (const std::exception& error) {
        g_printerr("pipeduck: could not save tuner settings: %s\n", error.what());
    }
}

void on_scale_changed(GtkRange*, gpointer user_data) {
    save_and_refresh(*static_cast<TunerContext*>(user_data));
}

void on_trigger_changed(GObject*, GParamSpec*, gpointer user_data) {
    save_and_refresh(*static_cast<TunerContext*>(user_data));
}

GtkWidget* append_slider_row(GtkWidget* root, const char* title, const char* subtitle, double value,
                             double min, double max, double step, GtkWidget** out_scale,
                             GtkWidget** out_value) {
    (void)root;
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_add_css_class(label, "heading");

    GtkWidget* value_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(value_label), 1.0f);
    gtk_label_set_width_chars(GTK_LABEL(value_label), 32);
    gtk_widget_set_halign(value_label, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(header), label);
    gtk_box_append(GTK_BOX(header), value_label);
    gtk_box_append(GTK_BOX(outer), header);

    GtkWidget* hint = gtk_label_new(subtitle);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(outer), hint);

    GtkAdjustment* adjustment = gtk_adjustment_new(value, min, max, step, step * 5, 0);
    GtkWidget* scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adjustment);
    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_box_append(GTK_BOX(outer), scale);

    *out_scale = scale;
    *out_value = value_label;
    return outer;
}

void on_window_destroy(GtkWidget*, gpointer user_data) {
    g_main_loop_quit(static_cast<GMainLoop*>(user_data));
}

} // namespace

int run_gui_tuner() {
    gtk_init();

    if (auto path = icons::icon_theme_path()) {
        if (GdkDisplay* display = gdk_display_get_default()) {
            gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(display), path->c_str());
        }
    }
    gtk_window_set_default_icon_name(icons::kAppIconName);

    const Config config = Config::load_or_default();
    const DuckingSettings settings = DuckingSettings::from_config(config);

    GtkWidget* window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "pipeduck Tuner");
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 480);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_window_set_child(GTK_WINDOW(window), root);

    GtkWidget* title = gtk_label_new("pipeduck Tuner");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_add_css_class(title, "title-2");
    gtk_box_append(GTK_BOX(root), title);

    TunerContext context;
    gtk_box_append(GTK_BOX(root),
                   append_slider_row(root, "Sensitivity", "0% = off, 100% = very sensitive",
                                     sensitivity_percent(settings.vad_threshold), 0, 100, 1,
                                     &context.sensitivity, &context.sensitivity_value));
    gtk_box_append(GTK_BOX(root),
                   append_slider_row(root, "Ducking volume", "Target volume while speech is active",
                                     settings.duck_percent, 0, 100, 1, &context.duck,
                                     &context.duck_value));
    gtk_box_append(
        GTK_BOX(root),
        append_slider_row(root, "Hold", "How long ducking stays active after speech stops",
                          settings.hold_ms, 0, 4000, 50, &context.hold, &context.hold_value));
    gtk_box_append(GTK_BOX(root),
                   append_slider_row(root, "Duck fade", "Smooth fade down when speech starts",
                                     settings.attack_fade_ms, 0, 2000, 10, &context.attack_fade,
                                     &context.attack_value));
    gtk_box_append(GTK_BOX(root),
                   append_slider_row(root, "Return fade", "Smooth fade back to the correct volume",
                                     settings.release_fade_ms, 0, 3000, 10, &context.release_fade,
                                     &context.release_value));

    GtkWidget* trigger_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* trigger_label = gtk_label_new("Trigger");
    gtk_label_set_xalign(GTK_LABEL(trigger_label), 0.0f);
    gtk_widget_set_hexpand(trigger_label, TRUE);
    gtk_widget_add_css_class(trigger_label, "heading");
    const char* triggers[] = {"Call audio", "My microphone", "Call or microphone", nullptr};
    GtkWidget* trigger = gtk_drop_down_new_from_strings(triggers);
    const int trigger_index = settings.trigger == Trigger::Mic    ? 1
                              : settings.trigger == Trigger::Both ? 2
                                                                  : 0;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(trigger), static_cast<guint>(trigger_index));
    gtk_box_append(GTK_BOX(trigger_row), trigger_label);
    gtk_box_append(GTK_BOX(trigger_row), trigger);
    gtk_box_append(GTK_BOX(root), trigger_row);
    context.trigger = trigger;

    std::string mic_text = "Microphone: default";
    if (config.mic_source.has_value()) {
        mic_text = "Microphone: " + (config.mic_source->label.has_value()
                                         ? *config.mic_source->label
                                         : config.mic_source->name.value_or("default"));
    }
    GtkWidget* mic_label = gtk_label_new(mic_text.c_str());
    gtk_label_set_xalign(GTK_LABEL(mic_label), 0.0f);
    gtk_widget_add_css_class(mic_label, "dim-label");
    gtk_box_append(GTK_BOX(root), mic_label);

    GtkWidget* hint = gtk_label_new("Changes are saved immediately and apply to the running tray.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(root), hint);

    GtkWidget* path_label = gtk_label_new(("Config: " + config_path().string()).c_str());
    gtk_label_set_xalign(GTK_LABEL(path_label), 0.0f);
    gtk_widget_add_css_class(path_label, "dim-label");
    gtk_box_append(GTK_BOX(root), path_label);

    context.updating = true;
    for (GtkWidget* scale : {context.sensitivity, context.duck, context.hold, context.attack_fade,
                             context.release_fade}) {
        g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale_changed), &context);
    }
    g_signal_connect(trigger, "notify::selected", G_CALLBACK(on_trigger_changed), &context);
    refresh_labels(context, settings);
    context.updating = false;

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), loop);
    gtk_window_present(GTK_WINDOW(window));
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return 0;
}

} // namespace pipeduck

#endif // PIPEDUCK_GUI
