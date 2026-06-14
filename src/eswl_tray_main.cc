// WeazyStroke system-tray helper.
//
// A standalone StatusNotifierItem (the freedesktop/KDE tray protocol) over
// DBus, plus its DBusMenu. The daemon spawns this as a child and pushes the
// enable/disable state down our stdin ("E"/"D" lines); we push the user's menu
// actions back up as signals to the daemon (our parent): SIGUSR1 toggles
// enable, SIGTERM quits. Preferences is launched directly (sibling binary).
//
// No GTK: just GIO/GDBus, so it stays light and works natively on KDE Plasma
// Wayland (and any SNI host).

#include <gio/gio.h>
#include <glib-unix.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <csignal>
#include <initializer_list>

namespace {

GMainLoop *g_loop = nullptr;
GDBusConnection *g_conn = nullptr;
gchar *g_service_name = nullptr; // our owned bus name, registered with the watcher
bool g_enabled = true;           // mirrors the daemon's run state
guint g_menu_revision = 1;

constexpr const char *kIconName = "weazystroke";
constexpr const char *kSniPath = "/StatusNotifierItem";
constexpr const char *kMenuPath = "/MenuBar";

// ----- Directory of our own executable (to find the sibling eswl-config) -----
gchar *self_dir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return g_strdup(".");
    buf[n] = '\0';
    return g_path_get_dirname(buf);
}

// Launch the (single-instance) config GUI. With toggle=true the running window
// is shown/hidden; otherwise it is always presented. GApplication routes the
// spawn to the one primary instance, so this never stacks windows.
void launch_config(bool toggle) {
    gchar *dir = self_dir();
    gchar *exe = g_build_filename(dir, "eswl-config", nullptr);
    char *argv_toggle[] = {exe, const_cast<char *>("--toggle"), nullptr};
    char *argv_plain[] = {exe, nullptr};
    g_spawn_async(nullptr, toggle ? argv_toggle : argv_plain, nullptr, G_SPAWN_DEFAULT, nullptr,
                  nullptr, nullptr, nullptr);
    g_free(exe);
    g_free(dir);
}

// ===================== DBusMenu (com.canonical.dbusmenu) =====================
//
// A tiny static menu: id 1 "Enabled" (checkmark), 2 separator, 3 "Preferences",
// 4 "Quit". id 0 is the invisible root container.

GVariant *item_props(int id) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    switch (id) {
    case 0:
        g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
        break;
    case 1:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Enabled"));
        g_variant_builder_add(&b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
        g_variant_builder_add(&b, "{sv}", "toggle-state", g_variant_new_int32(g_enabled ? 1 : 0));
        break;
    case 2:
        g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
        break;
    case 3:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Preferences…"));
        break;
    case 4:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Quit"));
        break;
    default:
        break;
    }
    return g_variant_builder_end(&b);
}

// Build a (id, props, children) layout node. Only the root (id 0) has children.
GVariant *build_item(int id, bool with_children) {
    GVariantBuilder ch;
    g_variant_builder_init(&ch, G_VARIANT_TYPE("av"));
    if (with_children && id == 0)
        for (int c : {1, 2, 3, 4})
            g_variant_builder_add(&ch, "v", build_item(c, false));
    return g_variant_new("(i@a{sv}av)", id, item_props(id), &ch);
}

void emit_menu_update() {
    if (!g_conn)
        return;
    // The "Enabled" checkmark (id 1) changed.
    GVariantBuilder upd;
    g_variant_builder_init(&upd, G_VARIANT_TYPE("a(ia{sv})"));
    g_variant_builder_add(&upd, "(i@a{sv})", 1, item_props(1));
    GVariantBuilder rem;
    g_variant_builder_init(&rem, G_VARIANT_TYPE("a(ias)"));
    g_dbus_connection_emit_signal(g_conn, nullptr, kMenuPath, "com.canonical.dbusmenu",
                                  "ItemsPropertiesUpdated",
                                  g_variant_new("(a(ia{sv})a(ias))", &upd, &rem), nullptr);
}

void emit_sni_tooltip() {
    if (g_conn)
        g_dbus_connection_emit_signal(g_conn, nullptr, kSniPath, "org.kde.StatusNotifierItem",
                                      "NewToolTip", nullptr, nullptr);
}

void set_enabled(bool en) {
    if (en == g_enabled)
        return;
    g_enabled = en;
    emit_menu_update();
    emit_sni_tooltip();
}

