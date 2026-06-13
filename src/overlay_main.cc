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
    double width = 4.0; // trail line width (px), set via the "W" command
    std::string inbuf;  // accumulates partial stdin lines
};

void draw_cb(GtkDrawingArea *, cairo_t *cr, int width, int height, gpointer data) {
    Overlay *o = static_cast<Overlay *>(data);

    // Start from a fully transparent surface.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (o->xs.size() < 2)
        return;

    const double sx = width / o->screen_w;
    const double sy = height / o->screen_h;
    cairo_set_line_width(cr, o->width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    // Per-segment blue→green direction gradient, matching the record pad and
    // easystroke's Stroke::draw (start blue, end green).
    std::size_t n = o->xs.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        double t = static_cast<double>(i) / (n - 1);
        cairo_set_source_rgba(cr, 0.0, t, 1.0 - t, 1.0);
        cairo_move_to(cr, o->xs[i] * sx, o->ys[i] * sy);
        cairo_line_to(cr, o->xs[i + 1] * sx, o->ys[i + 1] * sy);
        cairo_stroke(cr);
    }
}

void process_line(Overlay *o, const std::string &line) {
    if (line.empty())
        return;
    switch (line[0]) {
    case 'B':
    case 'E': // begin and end both reset the trail
        o->xs.clear();
        o->ys.clear();
        break;
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
