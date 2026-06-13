// eswl-config: a GTK4 configuration GUI for easystroke-wayland.
//
// Carries over the original easystroke "Actions" workflow: a columnar table of
// gestures — Stroke (thumbnail) · Name · Type · Argument — with inline editing,
// a bottom action bar (Add / Delete / Record Stroke), and tabs in the header.
// Styled glass/translucent to match the user's Chiguiro aesthetic. Reads and
// writes the same gestures.json the daemon uses; restart the daemon to apply.

#include "gesture_config.h"
#include "json.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace es;

namespace {

// Action types in dropdown order (easystroke's set). Index 0 = no action.
const char *const kTypeNames[] = {"(none)", "command", "key",    "text",
                                  "button", "scroll",  "ignore", nullptr};

const char *const kTriggerLabels[] = {"1 — left",       "2 — middle",  "3 — right",
                                      "8 — back",        "9 — forward", "10 — pen tip",
                                      "11 — pen button", nullptr};
const int kTriggerButtons[] = {1, 2, 3, 8, 9, 10, 11};

constexpr int kThumbW = 64;
constexpr int kThumbH = 40;
constexpr int kNameW = 180;
constexpr int kTypeW = 120;

// GUI-only appearance settings, persisted next to gestures.json in gui.json.
struct Appearance {
    int window_opacity = 95;              // percent (less transparent by default)
    std::string glass_color = "#14141a";  // window tint
    std::string accent_color = "#3584e4"; // selection / accents
};

struct State {
    std::string config_path;
    std::string gui_path;
    GestureConfig cfg;
    Appearance appearance;
    int selected = -1;
    GtkWidget *window = nullptr;
    GtkWidget *listbox = nullptr;
    GtkWidget *trigger_dropdown = nullptr;
    GtkWidget *status = nullptr;
    GtkCssProvider *dyn_css = nullptr; // appearance overrides, reloaded live
};

void parse_hex(const std::string &hex, int &r, int &g, int &b) {
    r = g = b = 0;
    if (hex.size() >= 7 && hex[0] == '#')
        std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
}

// Per-row context; owned by the row widget and freed with it.
struct RowData {
    State *s;
    int idx;
    GtkWidget *arg;
};

int type_to_index(const std::string &type) {
    for (int i = 0; kTypeNames[i]; ++i)
        if (type == kTypeNames[i])
            return i;
    return 0;
}
std::string index_to_type(guint idx) {
    return idx == 0 ? std::string() : std::string(kTypeNames[idx]);
}
const char *placeholder_for(const std::string &type) {
    if (type == "command")
        return "shell command (e.g. chiguiro)";
    if (type == "key")
        return "key combo (e.g. ctrl+shift+t)";
    if (type == "text")
        return "text to type";
    if (type == "button")
        return "button number";
    if (type == "scroll")
        return "up / down / left / right [count]";
    return "";
}

void set_status(State *s, const std::string &msg) {
    if (s->status)
        gtk_label_set_text(GTK_LABEL(s->status), msg.c_str());
}

// ----- Stroke thumbnail (easystroke's blue→green direction gradient) --------

void draw_thumb(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer data) {
    RowData *rd = static_cast<RowData *>(data);
    if (rd->idx < 0 || rd->idx >= (int)rd->s->cfg.gestures.size())
        return;
    const auto &strokes = rd->s->cfg.gestures[rd->idx].strokes;

    if (strokes.empty() || strokes[0].size() < 2) {
        // Empty marker: a faint dashed dot, like easystroke's empty stroke.
        cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
        cairo_arc(cr, w / 2.0, h / 2.0, 2.5, 0, 2 * M_PI);
        cairo_fill(cr);
        return;
    }
    const std::vector<Point> &pts = strokes[0];
    double minx = pts[0].x, maxx = minx, miny = pts[0].y, maxy = miny;
    for (const Point &p : pts) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    const double pad = 5.0;
    double availw = w - 2 * pad, availh = h - 2 * pad;
    double sw = maxx - minx, sh = maxy - miny;
    double sc = std::min(sw > 1e-6 ? availw / sw : 1e9, sh > 1e-6 ? availh / sh : 1e9);
    double ox = pad + (availw - sw * sc) / 2 - minx * sc;
    double oy = pad + (availh - sh * sc) / 2 - miny * sc;

    cairo_set_line_width(cr, 2.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    std::size_t n = pts.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        double t = static_cast<double>(i) / (n - 1);
        cairo_set_source_rgba(cr, 0.0, t, 1.0 - t, 1.0); // start blue → end green
        cairo_move_to(cr, ox + pts[i].x * sc, oy + pts[i].y * sc);
        cairo_line_to(cr, ox + pts[i + 1].x * sc, oy + pts[i + 1].y * sc);
        cairo_stroke(cr);
    }
}

// ----- Styling --------------------------------------------------------------

void load_css() {
    // Force the dark theme variant so built-in widget colors (entry text,
    // selections, popovers) are light against our dark translucent window.
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);