void on_menu_clicked(int id) {
    switch (id) {
    case 1: // toggle; the daemon echoes the new state back down our stdin
        ::kill(::getppid(), SIGUSR1);
        break;
    case 3:
        launch_config(false); // Preferences: always open
        break;
    case 4:
        ::kill(::getppid(), SIGTERM);
        if (g_loop)
            g_main_loop_quit(g_loop);
        break;
    default:
        break;
    }
}

void menu_method(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method,
                 GVariant *params, GDBusMethodInvocation *inv, gpointer) {
    if (!g_strcmp0(method, "GetLayout")) {
        gint parent = 0, depth = 0;
        GVariant *pn = nullptr;
        g_variant_get(params, "(ii@as)", &parent, &depth, &pn);
        if (pn)
            g_variant_unref(pn);
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(u@(ia{sv}av))", g_menu_revision, build_item(parent, true)));
    } else if (!g_strcmp0(method, "GetGroupProperties")) {
        GVariant *ids_v = nullptr, *pn = nullptr;
        g_variant_get(params, "(@ai@as)", &ids_v, &pn);
        GVariantBuilder out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        gsize nids = 0;
        const gint32 *ids =
            static_cast<const gint32 *>(g_variant_get_fixed_array(ids_v, &nids, sizeof(gint32)));
        for (gsize k = 0; k < nids; ++k)
            g_variant_builder_add(&out, "(i@a{sv})", ids[k], item_props(ids[k]));
        g_variant_unref(ids_v);
        if (pn)
            g_variant_unref(pn);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ia{sv}))", &out));
    } else if (!g_strcmp0(method, "GetProperty")) {
        gint id = 0;
        const gchar *name = nullptr;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariant *props = g_variant_ref_sink(item_props(id));
        GVariant *val = g_variant_lookup_value(props, name, nullptr);
        if (!val)
            val = g_variant_new_string("");
        val = g_variant_ref_sink(val);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", val));
        g_variant_unref(props);
        g_variant_unref(val);
    } else if (!g_strcmp0(method, "Event")) {
        gint id = 0;
        const gchar *event = nullptr;
        GVariant *data = nullptr;
        guint ts = 0;
        g_variant_get(params, "(i&svu)", &id, &event, &data, &ts);
        if (event && !g_strcmp0(event, "clicked"))
            on_menu_clicked(id);
        if (data)
            g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, nullptr);
    } else if (!g_strcmp0(method, "EventGroup")) {
        GVariantBuilder errs;
        g_variant_builder_init(&errs, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ai)", &errs));
    } else if (!g_strcmp0(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else {
        g_dbus_method_invocation_return_value(inv, nullptr);
    }
}

GVariant *menu_get_prop(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                        const gchar *prop, GError **, gpointer) {
    if (!g_strcmp0(prop, "Version"))
        return g_variant_new_uint32(3);
    if (!g_strcmp0(prop, "Status"))
        return g_variant_new_string("normal");
    if (!g_strcmp0(prop, "TextDirection"))
        return g_variant_new_string("ltr");
    if (!g_strcmp0(prop, "IconThemePath")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        return g_variant_builder_end(&b);
    }
    return nullptr;
}

// ===================== StatusNotifierItem (org.kde) ==========================

void sni_method(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method,
                GVariant *, GDBusMethodInvocation *inv, gpointer) {
    if (!g_strcmp0(method, "Activate"))
        launch_config(true); // left click -> show/hide the window
    else if (!g_strcmp0(method, "SecondaryActivate"))
        ::kill(::getppid(), SIGUSR1); // middle click -> toggle enable
    // ContextMenu / Scroll: the host drives the menu via the Menu property.
    g_dbus_method_invocation_return_value(inv, nullptr);
}

GVariant *sni_get_prop(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                       const gchar *prop, GError **, gpointer) {
    if (!g_strcmp0(prop, "Category"))
        return g_variant_new_string("ApplicationStatus");
    if (!g_strcmp0(prop, "Id"))
        return g_variant_new_string("weazystroke");
    if (!g_strcmp0(prop, "Title"))
        return g_variant_new_string("WeazyStroke");
    if (!g_strcmp0(prop, "Status"))
        return g_variant_new_string("Active");
    if (!g_strcmp0(prop, "WindowId"))
        return g_variant_new_uint32(0);
    if (!g_strcmp0(prop, "IconName"))
        return g_variant_new_string(kIconName);
    if (!g_strcmp0(prop, "IconThemePath"))
        return g_variant_new_string("");
    if (!g_strcmp0(prop, "Menu"))
        return g_variant_new_object_path(kMenuPath);
    if (!g_strcmp0(prop, "ItemIsMenu"))
        return g_variant_new_boolean(FALSE);
    if (!g_strcmp0(prop, "ToolTip")) {
        GVariantBuilder px;
        g_variant_builder_init(&px, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_new("(sa(iiay)ss)", kIconName, &px, "WeazyStroke",
                             g_enabled ? "Gesture daemon — enabled" : "Gesture daemon — paused");
    }
    return nullptr;
}

const char *kSniXml = R"XML(<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="u" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewIcon"/>
    <signal name="NewTitle"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
</node>)XML";

const char *kMenuXml = R"XML(<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
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
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parent" type="i"/>
    </signal>
  </interface>
</node>)XML";

