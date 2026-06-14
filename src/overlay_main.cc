// eswl-overlay: a transient, click-through, full-screen layer-shell window that
// draws the live gesture trail. Driven over stdin by the input daemon:
//   B            begin a new stroke (clear)
//   P <x> <y>    append a point (in the daemon's screen-pixel space)
//   E            end the stroke (clear the trail)
// EOF on stdin or SIGTERM exits. Kept in its own process so the input engine
// links no GUI toolkit and an overlay crash can't take down input capture.

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include <glib-unix.h>

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

struct Overlay {
    GtkWidget *area = nullptr;
    GMainLoop *loop = nullptr;
    std::vector<double> xs; // points in the daemon's screen space
    std::vector<double> ys;
    double screen_w = 1920.0;
    double screen_h = 1080.0;
    double width = 4.0;     // trail line width (px), set via the "W" command
    int effect = 0;         // 0 plain, 1 glow, 2 sparkle (set via the "F" command)
    int fade_ms = 380;      // completion fade-out duration (set via the "D" command)
    std::string inbuf;      // accumulates partial stdin lines
    std::string osd;        // matched-gesture name to flash (empty = none)
    guint osd_timer = 0;    // timeout source clearing the OSD
    // Completion fade-out: on stroke end the trail animates away instead of
    // blanking. `fade` goes 0→1 over the animation, driven by a tick callback.
    bool fading = false;
    double fade = 0.0;
    gint64 fade_start = 0;
    guint fade_tick = 0;
};

void draw_cb(GtkDrawingArea *, cairo_t *cr, int width, int height, gpointer data) {
    Overlay *o = static_cast<Overlay *>(data);

    // Start from a fully transparent surface.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (o->xs.size() >= 2) {
        const double sx = width / o->screen_w;
        const double sy = height / o->screen_h;
        const std::size_t n = o->xs.size();

        // Completion: the trail's start retracts into the end point — the line
        // un-draws itself. i0 advances 0 → n-1 over the animation (smoothstep).
        std::size_t i0 = 0;
        if (o->fading) {
            double e = o->fade * o->fade * (3.0 - 2.0 * o->fade);
            i0 = static_cast<std::size_t>(e * (n - 1));
            if (i0 >= n)
                i0 = n - 1;
        }
        {
            // Line-based effects. Plain is a single smooth blue→green gradient
            // line (start blue, end green), matching easystroke's Stroke::draw;
            // Glow adds two wide faint underlays first for a soft bloom.
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            // Catmull-Rom smoothing: draw a cubic curve through each pair of
            // points (control points from the neighbours), so the trail flows
            // instead of faceting between raw samples.
            auto px = [&](std::size_t i) { return o->xs[i] * sx; };
            auto py = [&](std::size_t i) { return o->ys[i] * sy; };
            const int passes = o->effect == 1 ? 3 : 1;
            const double wmul[3] = {4.6, 2.4, 1.0};
            const double walpha[3] = {0.10, 0.20, 1.0};
            for (int p = passes - 1; p >= 0; --p) {
                int idx = o->effect == 1 ? p : 2;
                cairo_set_line_width(cr, o->width * wmul[idx]);
                for (std::size_t i = i0; i + 1 < n; ++i) {
                    std::size_t ip = i > i0 ? i - 1 : i;        // previous (clamped)
                    std::size_t in = i + 2 < n ? i + 2 : i + 1; // next-next (clamped)
                    double c1x = px(i) + (px(i + 1) - px(ip)) / 6.0;
                    double c1y = py(i) + (py(i + 1) - py(ip)) / 6.0;
                    double c2x = px(i + 1) - (px(in) - px(i)) / 6.0;
                    double c2y = py(i + 1) - (py(in) - py(i)) / 6.0;
                    double t = static_cast<double>(i) / (n - 1);
                    cairo_set_source_rgba(cr, 0.0, t, 1.0 - t, walpha[idx]);
                    cairo_move_to(cr, px(i), py(i));
                    cairo_curve_to(cr, c1x, c1y, c2x, c2y, px(i + 1), py(i + 1));
                    cairo_stroke(cr);
                }
            }
            if (o->effect == 2) {
                // Sparkle: sparse hard square pixels (white/green) with discrete
                // sizes that step down to nothing as the transition completes.
                cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
                for (std::size_t i = i0; i < n; i += 2) {
                    double age = n > 1 ? static_cast<double>(n - 1 - i) / (n - 1) : 0.0;
                    bool front = i0 > 0 && i < i0 + 4;
                    if (!front && age > 0.4)
                        continue;
                    unsigned h = static_cast<unsigned>(i) * 2654435761u + 12345u;
                    if ((h & 1u) == 0u)
                        continue; // ~half off -> sparse
                    int base = 3 + static_cast<int>((h >> 8) & 1u); // 3 or 4 px
                    int sz = o->fading ? base - static_cast<int>(o->fade * (base + 1)) : base;
                    if (sz < 1)
                        continue;
                    double jx = static_cast<double>(static_cast<int>((h >> 1) & 7u) - 3) * 2.0;
                    double jy = static_cast<double>(static_cast<int>((h >> 4) & 7u) - 3) * 2.0;
                    if ((h >> 10) & 1u)
                        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0); // white
                    else
                        cairo_set_source_rgba(cr, 0.3, 1.0, 0.55, 1.0); // green
                    cairo_rectangle(cr, o->xs[i] * sx + jx, o->ys[i] * sy + jy, sz, sz);
                    cairo_fill(cr);
                }
            }
        }
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT); // restore for OSD text
    }

    if (!o->osd.empty()) {
        double fs = height * 0.020;
        if (fs < 20.0)
            fs = 20.0;
        if (fs > 32.0)
            fs = 32.0;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_text_extents_t te;
        cairo_text_extents(cr, o->osd.c_str(), &te);
        double pad = fs * 0.7;
        double bw = te.width + 2 * pad, bh = te.height + pad;
        // small pill, centered near the bottom of the screen
        double bx = (width - bw) / 2.0, by = height - bh - height * 0.06, r = bh / 2.0;
        cairo_new_sub_path(cr); // rounded-pill background
        cairo_arc(cr, bx + bw - r, by + r, r, -M_PI / 2, M_PI / 2);
        cairo_arc(cr, bx + r, by + r, r, M_PI / 2, 3 * M_PI / 2);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.80);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.96);
        cairo_move_to(cr, bx + pad - te.x_bearing, by + pad / 2.0 - te.y_bearing);
        cairo_show_text(cr, o->osd.c_str());
    }
}