    static const char *kCss = R"css(
window { background-color: rgba(20, 20, 26, 0.88); color: rgba(255,255,255,0.97); }
headerbar { background: transparent; box-shadow: none; border: none; }
label { text-shadow: 0 1px 2px rgba(0,0,0,0.55); }
box, listbox, list, row, scrolledwindow, viewport, label, stack, grid, separator, stackswitcher {
  background-color: transparent; background-image: none;
}
separator { background-color: rgba(255,255,255,0.10); }
listbox > row { border-radius: 8px; margin: 1px 4px; padding: 2px; }
listbox > row:selected { background-color: color-mix(in srgb, white 13%, transparent); color: inherit; }

/* Flat "cell" inputs: read like text in the table, reveal a border on focus. */
entry.cell, entry.cell > text { color: rgba(255,255,255,0.97); }
entry.cell {
  background: transparent; background-image: none;
  border: 1px solid transparent; border-radius: 5px; box-shadow: none;
  min-height: 26px; padding: 1px 6px;
}
entry.cell:focus-within { border-color: rgba(255,255,255,0.40); background: rgba(255,255,255,0.06); }
entry.cell > text { background: transparent; caret-color: rgba(255,255,255,0.97); }
entry.cell > text > placeholder { color: rgba(255,255,255,0.40); }
entry.cell > text selection { background-color: rgba(110,160,235,0.55); color: #fff; }
dropdown.cell > button {
  background: transparent; background-image: none;
  border: 1px solid transparent; border-radius: 5px; box-shadow: none;
  min-height: 26px; color: rgba(255,255,255,0.97);
}
dropdown.cell > button:hover { border-color: rgba(255,255,255,0.25); }
dropdown.cell > button label { color: rgba(255,255,255,0.97); }

/* Dropdown/menu popovers must be OPAQUE with light text — the transparent
   cascade above would otherwise leave their items unreadable over the desktop. */
popover > contents, popover > arrow {
  background-color: rgb(34, 34, 42);
  border: 1px solid rgba(255,255,255,0.14);
  box-shadow: 0 6px 18px rgba(0,0,0,0.55);
}
popover > contents { border-radius: 9px; padding: 4px; }
popover, popover label, popover row, popover listview, popover .item { color: rgba(255,255,255,0.97); }
popover row:selected, popover row:hover, popover listview > row:hover {
  background-color: rgba(110,160,235,0.45); color: #ffffff; border-radius: 5px;
}

/* Normal (non-cell) entries/dropdowns keep a visible glass border. */
entry:not(.cell) {
  background: transparent; border: 1px solid rgba(255,255,255,0.15);
  border-radius: 6px; min-height: 28px; padding: 2px 8px; color: rgba(255,255,255,0.97);
}
dropdown:not(.cell) > button {
  background: transparent; border: 1px solid rgba(255,255,255,0.15);
  border-radius: 6px; min-height: 28px; color: rgba(255,255,255,0.97);
}

button:not(.close):not(.minimize):not(.maximize):not(.titlebutton) {
  background-color: transparent; background-image: none;
  border: 1px solid rgba(255,255,255,0.15); border-radius: 6px; box-shadow: none;
  min-height: 28px; padding: 2px 12px; color: rgba(255,255,255,0.97);
}
button:not(.close):not(.minimize):not(.maximize):not(.titlebutton):hover {
  border-color: rgba(255,255,255,0.30); background-color: rgba(255,255,255,0.06);
}
button.suggested-action {
  border-color: color-mix(in srgb, @accent_bg_color 70%, transparent); color: @accent_color;
}
spinbutton {
  background: transparent; border: 1px solid rgba(255,255,255,0.15);
  border-radius: 6px; color: rgba(255,255,255,0.97); min-height: 28px;
}
spinbutton > text { color: rgba(255,255,255,0.97); caret-color: rgba(255,255,255,0.97); }
spinbutton > button { color: rgba(255,255,255,0.82); background: transparent; border: none; }
.dim { color: rgba(255,255,255,0.62); }
.colhdr { color: rgba(255,255,255,0.62); font-weight: bold; }

/* Keep text light when the window is unfocused (backdrop) — the theme would
   otherwise dim it to a dark, unreadable color. */
label:backdrop, button:backdrop, button:backdrop label,
entry.cell:backdrop, entry.cell > text:backdrop,
dropdown.cell > button:backdrop, dropdown.cell > button:backdrop label,
stackswitcher button:backdrop, stackswitcher button:backdrop label,
headerbar:backdrop, spinbutton:backdrop, spinbutton:backdrop text {
  color: rgba(255,255,255,0.90);
}
.colhdr:backdrop, .dim:backdrop { color: rgba(255,255,255,0.58); }
window:backdrop { color: rgba(255,255,255,0.92); }
)css";
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, kCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

// Live appearance overrides (window opacity / colors), in a higher-priority
// provider so they win over the static structural CSS above.
void apply_appearance(State *s) {
    if (!s->dyn_css) {
        s->dyn_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(s->dyn_css),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    int gr, gg, gb;
    parse_hex(s->appearance.glass_color, gr, gg, gb);
    double alpha = std::clamp(s->appearance.window_opacity, 0, 100) / 100.0;
    const char *ac = s->appearance.accent_color.c_str();
    char buf[1600];
    std::snprintf(
        buf, sizeof buf,
        "window { background-color: rgba(%d,%d,%d,%.3f); }\n"
        "listbox > row:selected { background-color: color-mix(in srgb, %s 32%%, transparent); }\n"
        "entry.cell:focus-within { border-color: color-mix(in srgb, %s 65%%, transparent); }\n"
        "popover row:selected, popover row:hover { background-color: color-mix(in srgb, %s 50%%, "
        "transparent); }\n"
        "button.suggested-action { border-color: color-mix(in srgb, %s 75%%, transparent); color: "
        "%s; }\n"
        "stackswitcher button:checked { color: %s; }\n",
        gr, gg, gb, alpha, ac, ac, ac, ac, ac, ac);
    gtk_css_provider_load_from_string(s->dyn_css, buf);
}

void load_appearance(State *s) {
    std::ifstream in(s->gui_path);
    if (!in)
        return;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        json::Value root = json::Value::parse(ss.str());
        if (root["window_opacity"].is_number())
            s->appearance.window_opacity = static_cast<int>(root["window_opacity"].as_number());
        if (root["glass_color"].is_string())
            s->appearance.glass_color = root["glass_color"].as_string();
        if (root["accent_color"].is_string())
            s->appearance.accent_color = root["accent_color"].as_string();
    } catch (...) {
    }
}

void save_appearance(State *s) {
    json::Object o;
    o["window_opacity"] = json::Value(s->appearance.window_opacity);
    o["glass_color"] = json::Value(s->appearance.glass_color);
    o["accent_color"] = json::Value(s->appearance.accent_color);
    std::ofstream out(s->gui_path, std::ios::trunc);
    if (out)
        out << json::Value(std::move(o)).dump() << '\n';
}

std::string rgba_to_hex(const GdkRGBA *c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", static_cast<int>(std::lround(c->red * 255)),
                  static_cast<int>(std::lround(c->green * 255)),
                  static_cast<int>(std::lround(c->blue * 255)));
    return b;
}