const GDBusInterfaceVTable kSniVtable = {sni_method, sni_get_prop, nullptr, {}};
const GDBusInterfaceVTable kMenuVtable = {menu_method, menu_get_prop, nullptr, {}};

void register_with_watcher() {
    if (!g_conn || !g_service_name)
        return;
    g_dbus_connection_call(g_conn, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           g_variant_new("(s)", g_service_name), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                           nullptr, nullptr, nullptr);
}

void on_bus_acquired(GDBusConnection *conn, const gchar *, gpointer) {
    g_conn = conn;
    GDBusNodeInfo *sni = g_dbus_node_info_new_for_xml(kSniXml, nullptr);
    GDBusNodeInfo *menu = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
    if (sni)
        g_dbus_connection_register_object(conn, kSniPath, sni->interfaces[0], &kSniVtable, nullptr,
                                          nullptr, nullptr);
    if (menu)
        g_dbus_connection_register_object(conn, kMenuPath, menu->interfaces[0], &kMenuVtable,
                                          nullptr, nullptr, nullptr);
    // Node infos are kept for the process lifetime (referenced by the registry).
}

void on_name_acquired(GDBusConnection *, const gchar *, gpointer) { register_with_watcher(); }
void on_name_lost(GDBusConnection *, const gchar *, gpointer) {}

// Re-register whenever the watcher (re)appears, e.g. after a plasmashell restart.
void on_watcher_up(GDBusConnection *, const gchar *, const gchar *, gpointer) {
    register_with_watcher();
}

// ----- stdin from the daemon: "E"/"D" lines toggle the mirrored state --------
gboolean on_stdin(GIOChannel *ch, GIOCondition cond, gpointer) {
    if (cond & G_IO_HUP) { // daemon closed the pipe / exited
        if (g_loop)
            g_main_loop_quit(g_loop);
        return G_SOURCE_REMOVE;
    }
    gchar *line = nullptr;
    gsize len = 0;
    GIOStatus st = g_io_channel_read_line(ch, &line, &len, nullptr, nullptr);
    if (st == G_IO_STATUS_NORMAL && line && len > 0) {
        if (line[0] == 'E')
            set_enabled(true);
        else if (line[0] == 'D')
            set_enabled(false);
    }
    g_free(line);
    if (st == G_IO_STATUS_EOF) {
        if (g_loop)
            g_main_loop_quit(g_loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean on_quit_signal(gpointer) {
    if (g_loop)
        g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

// Single-instance: hold an exclusive advisory lock for the process lifetime, so
// a second tray exits immediately instead of stacking a duplicate icon.
int acquire_tray_lock() {
    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    gchar *path = g_build_filename(rt && *rt ? rt : "/tmp", "weazystroke-tray.lock", nullptr);
    int fd = ::open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    g_free(path);
    if (fd < 0)
        return -1;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main() {
    if (acquire_tray_lock() < 0)
        return 0; // another tray already running; fd held for the process lifetime

    // Die with the daemon even if it crashes without reaping us.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    g_loop = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGINT, on_quit_signal, nullptr);
    g_unix_signal_add(SIGTERM, on_quit_signal, nullptr);

    GIOChannel *in = g_io_channel_unix_new(STDIN_FILENO);
    g_io_add_watch(in, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP), on_stdin, nullptr);
    g_io_channel_unref(in);

    // Spec-recommended unique name: org.kde.StatusNotifierItem-<pid>-<nr>.
    g_service_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", ::getpid());
    g_bus_own_name(G_BUS_TYPE_SESSION, g_service_name, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired,
                   on_name_acquired, on_name_lost, nullptr, nullptr);
    g_bus_watch_name(G_BUS_TYPE_SESSION, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE,
                     on_watcher_up, nullptr, nullptr, nullptr);

    g_main_loop_run(g_loop);
    return 0;
}
