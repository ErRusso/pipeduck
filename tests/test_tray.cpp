// Integration test for the StatusNotifierItem and DBusMenu contract.
//
// It is intended to run under a private session bus:
//
//   dbus-run-session -- ./pipeduck_tray_tests /path/to/pipeduck
//
// The test starts the real tray binary with a temporary config directory and a
// deliberately unreachable PulseAudio socket, then drives the D-Bus interface
// directly. It therefore needs no audio server or desktop environment.

#include <gio/gio.h>
#include <glib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr const char* kSniPath = "/StatusNotifierItem";
constexpr const char* kSniInterface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kMenuInterface = "com.canonical.dbusmenu";

GDBusConnection* g_bus = nullptr;
int g_failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);              \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (false)

GVariant* call(const std::string& service, const char* path, const char* interface,
               const char* method, GVariant* parameters, const GVariantType* reply_type) {
    GError* error = nullptr;
    GVariant* reply =
        g_dbus_connection_call_sync(g_bus, service.c_str(), path, interface, method, parameters,
                                    reply_type, G_DBUS_CALL_FLAGS_NONE, 4000, nullptr, &error);
    if (reply == nullptr) {
        std::fprintf(stderr, "FAIL %s.%s: %s\n", interface, method,
                     error != nullptr ? error->message : "unknown D-Bus error");
        ++g_failures;
        if (error != nullptr) {
            g_error_free(error);
        }
    }
    return reply;
}

GVariant* empty_string_array() {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
    return g_variant_builder_end(&builder);
}

GVariant* get_layout(const std::string& service, uint32_t* revision, int parent = 0) {
    GVariant* reply = call(service, kMenuPath, kMenuInterface, "GetLayout",
                           g_variant_new("(ii@as)", parent, -1, empty_string_array()),
                           G_VARIANT_TYPE("(u(ia{sv}av))"));
    if (reply == nullptr) {
        return nullptr;
    }
    CHECK(g_variant_is_of_type(reply, G_VARIANT_TYPE("(u(ia{sv}av))")));
    GVariant* layout = nullptr;
    g_variant_get(reply, "(u@(ia{sv}av))", revision, &layout);
    g_variant_unref(reply);
    return layout;
}

bool service_has_owner(const std::string& name) {
    GVariant* reply = g_dbus_connection_call_sync(
        g_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameHasOwner", g_variant_new("(s)", name.c_str()), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (reply == nullptr) {
        return false;
    }
    gboolean owned = FALSE;
    g_variant_get(reply, "(b)", &owned);
    g_variant_unref(reply);
    return owned == TRUE;
}

bool wait_for_service(const std::string& name) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (service_has_owner(name)) {
            return true;
        }
        g_usleep(50 * 1000);
    }
    return false;
}

bool wait_for_file_text(const std::filesystem::path& path, const std::string& needle) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::ifstream input(path);
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (text.find(needle) != std::string::npos) {
            return true;
        }
        g_usleep(50 * 1000);
    }
    return false;
}