void on_window_realize(GtkWidget *win, gpointer) {
    // Empty opaque region => the surface is transparent to the compositor, so
    // KWin composites our alpha / blurs behind (same trick as Chiguiro).
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win));
    if (!surface)
        return;
    cairo_region_t *region = cairo_region_create();
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_surface_set_opaque_region(surface, region);
    G_GNUC_END_IGNORE_DEPRECATIONS
    cairo_region_destroy(region);
}

// ----- Row editing ----------------------------------------------------------

void on_name_changed(GtkEditable *e, gpointer d) {
    RowData *rd = static_cast<RowData *>(d);
    rd->s->cfg.gestures[rd->idx].name = gtk_editable_get_text(e);
}
void on_arg_changed(GtkEditable *e, gpointer d) {
    RowData *rd = static_cast<RowData *>(d);
    rd->s->cfg.gestures[rd->idx].argument = gtk_editable_get_text(e);
}
void on_type_changed(GObject *dd, GParamSpec *, gpointer d) {
    RowData *rd = static_cast<RowData *>(d);
    std::string type = index_to_type(gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)));
    rd->s->cfg.gestures[rd->idx].type = type;
    gtk_entry_set_placeholder_text(GTK_ENTRY(rd->arg), placeholder_for(type));
}
void select_row_idx(State *s, int idx) {
    s->selected = idx;
    gtk_list_box_select_row(GTK_LIST_BOX(s->listbox),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->listbox), idx));
}
void on_row_focus_enter(GtkEventControllerFocus *, gpointer d) {
    RowData *rd = static_cast<RowData *>(d);
    select_row_idx(rd->s, rd->idx);
}
void on_row_pressed(GtkGestureClick *, int, double, double, gpointer d) {
    RowData *rd = static_cast<RowData *>(d);
    select_row_idx(rd->s, rd->idx);
}
void on_row_selected(GtkListBox *, GtkListBoxRow *row, gpointer data) {
    static_cast<State *>(data)->selected = row ? gtk_list_box_row_get_index(row) : -1;
}
void free_rowdata(gpointer d) { delete static_cast<RowData *>(d); }

