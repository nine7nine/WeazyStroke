# WeazyStroke -- Config GUI & Tray

This page documents the two user-facing processes: `eswl-config`, the GTK4 + libadwaita configuration app (Actions table, in-window recorder, paginated Preferences, History), and `eswl-tray`, the GIO/GDBus system-tray helper. Neither captures input — they edit the config and signal the daemon.

## Table of Contents

1. [The config app at a glance](#1-the-config-app-at-a-glance)
2. [The Actions table](#2-the-actions-table)
3. [Recording a stroke](#3-recording-a-stroke)
4. [Preferences](#4-preferences)
5. [Triggers](#5-triggers)
6. [Autostart and live apply](#6-autostart-and-live-apply)
7. [History](#7-history)
8. [Theming: the glass look](#8-theming-the-glass-look)
9. [The system tray](#9-the-system-tray)
10. [A note on names](#10-a-note-on-names)

---

## 1. The config app at a glance

`eswl-config` (`src/eswl_config_main.cc`) is an `AdwApplication` with the ID `org.weazystroke.Config`. That ID makes it a singleton: a second launch is routed to the running instance, which presents (or, with `--toggle`, hides) the existing window rather than stacking a new one. The tray's left-click passes `--toggle`; the `--config PATH` flag overrides the config path.

<figure class="screenshot">
  <img src="screenshot.png" alt="WeazyStroke config window: the Actions table with stroke thumbnails, name, type, and argument columns">
  <figcaption>The Actions table &mdash; each row is one gesture: a stroke thumbnail (drag-to-reorder handle, with an &times;N badge for multi-example gestures), a name, a typed action, and a type-specific argument editor. Tabs, Pause, and Save live in the window body; the empty headerbar keeps client-side window controls.</figcaption>
</figure>

The window is `920×940`, forced to dark (`ADW_COLOR_SCHEME_FORCE_DARK`) so the glass styling reads correctly, and given an empty opaque region on realize so the compositor composites the window's own translucency/blur. Its structure is deliberately unconventional: the `GtkHeaderBar` is **empty**, present only to keep client-side decorations (window controls + a draggable titlebar). The real chrome lives in the body:

| Element | Role |
| --- | --- |
| `GtkStack` + `GtkStackSwitcher` | three tabs — **Actions**, **Preferences**, **History** |
| Pause / Resume button | `pkill -USR1 -x eswl-daemon` — toggle the daemon's enabled state |
| Save button (`suggested-action`) | write `gestures.json`, then `pkill -HUP -x eswl-daemon` to reload |
| status label (bottom) | one-line feedback ("Saved — running daemon reloaded.", etc.) |

Gesture data lives in `GestureConfig`, loaded from `gestures.json` on startup. Appearance lives separately in a sibling `gui.json` (window opacity and the glass/accent colours), and the History tab reads a sibling `history.jsonl`.

## 2. The Actions table

The Actions tab is a `GtkListBox` with one row per gesture, under a fixed column header. The columns are **Stroke** (64 px), **Name** (180 px), **Type** (120 px), and **Argument** (the rest).

**Stroke thumbnail.** A 64×40 `GtkDrawingArea` renders the gesture's first recorded example as a polyline in the same blue→green direction gradient the trail uses (start blue, end green), auto-fitted with padding. When a gesture carries multiple examples, a bold `×N` count badge is drawn in the corner. The thumbnail doubles as the **drag handle** (cursor "grab", tooltip "Drag to reorder").

**Name / Type / Argument.** The name is a flat entry; the type is a dropdown over `(none)`, `command`, `key`, `text`, `button`, `scroll`, `ignore`, `misc`; and the argument cell is rebuilt to match the type (next paragraph). Editing any field updates the in-memory config; nothing is persisted until **Save**.

**Type-specific argument editors.** `populate_arg_editor` swaps in the right widget for the selected type:

| Type | Editor |
| --- | --- |
| `command` / `text` / `(none)` | a plain entry with a type-specific placeholder |
| `key` | a **Key grabber** button: click to arm, then the first non-modifier press is captured and formatted as `ctrl+shift+t` (Esc cancels; modifier-only presses ignored) |
| `button` | a dropdown of buttons `1 — left` … `9 — forward` (stores the number) |
| `scroll` | a dropdown of `up` / `down` / `left` / `right` |
| `misc` | a dropdown: `(none)` or `Disable / Enable WeazyStroke` (→ argument `disable`) |
| `ignore` | a dim "(no argument)" label |

Switching type clears the argument and repopulates the editor. Selection is robust by design — a click anywhere on a row (via a single `GtkGestureClick` on the listbox), focus-in, and the `row-selected` signal all converge on the same selected index, and the selected row gets an accent tint plus a 3 px accent left-bar from CSS.

**Reordering.** The thumbnail is a `GDK_ACTION_MOVE` drag source; the drag icon is a live snapshot of the whole row. Dropping on another row reorders the underlying gesture list (compensating for the erase-then-insert index shift) and sets the status "Reordered — Save to keep." **Add Action**, **Delete Action**, and **Record Stroke** sit in a bar below the list.

## 3. Recording a stroke

Recording happens **in-window**, not in a separate dialog. `on_record` builds a sheet inside a `GtkOverlay` that hosts the main content, so it is anchored and centred to the window:

- a dim `.sheet-backdrop` that fills the window and dismisses the sheet on a click outside;
- a `460×380` `.sheet-card` with a header, a drawing area, and three buttons.

You press-and-drag in the drawing area; a `GtkGestureDrag` collects the points and previews them live in the blue→green gradient. Two save buttons distinguish the multi-example workflow:

| Button | Effect |
| --- | --- |
| **Replace stroke** (suggested) | clears existing examples, then stores this one |
| **Add example** | appends this stroke as another template for the same gesture |

Because recognition matches against *any* recorded example (best score wins), "Add example" is how you make a gesture sturdier — and the thumbnail's `×N` badge then reflects the new count. See [Gesture Recognition](recognition.gen.html). (The daemon also has a headless `--record NAME` mode that appends to an existing name; the GUI sheet is the graphical equivalent.)

## 4. Preferences

Preferences is an `AdwCarousel` of three pages — **Triggers**, **Feedback**, **Appearance** — paginated by clickable `●` page-dots (mouse-wheel paging is disabled so scrolling a spin button doesn't flip the page). Each page is an `AdwClamp` (max 560 px) so controls stay centred and readable. Every control writes straight into the in-memory `GestureConfig` (or, for the GUI-only appearance settings, applies and saves immediately).

These map one-to-one onto the JSON settings the daemon reads ([Configuration & Process Model](configuration.gen.html)):

| Setting (label) | Range / values | Config key |
| --- | --- | --- |
| Match threshold | `0.30 – 0.90` | `match_threshold` |
| Trail width (px) | `1 – 20` | `trace_width` |
| Show matched name (OSD) | on/off | `show_osd` |
| Pressure-sensitive trail | on/off | `pressure` |
| Pressure width min / max (px) | `1–40` / `1–60` | `pressure_min` / `pressure_max` |
| Scroll speed | `0.2 – 10.0` | `scroll_speed` |
| Invert scroll | on/off | `scroll_invert` |
| Trail effect | Plain / Glow / Sparkle | `trail_effect` |
| End transition (ms) | `0 – 1500` | `trail_fade_ms` |
| Trail colour start / end | colour pickers | `trail_color_start` / `trail_color_end` |
| Anchor edge | Off / Left / Right / Top / Bottom | `touch_edge` |
| Edge band (px) | `5 – 200` | `touch_band` |
| Ring size (px) | `20 – 300` | `touch_ring` |
| Grow / fade-out time (ms) | `50 – 2000` each | `touch_grow_ms` / `touch_out_ms` |
| Show edge-hold ring | on/off | `touch_cue` |
| Pressure-sensitive touch | on/off | `touch_pressure` |
| Contact size thin / full | `0 – 4000` each | `touch_pressure_floor` / `touch_pressure_ref` |
| Window opacity | `20 – 100` | `gui.json` (immediate) |
| Glass / accent colour | colour pickers | `gui.json` (immediate) |
| Start on login | on/off | systemd `--user` service |

Note the two colour groups behave differently: the **glass and accent** colours are GUI-only chrome and persist immediately to `gui.json`; the **trail** colours are daemon-side and persist only on **Save**.

## 5. Triggers

The Triggers page sets what starts a gesture, device-agnostically. A dropdown offers nine presets — built from `{trigger, gate}` pairs:

| Preset | trigger / gate |
| --- | --- |
| Pen tip + side button (hold) | `10` / `11` (chord) |
| Pen tip · Pen side button · Pen 2nd button | `10` · `11` · `12` |
| Right · Middle · Left button | `3` · `2` · `1` |
| Back · Forward button | `8` · `9` |

For a chord preset the side button must be held for the tip to start a gesture, keeping the side button free for its own action ([Input Capture & Triggers](input-pipeline.gen.html)). Four **Modifier** checkboxes (Ctrl/Alt/Shift/Super) add required modifiers for mouse-button triggers, writing `trigger_modifiers`.

If your trigger isn't a preset, **"Set…"** runs a learn-mode: it spawns `eswl-daemon --capture-trigger`, which prints the next pressed button or chord as `CAPTURE trigger=N gate=N`; the GUI parses that, sets the trigger/gate, and shows it as a "Custom: …" entry. Because the daemon reports the exact logical button (including which pen button), any device works. The same page also configures the two-finger touch edge gesture (edge, band, ring, timing, cue, contact-size pressure).

## 6. Autostart and live apply

**Start on login** installs a systemd `--user` service at `~/.config/systemd/user/weazystroke.service`. The unit's `ExecStart` is the resolved daemon path with `--overlay --tray`, so login brings up the engine, the trail, and the tray together:

```ini
[Unit]
Description=WeazyStroke gesture daemon
PartOf=graphical-session.target
After=graphical-session.target

[Service]
Type=simple
ExecStart=<.../eswl-daemon> --overlay --tray
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
```

Toggling the checkbox writes (or removes) this file and runs `systemctl --user daemon-reload` then `enable --now` / `disable --now`.

**Live apply.** The GUI never restarts the daemon to apply edits. **Save** writes `gestures.json` and raises `SIGHUP` (`pkill -HUP -x eswl-daemon`); the running daemon reloads and rebuilds its bindings in place. **Pause/Resume** raises `SIGUSR1` to toggle the enabled state. Both are fire-and-forget signals to whatever daemon is running — there is no socket between the GUI and the engine, only the shared config file and these signals.

## 7. History

The History tab is a live recognition log, fed by the `history.jsonl` the daemon appends one line per stroke. `refresh_history` reads the file, keeps the last 80 lines, and renders them newest-first:

- matched → `12:01:54   ✓  open-terminal   score 0.82`
- missed → a dim `12:02:07   ·  no match   best 0.54`

A 2-second timer polls the file's size and only re-renders when it changed, so an idle History tab is cheap. Seeing the best score on a miss is the practical way to decide whether to lower the threshold or redraw a gesture more carefully.

## 8. Theming: the glass look

The app ships a hand-written glass aesthetic in two CSS layers. The base layer (`load_css`, application priority) makes the window translucent, flattens entries/dropdowns/buttons into "cell" inputs that only reveal a border on hover/focus, forces popovers opaque so dropdown menus stay readable over the transparent window, and styles the Record sheet, scrollbars, sliders, and carousel dots.

The second layer (`apply_appearance`, user priority, rebuilt live) is where the user's colours land. Crucially it **redefines the theme's named accent colours** with `@define-color accent_bg_color` / `accent_color` / `accent_fg_color`, so *every* accent the theme draws — checkboxes, the active tab, focus rings, the threshold slider, text selection, the selected-row bar — follows the chosen accent. It also sets the window background to the glass colour at the chosen opacity. Opacity and the glass/accent colours are stored in `gui.json` and applied the instant you change them.

## 9. The system tray

`eswl-tray` (`src/eswl_tray_main.cc`) is a standalone **StatusNotifierItem**, implemented over GIO/GDBus with **no GTK** at all, so it stays light and works on KDE Plasma and any SNI host. The daemon spawns it (`--tray`) as a child over a pipe, exactly like the overlay.

On the session bus it owns a unique name (`org.kde.StatusNotifierItem-<pid>-1`) and registers two objects:

| Object path | Interface | Role |
| --- | --- | --- |
| `/StatusNotifierItem` | `org.kde.StatusNotifierItem` | the icon, status, tooltip, and click actions |
| `/MenuBar` | `com.canonical.dbusmenu` | the right-click menu |

It registers itself with `org.kde.StatusNotifierWatcher` and re-registers if the watcher reappears (e.g. after a `plasmashell` restart). The icon is `weazystroke`; the tooltip reflects state ("Gesture daemon — enabled" / "— paused"). The menu has four items: **Enabled** (a checkmark toggle), a separator, **Preferences…**, and **Quit**.

Control flows by signals to the parent daemon, never by a DBus call back to it:

| Tray action | Effect |
| --- | --- |
| left-click (`Activate`) | launch `eswl-config --toggle` (show/hide the window) |
| middle-click (`SecondaryActivate`) | `kill(getppid(), SIGUSR1)` — toggle enable |
| menu **Enabled** | `kill(getppid(), SIGUSR1)` — toggle enable |
| menu **Preferences…** | launch `eswl-config` |
| menu **Quit** | `kill(getppid(), SIGTERM)` then quit |

The reverse direction is the pipe: the daemon writes `E` / `D` lines to push its current enabled state down, and the tray updates the checkmark and tooltip — so the tray stays in sync no matter how the state was changed. A `flock` on `weazystroke-tray.lock` keeps it a singleton, and `prctl(PR_SET_PDEATHSIG, SIGTERM)` ensures it dies with the daemon.

## 10. A note on names

Three naming conventions coexist, which is worth knowing when reading paths and logs:

- the product / UI name is **WeazyStroke**;
- the binaries and the engine namespace use **`eswl-`** / `es::`;
- the on-disk config directory and the daemon project are still **`easystroke-wayland`** (`~/.config/easystroke-wayland/gestures.json`), reflecting the easystroke lineage;
- DBus and systemd identifiers use **`weazystroke`** (`org.weazystroke.Config`, `weazystroke.service`, the `weazystroke` icon).

See [Configuration & Process Model](configuration.gen.html) for the config file these apps read and write, and the [Architecture Overview](architecture.gen.html) for how the GUI and tray relate to the daemon.