void cancel_fade(Overlay *o) {
    if (o->fade_tick && o->area)
        gtk_widget_remove_tick_callback(o->area, o->fade_tick);
    o->fade_tick = 0;
    o->fading = false;
    o->fade = 0.0;
}

gboolean fade_cb(GtkWidget *, GdkFrameClock *, gpointer data) {
    Overlay *o = static_cast<Overlay *>(data);
    const double dur_us = o->fade_ms * 1000.0;
    double p = static_cast<double>(g_get_monotonic_time() - o->fade_start) / dur_us;
    if (p >= 1.0) {
        o->fading = false;
        o->fade = 0.0;
        o->fade_tick = 0;
        o->xs.clear();
        o->ys.clear();
        if (o->area)
            gtk_widget_queue_draw(o->area);
        return G_SOURCE_REMOVE;
    }
    o->fade = p;
    if (o->area)
        gtk_widget_queue_draw(o->area);
    return G_SOURCE_CONTINUE;
}

// Begin the completion fade-out (or clear immediately if there's nothing to show).
void start_fade(Overlay *o) {
    cancel_fade(o);
    if (o->xs.size() < 2 || !o->area || o->fade_ms <= 0) {
        o->xs.clear();
        o->ys.clear();
        if (o->area)
            gtk_widget_queue_draw(o->area);
        return;
    }
    o->fading = true;
    o->fade = 0.0;
    o->fade_start = g_get_monotonic_time();
    o->fade_tick = gtk_widget_add_tick_callback(o->area, fade_cb, o, nullptr);
}

gboolean osd_clear(gpointer data) {
    Overlay *o = static_cast<Overlay *>(data);
    o->osd.clear();
    o->osd_timer = 0;
    if (o->area)
        gtk_widget_queue_draw(o->area);
    return G_SOURCE_REMOVE;
}