GtkWidget *build_row(State *s, int idx) {
    GestureEntry &g = s->cfg.gestures[idx];
    RowData *rd = new RowData{s, idx, nullptr};

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(box, 2);
    gtk_widget_set_margin_bottom(box, 2);

    GtkWidget *thumb = gtk_drawing_area_new();
    gtk_widget_set_size_request(thumb, kThumbW, kThumbH);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(thumb), draw_thumb, rd, nullptr);
    gtk_box_append(GTK_BOX(box), thumb);

    GtkWidget *name = gtk_entry_new();
    gtk_widget_add_css_class(name, "cell");
    gtk_widget_set_size_request(name, kNameW, -1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(name), "name");
    gtk_editable_set_text(GTK_EDITABLE(name), g.name.c_str());
    g_signal_connect(name, "changed", G_CALLBACK(on_name_changed), rd);
    gtk_box_append(GTK_BOX(box), name);

    GtkWidget *type = gtk_drop_down_new_from_strings(kTypeNames);
    gtk_widget_add_css_class(type, "cell");
    gtk_widget_set_size_request(type, kTypeW, -1);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(type), type_to_index(g.type));
    g_signal_connect(type, "notify::selected", G_CALLBACK(on_type_changed), rd);
    gtk_box_append(GTK_BOX(box), type);

    GtkWidget *arg = gtk_entry_new();
    gtk_widget_add_css_class(arg, "cell");
    gtk_widget_set_hexpand(arg, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(arg), placeholder_for(g.type));
    gtk_editable_set_text(GTK_EDITABLE(arg), g.argument.c_str());
    g_signal_connect(arg, "changed", G_CALLBACK(on_arg_changed), rd);
    gtk_box_append(GTK_BOX(box), arg);
    rd->arg = arg;

    GtkEventController *fc = gtk_event_controller_focus_new();
    g_signal_connect(fc, "enter", G_CALLBACK(on_row_focus_enter), rd);
    gtk_widget_add_controller(box, fc);

    // Clicking anywhere on the row (thumbnail, gaps) selects it too.
    GtkGesture *click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_row_pressed), rd);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));

    g_object_set_data_full(G_OBJECT(box), "rd", rd, free_rowdata);
    return box;
}

void rebuild_list(State *s) {
    while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->listbox), 0))
        gtk_list_box_remove(GTK_LIST_BOX(s->listbox), GTK_WIDGET(row));
    for (int i = 0; i < (int)s->cfg.gestures.size(); ++i)
        gtk_list_box_append(GTK_LIST_BOX(s->listbox), build_row(s, i));
}

void select_index(State *s, int i) {
    if (i >= 0 && i < (int)s->cfg.gestures.size())
        gtk_list_box_select_row(GTK_LIST_BOX(s->listbox),
                                gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->listbox), i));
}

void on_add(GtkButton *, gpointer data) {
    State *s = static_cast<State *>(data);
    GestureEntry g;
    g.name = "new gesture";
    s->cfg.gestures.push_back(g);
    rebuild_list(s);
    select_index(s, (int)s->cfg.gestures.size() - 1);
}