void click(const std::string& service, int id) {
    GVariant* reply = call(
        service, kMenuPath, kMenuInterface, "Event",
        g_variant_new("(isvu)", id, "clicked", g_variant_new_variant(g_variant_new_int32(0)), 0u),
        nullptr);
    if (reply != nullptr) {
        g_variant_unref(reply);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s /path/to/pipeduck\n", argv[0]);
        return 2;
    }

    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() /
        ("pipeduck-tray-test-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(temporary);
    std::filesystem::create_directories(temporary / "config");
    std::filesystem::create_directories(temporary / "runtime");
    const auto config_file = temporary / "config" / "pipeduck" / "config.toml";

    setenv("XDG_CONFIG_HOME", (temporary / "config").c_str(), 1);
    setenv("XDG_RUNTIME_DIR", (temporary / "runtime").c_str(), 1);
    setenv("PIPEDUCK_SNI_UNIQUE_NAME", "1", 1);
    // Keep the test independent of the developer's audio server.
    setenv("PULSE_SERVER", "unix:/nonexistent/pipeduck-pulse", 1);

    GError* error = nullptr;
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (g_bus == nullptr) {
        std::fprintf(stderr, "cannot connect to the session bus: %s\n",
                     error != nullptr ? error->message : "unknown error");
        if (error != nullptr) {
            g_error_free(error);
        }
        return 1;
    }

    gchar* child_argv[] = {const_cast<gchar*>(argv[1]), const_cast<gchar*>("tray"), nullptr};
    GPid pid = 0;
    g_spawn_async(nullptr, child_argv, nullptr, G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr, &pid,
                  &error);
    if (pid == 0) {
        std::fprintf(stderr, "cannot start the tray: %s\n",
                     error != nullptr ? error->message : "unknown error");
        if (error != nullptr) {
            g_error_free(error);
        }
        return 1;
    }

    const std::string service = "org.kde.StatusNotifierItem-" + std::to_string(pid) + "-1";
    const bool started = wait_for_service(service);

    if (started) {
        GVariant* properties = call(service, kSniPath, "org.freedesktop.DBus.Properties", "GetAll",
                                    g_variant_new("(s)", kSniInterface), G_VARIANT_TYPE("(a{sv})"));
        if (properties != nullptr) {
            GVariant* dict = nullptr;
            g_variant_get(properties, "(@a{sv})", &dict);
            for (const char* key : {"Id", "Title", "Status", "Menu", "IconPixmap", "ToolTip"}) {
                GVariant* value = g_variant_lookup_value(dict, key, nullptr);
                CHECK(value != nullptr);
                if (value != nullptr) {
                    g_variant_unref(value);
                }
            }
            GVariant* id = g_variant_lookup_value(dict, "Id", G_VARIANT_TYPE_STRING);
            CHECK(id != nullptr && std::string(g_variant_get_string(id, nullptr)) == "pipeduck");
            if (id != nullptr) {
                g_variant_unref(id);
            }
            GVariant* menu = g_variant_lookup_value(dict, "Menu", G_VARIANT_TYPE_OBJECT_PATH);
            CHECK(menu != nullptr && std::string(g_variant_get_string(menu, nullptr)) == kMenuPath);
            if (menu != nullptr) {
                g_variant_unref(menu);
            }
            g_variant_unref(dict);
            g_variant_unref(properties);
        }

        uint32_t revision = 0;
        GVariant* layout = get_layout(service, &revision);
        CHECK(layout != nullptr);
        if (layout != nullptr) {
            int32_t root_id = -1;
            GVariant* root_props = nullptr;
            GVariant* children = nullptr;
            g_variant_get(layout, "(i@a{sv}@av)", &root_id, &root_props, &children);
            CHECK(root_id == 0);
            CHECK(g_variant_n_children(children) > 0);
            g_variant_unref(children);
            g_variant_unref(root_props);
            g_variant_unref(layout);
        }

        // Lazy submenu fetches must return the same populated subtrees.
        for (const int parent : {20, 30, 40}) {
            uint32_t submenu_revision = 0;
            GVariant* submenu = get_layout(service, &submenu_revision, parent);
            CHECK(submenu != nullptr);
            if (submenu != nullptr) {
                int32_t submenu_id = -1;
                GVariant* submenu_props = nullptr;
                GVariant* submenu_children = nullptr;
                g_variant_get(submenu, "(i@a{sv}@av)", &submenu_id, &submenu_props,
                              &submenu_children);
                CHECK(submenu_id == parent);
                CHECK(g_variant_n_children(submenu_children) > 0);
                g_variant_unref(submenu_children);
                g_variant_unref(submenu_props);
                g_variant_unref(submenu);
            }
        }

        // GetLayout must accept the third propertyNames argument and AboutToShow
        // must return false so hosts do not rebuild the menu and close submenus.
        GVariant* about_root = call(service, kMenuPath, kMenuInterface, "AboutToShow",
                                    g_variant_new("(i)", 0), G_VARIANT_TYPE("(b)"));
        if (about_root != nullptr) {
            gboolean need_update = TRUE;
            g_variant_get(about_root, "(b)", &need_update);
            CHECK(need_update == FALSE);
            g_variant_unref(about_root);
        }
        GVariant* about_submenu = call(service, kMenuPath, kMenuInterface, "AboutToShow",
                                       g_variant_new("(i)", 20), G_VARIANT_TYPE("(b)"));
        if (about_submenu != nullptr) {
            gboolean need_update = TRUE;
            g_variant_get(about_submenu, "(b)", &need_update);
            CHECK(need_update == FALSE);
            g_variant_unref(about_submenu);
        }

        // Opening a submenu must not request a layout rebuild.
        uint32_t before = 0;
        uint32_t after = 0;
        GVariant* first = get_layout(service, &before);
        if (first != nullptr) {
            g_variant_unref(first);
        }
        GVariant* about_again = call(service, kMenuPath, kMenuInterface, "AboutToShow",
                                     g_variant_new("(i)", 20), G_VARIANT_TYPE("(b)"));
        if (about_again != nullptr) {
            g_variant_unref(about_again);
        }
        GVariant* second = get_layout(service, &after);
        if (second != nullptr) {
            g_variant_unref(second);
        }
        CHECK(before == after);

        GVariantBuilder ids;
        g_variant_builder_init(&ids, G_VARIANT_TYPE("ai"));
        g_variant_builder_add(&ids, "i", 0);
        g_variant_builder_add(&ids, "i", 10);
        g_variant_builder_add(&ids, "i", 21);
        GVariant* group =
            call(service, kMenuPath, kMenuInterface, "GetGroupProperties",
                 g_variant_new("(@ai@as)", g_variant_builder_end(&ids), empty_string_array()),
                 G_VARIANT_TYPE("(a(ia{sv}))"));
        if (group != nullptr) {
            GVariant* entries = nullptr;
            g_variant_get(group, "(@a(ia{sv}))", &entries);
            CHECK(g_variant_n_children(entries) == 3);
            g_variant_unref(entries);
            g_variant_unref(group);
        }

        // Clicking "My microphone" must update config.toml.
        click(service, 22);
        CHECK(wait_for_file_text(config_file, "trigger = \"mic\""));

        // Quit through the menu.
        click(service, 60);
    } else {
        std::fprintf(stderr, "FAIL: tray service %s did not appear\n", service.c_str());
        ++g_failures;
        kill(pid, SIGTERM);
    }

    bool exited = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            exited = true;
            break;
        }
        g_usleep(50 * 1000);
    }
    if (!exited) {
        std::fprintf(stderr, "FAIL: tray did not exit after Quit\n");
        ++g_failures;
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
    g_spawn_close_pid(pid);
    g_object_unref(g_bus);
    std::filesystem::remove_all(temporary);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d tray check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All tray integration checks passed\n");
    return 0;
}