void process_line(Overlay *o, const std::string &line) {
    if (line.empty())
        return;
    switch (line[0]) {
    case 'B': // begin: cancel any fade, start a fresh trail
        cancel_fade(o);
        o->xs.clear();
        o->ys.clear();
        break;
    case 'E': // end: animate the trail out (start_fade clears when finished)
        start_fade(o);
        return;
    case 'P': {
        double x = 0, y = 0;
        if (std::sscanf(line.c_str() + 1, "%lf %lf", &x, &y) == 2) {
            o->xs.push_back(x);
            o->ys.push_back(y);
        }
        break;
    }
    case 'W': { // set trail width
        double w = 0;
        if (std::sscanf(line.c_str() + 1, "%lf", &w) == 1 && w > 0)
            o->width = w;
        return; // no redraw needed
    }
    case 'F': { // set trail effect: 0 plain, 1 glow, 2 sparkle
        int e = 0;
        if (std::sscanf(line.c_str() + 1, "%d", &e) == 1)
            o->effect = e;
        return;
    }
    case 'D': { // set completion fade-out duration (ms)
        int d = 0;
        if (std::sscanf(line.c_str() + 1, "%d", &d) == 1 && d >= 0)
            o->fade_ms = d;
        return;
    }
    case 'O': { // flash gesture name (OSD)
        o->osd = line.substr(1);
        if (!o->osd.empty() && o->osd[0] == ' ')
            o->osd.erase(0, 1);
        if (o->osd_timer)
            g_source_remove(o->osd_timer);
        o->osd_timer = g_timeout_add(1100, osd_clear, o);
        break;
    }
    default:
        return;
    }
    if (o->area)
        gtk_widget_queue_draw(o->area);
}

gboolean stdin_cb(gint fd, GIOCondition cond, gpointer data) {
    Overlay *o = static_cast<Overlay *>(data);
    char buf[4096];
    bool eof = false;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            o->inbuf.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (errno == EINTR)
            continue;
        break; // EAGAIN/EWOULDBLOCK: drained for now
    }

    std::size_t pos;
    while ((pos = o->inbuf.find('\n')) != std::string::npos) {
        process_line(o, o->inbuf.substr(0, pos));
        o->inbuf.erase(0, pos + 1);
    }

    if (eof || (cond & G_IO_HUP)) {
        g_main_loop_quit(o->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean on_term(gpointer data) {
    g_main_loop_quit(static_cast<Overlay *>(data)->loop);
    return G_SOURCE_REMOVE;
}

void on_realize(GtkWidget *window, gpointer) {
    // Empty input region => the overlay never captures pointer/pen input; the
    // compositor routes it to whatever is underneath (click-through).
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(window));
    if (surf) {
        cairo_region_t *empty = cairo_region_create();
        gdk_surface_set_input_region(surf, empty);
        cairo_region_destroy(empty);
    }
}

} // namespace

int main(int argc, char **argv) {
    Overlay o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            int w = 1920, h = 1080;
            std::sscanf(argv[++i], "%dx%d", &w, &h);
            o.screen_w = w;
            o.screen_h = h;
        }
    }

    gtk_init();

    // Transparent window/drawing-area background so only the trail shows.
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window, drawingarea { background: transparent; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *window = gtk_window_new();
    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(window), "easystroke-trail");
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    GtkWidget *area = gtk_drawing_area_new();
    o.area = area;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_cb, &o, nullptr);
    gtk_window_set_child(GTK_WINDOW(window), area);

    g_signal_connect(window, "realize", G_CALLBACK(on_realize), nullptr);
    gtk_window_present(GTK_WINDOW(window));

    // Read stdin without blocking the GTK main loop.
    int flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    o.loop = g_main_loop_new(nullptr, FALSE);
    g_unix_fd_add(STDIN_FILENO, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP), stdin_cb, &o);
    g_unix_signal_add(SIGTERM, on_term, &o);
    g_unix_signal_add(SIGINT, on_term, &o);

    g_main_loop_run(o.loop);
    g_main_loop_unref(o.loop);
    return 0;
}