void on_delete(GtkButton *, gpointer data) {
    State *s = static_cast<State *>(data);
    if (s->selected < 0 || s->selected >= (int)s->cfg.gestures.size())
        return;
    int next = s->selected;
    s->cfg.gestures.erase(s->cfg.gestures.begin() + s->selected);
    s->selected = -1;
    rebuild_list(s);
    if (!s->cfg.gestures.empty())
        select_index(s, std::min(next, (int)s->cfg.gestures.size() - 1));
}

void on_save(GtkButton *, gpointer data) {
    State *s = static_cast<State *>(data);
    guint ti = gtk_drop_down_get_selected(GTK_DROP_DOWN(s->trigger_dropdown));
    if (ti < G_N_ELEMENTS(kTriggerButtons))
        s->cfg.trigger_button = static_cast<Button>(kTriggerButtons[ti]);
    try {
        s->cfg.save(s->config_path);
        // Ask a running daemon to reload its config (harmless if none runs).
        g_spawn_command_line_async("pkill -HUP -x eswl-daemon", nullptr);
        set_status(s, "Saved — running daemon reloaded.");
    } catch (const std::exception &e) {
        set_status(s, std::string("Save failed: ") + e.what());
    }
}

void on_opacity_changed(GtkSpinButton *sp, gpointer d) {
    State *s = static_cast<State *>(d);
    s->appearance.window_opacity = static_cast<int>(gtk_spin_button_get_value(sp));
    apply_appearance(s);
    save_appearance(s);
}
void on_glass_color(GObject *btn, GParamSpec *, gpointer d) {
    State *s = static_cast<State *>(d);
    s->appearance.glass_color = rgba_to_hex(gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(btn)));
    apply_appearance(s);
    save_appearance(s);
}
void on_accent_color(GObject *btn, GParamSpec *, gpointer d) {
    State *s = static_cast<State *>(d);
    s->appearance.accent_color = rgba_to_hex(gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(btn)));
    apply_appearance(s);
    save_appearance(s);
}
GtkWidget *make_color_btn(const std::string &hex, GCallback cb, State *s) {
    GtkWidget *btn = gtk_color_dialog_button_new(gtk_color_dialog_new());
    GdkRGBA rgba;
    if (gdk_rgba_parse(&rgba, hex.c_str()))
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(btn), &rgba);
    gtk_widget_set_halign(btn, GTK_ALIGN_START);
    g_signal_connect(btn, "notify::rgba", cb, s);
    return btn;
}

// ----- Record-stroke dialog -------------------------------------------------

struct RecordState {
    State *app;
    GtkWidget *area;
    double start_x, start_y;
    std::vector<Point> pts;
};

void rec_draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer data) {
    RecordState *r = static_cast<RecordState *>(data);
    cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, 0.92);
    cairo_paint(cr);
    if (r->pts.size() < 2) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
        cairo_move_to(cr, 14, h / 2.0);
        cairo_show_text(cr, "Press and drag to draw the gesture");
        return;
    }
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    std::size_t n = r->pts.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        double t = static_cast<double>(i) / (n - 1);
        cairo_set_source_rgba(cr, 0.0, t, 1.0 - t, 1.0);
        cairo_move_to(cr, r->pts[i].x, r->pts[i].y);
        cairo_line_to(cr, r->pts[i + 1].x, r->pts[i + 1].y);
        cairo_stroke(cr);
    }
    (void)w;
}
void rec_begin(GtkGestureDrag *, double x, double y, gpointer data) {
    RecordState *r = static_cast<RecordState *>(data);
    r->start_x = x;
    r->start_y = y;
    r->pts.clear();
    r->pts.push_back({x, y});
    gtk_widget_queue_draw(r->area);
}
void rec_update(GtkGestureDrag *, double ox, double oy, gpointer data) {
    RecordState *r = static_cast<RecordState *>(data);
    r->pts.push_back({r->start_x + ox, r->start_y + oy});
    gtk_widget_queue_draw(r->area);
}

void on_record(GtkButton *, gpointer data) {
    State *s = static_cast<State *>(data);
    if (s->selected < 0)
        return;

    RecordState *r = new RecordState{s, nullptr, 0, 0, {}};
    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Record stroke");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(s->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 480, 360);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);

    GtkWidget *area = gtk_drawing_area_new();
    r->area = area;
    gtk_widget_set_vexpand(area, TRUE);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), rec_draw, r, nullptr);
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(rec_begin), r);
    g_signal_connect(drag, "drag-update", G_CALLBACK(rec_update), r);
    g_signal_connect(drag, "drag-end", G_CALLBACK(rec_update), r);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *ok = gtk_button_new_with_label("Save stroke");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), ok);

    gtk_box_append(GTK_BOX(box), area);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(GTK_WINDOW(dlg), box);

    g_signal_connect_data(dlg, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer) {}), r,
                          +[](gpointer d, GClosure *) { delete static_cast<RecordState *>(d); },
                          GConnectFlags(0));
    g_signal_connect(cancel, "clicked",
                     G_CALLBACK(+[](GtkButton *, gpointer d) { gtk_window_destroy(GTK_WINDOW(d)); }),
                     dlg);

    struct OkCtx {
        RecordState *r;
        GtkWidget *dlg;
    };
    OkCtx *ctx = new OkCtx{r, dlg};
    g_signal_connect_data(
        ok, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            OkCtx *c = static_cast<OkCtx *>(d);
            State *st = c->r->app;
            if (c->r->pts.size() > 2 && st->selected >= 0) {
                int sel = st->selected;
                st->cfg.gestures[sel].strokes.push_back(c->r->pts);
                rebuild_list(st);
                select_index(st, sel);
            }
            gtk_window_destroy(GTK_WINDOW(c->dlg));
        }),
        ctx, +[](gpointer d, GClosure *) { delete static_cast<OkCtx *>(d); }, GConnectFlags(0));

    gtk_window_present(GTK_WINDOW(dlg));
}

// ----- Window construction --------------------------------------------------

GtkWidget *col_header() {
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hdr, 10);
    gtk_widget_set_margin_end(hdr, 10);
    gtk_widget_set_margin_top(hdr, 4);
    gtk_widget_set_margin_bottom(hdr, 2);
    auto add = [&](const char *text, int width, bool expand) {
        GtkWidget *l = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0);
        gtk_widget_add_css_class(l, "colhdr");
        if (width > 0)
            gtk_widget_set_size_request(l, width, -1);
        gtk_widget_set_hexpand(l, expand);
        gtk_box_append(GTK_BOX(hdr), l);
    };
    add("Stroke", kThumbW, false);
    add("Name", kNameW, false);
    add("Type", kTypeW, false);
    add("Argument", -1, true);
    return hdr;
}

GtkWidget *build_actions_page(State *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(page, 6);
    gtk_widget_set_margin_end(page, 6);
    gtk_widget_set_margin_top(page, 6);
    gtk_widget_set_margin_bottom(page, 6);

    gtk_box_append(GTK_BOX(page), col_header());
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    s->listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s->listbox), GTK_SELECTION_SINGLE);
    g_signal_connect(s->listbox, "row-selected", G_CALLBACK(on_row_selected), s);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), s->listbox);
    gtk_box_append(GTK_BOX(page), scroll);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 4);
    GtkWidget *add = gtk_button_new_with_label("Add Action");
    GtkWidget *del = gtk_button_new_with_label("Delete Action");
    GtkWidget *rec = gtk_button_new_with_label("Record Stroke");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add), s);
    g_signal_connect(del, "clicked", G_CALLBACK(on_delete), s);
    g_signal_connect(rec, "clicked", G_CALLBACK(on_record), s);
    gtk_box_append(GTK_BOX(bar), add);
    gtk_box_append(GTK_BOX(bar), del);
    gtk_box_append(GTK_BOX(bar), rec);
    gtk_box_append(GTK_BOX(page), bar);

    return page;
}

GtkWidget *build_prefs_page(State *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 16);

    GtkWidget *l = gtk_label_new("Trigger button");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_add_css_class(l, "dim");
    gtk_box_append(GTK_BOX(page), l);

    s->trigger_dropdown = gtk_drop_down_new_from_strings(kTriggerLabels);
    gtk_widget_set_halign(s->trigger_dropdown, GTK_ALIGN_START);
    for (guint i = 0; i < G_N_ELEMENTS(kTriggerButtons); ++i)
        if (kTriggerButtons[i] == (int)s->cfg.trigger_button)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(s->trigger_dropdown), i);
    gtk_box_append(GTK_BOX(page), s->trigger_dropdown);

    GtkWidget *note = gtk_label_new(
        "The button held to draw a gesture. On a stylus, 11 is the side button.\n"
        "Restart the daemon after saving for changes to take effect.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0);
    gtk_widget_add_css_class(note, "dim");
    gtk_widget_set_margin_top(note, 8);
    gtk_box_append(GTK_BOX(page), note);

    // --- Appearance (glass settings, like Chiguiro) ---------------------
    GtkWidget *ah = gtk_label_new("Appearance");
    gtk_label_set_xalign(GTK_LABEL(ah), 0.0);
    gtk_widget_add_css_class(ah, "colhdr");
    gtk_widget_set_margin_top(ah, 22);
    gtk_box_append(GTK_BOX(page), ah);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_widget_set_halign(grid, GTK_ALIGN_START);
    gtk_widget_set_margin_top(grid, 6);

    auto row_label = [&](const char *text, int r) {
        GtkWidget *l2 = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l2), 0.0);
        gtk_widget_set_size_request(l2, 160, -1);
        gtk_grid_attach(GTK_GRID(grid), l2, 0, r, 1, 1);
    };

    row_label("Window Opacity", 0);
    GtkWidget *spin = gtk_spin_button_new_with_range(20, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), s->appearance.window_opacity);
    gtk_widget_set_halign(spin, GTK_ALIGN_START);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_opacity_changed), s);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, 0, 1, 1);

    row_label("Glass Color", 1);
    gtk_grid_attach(GTK_GRID(grid),
                    make_color_btn(s->appearance.glass_color, G_CALLBACK(on_glass_color), s), 1, 1,
                    1, 1);

    row_label("Accent Color", 2);
    gtk_grid_attach(GTK_GRID(grid),
                    make_color_btn(s->appearance.accent_color, G_CALLBACK(on_accent_color), s), 1, 2,
                    1, 1);

    gtk_box_append(GTK_BOX(page), grid);
    return page;
}

void on_activate(GtkApplication *app, gpointer data) {
    State *s = static_cast<State *>(data);
    load_css();
    apply_appearance(s);

    s->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(s->window), "WeazyStroke");
    gtk_window_set_default_size(GTK_WINDOW(s->window), 720, 500);
    g_signal_connect(s->window, "realize", G_CALLBACK(on_window_realize), nullptr);

    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_stack_add_titled(GTK_STACK(stack), build_actions_page(s), "actions", "Actions");
    gtk_stack_add_titled(GTK_STACK(stack), build_prefs_page(s), "preferences", "Preferences");

    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));

    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(save, "clicked", G_CALLBACK(on_save), s);

    // Tabs + Save live in the window body (an in-content toolbar), not the
    // window decoration.
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(toolbar, 8);
    gtk_widget_set_margin_end(toolbar, 8);
    gtk_widget_set_margin_top(toolbar, 8);
    gtk_widget_set_margin_bottom(toolbar, 4);
    gtk_box_append(GTK_BOX(toolbar), switcher);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar), spacer);
    gtk_box_append(GTK_BOX(toolbar), save);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), toolbar);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(root), stack);
    s->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s->status), 0.0);
    gtk_widget_add_css_class(s->status, "dim");
    gtk_widget_set_margin_start(s->status, 10);
    gtk_widget_set_margin_bottom(s->status, 4);
    gtk_box_append(GTK_BOX(root), s->status);
    gtk_window_set_child(GTK_WINDOW(s->window), root);

    rebuild_list(s);
    select_index(s, 0);
    gtk_window_present(GTK_WINDOW(s->window));
}

} // namespace

int main(int argc, char **argv) {
    State state;
    state.config_path = GestureConfig::default_path();
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--config" && i + 1 < argc)
            state.config_path = argv[++i];

    auto slash = state.config_path.find_last_of('/');
    state.gui_path =
        (slash == std::string::npos ? std::string() : state.config_path.substr(0, slash + 1)) +
        "gui.json";

    try {
        state.cfg = GestureConfig::load(state.config_path);
    } catch (const std::exception &e) {
        g_printerr("warning: could not load %s: %s\n", state.config_path.c_str(), e.what());
    }
    load_appearance(&state);

    GtkApplication *app =
        gtk_application_new("org.weazystroke.Config", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return status;
}
