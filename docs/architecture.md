# WeazyStroke -- Architecture Overview

This page is the system map for WeazyStroke: the four processes, how a stroke travels from a raw device event to an executed action, where each subsystem lives, and the design rules that hold the split together.

## Table of Contents

1. [What WeazyStroke is](#1-what-weazystroke-is)
2. [Process model](#2-process-model)
3. [The gesture pipeline](#3-the-gesture-pipeline)
4. [Permission model](#4-permission-model)
5. [Source layout](#5-source-layout)
6. [The core library](#6-the-core-library)
7. [Design rules](#7-design-rules)
8. [Document index](#8-document-index)

---

## 1. What WeazyStroke is

WeazyStroke is gesture recognition for Wayland. You hold a trigger — a mouse button, the pen tip, a stylus chord, or a two-finger touch — draw a stroke, and on release the daemon matches the shape against your recorded gestures and runs the bound action: launch a command, send a key combo, type text, click, scroll, or nothing.

It is a desktop-environment-agnostic rebuild of the classic [easystroke](https://github.com/thjaeger/easystroke) (X11) on top of Wayland-native plumbing. The recognition core is recycled verbatim from easystroke; everything around it — input capture, injection, the live trail, the GTK4 GUI — is new. The engine links no GUI toolkit and no Boost: it speaks only to `libinput`/`libevdev`, `xkbcommon`, and the kernel's `uinput`. That self-containment is the spine of the whole architecture.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 1000 726" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg      { fill: #1a1b26; }
    .layer-d { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .layer-e { fill: #1a2235; stroke: #7aa2f7; stroke-width: 1.5; }
    .layer-o { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .layer-c { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.5; }
    .box     { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .box-hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .sys     { fill: #1f2535; stroke: #565f89; stroke-width: 1; }
    .lbl     { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #8c92b3; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-cy  { fill: #7dcfff; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-blu { fill: #7aa2f7; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-pur { fill: #bb9af7; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln      { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .ln-p    { stroke: #bb9af7; stroke-width: 1.5; fill: none; }
    .bound   { stroke: #6b7398; stroke-width: 1.2; stroke-dasharray: 6,4; fill: none; }
    .title   { fill: #7aa2f7; font-size: 14px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>

  <rect x="0" y="0" width="1000" height="726" class="bg"/>
  <text x="500" y="26" text-anchor="middle" class="title">WeazyStroke process &amp; data-flow architecture</text>

  <!-- devices -->
  <rect x="20" y="44" width="700" height="86" class="layer-d"/>
  <text x="40" y="64" class="lbl-cy">input devices</text>
  <text x="40" y="78" class="lbl-mut">read from /dev/input/event* (input group / udev rule)</text>
  <rect x="40"  y="86" width="200" height="34" class="box"/>
  <text x="140" y="107" text-anchor="middle" class="lbl-sm">mouse (relative)</text>
  <rect x="252" y="86" width="220" height="34" class="box"/>
  <text x="362" y="107" text-anchor="middle" class="lbl-sm">pen / tablet (absolute)</text>
  <rect x="484" y="86" width="216" height="34" class="box"/>
  <text x="592" y="107" text-anchor="middle" class="lbl-sm">touchscreen (MT slots)</text>

  <!-- capture connector -->
  <line x1="120" y1="130" x2="120" y2="184" class="ln"/>
  <text x="130" y="152" class="lbl-cy">capture</text>
  <text x="130" y="165" class="lbl-mut">monitor, or EVIOCGRAB</text>

  <!-- daemon engine -->
  <rect x="20" y="188" width="700" height="178" class="layer-e"/>
  <text x="40" y="208" class="lbl-blu">eswl-daemon  --  input engine (links eswl_core)</text>
  <text x="40" y="222" class="lbl-mut">singleton (flock); captures, recognizes, dispatches; no GUI toolkit</text>

  <rect x="40"  y="234" width="200" height="118" class="box"/>
  <text x="140" y="254" text-anchor="middle" class="lbl-sm">input sources</text>
  <text x="140" y="270" text-anchor="middle" class="lbl-mut">LibinputSource</text>
  <text x="140" y="284" text-anchor="middle" class="lbl-mut">EvdevSource (grab)</text>
  <text x="140" y="298" text-anchor="middle" class="lbl-mut">TouchEvdevSource</text>
  <text x="140" y="320" text-anchor="middle" class="lbl-mut">normalize to Samples</text>
  <text x="140" y="334" text-anchor="middle" class="lbl-mut">(x, y, t, pressure)</text>

  <rect x="252" y="234" width="210" height="118" class="box-hot"/>
  <text x="357" y="254" text-anchor="middle" class="lbl-yel">gesture recognizer</text>
  <text x="357" y="272" text-anchor="middle" class="lbl-mut">trigger gate / touch gate</text>
  <text x="357" y="286" text-anchor="middle" class="lbl-mut">accumulate on hold</text>
  <text x="357" y="300" text-anchor="middle" class="lbl-mut">match on release via</text>
  <text x="357" y="314" text-anchor="middle" class="lbl-mut">stroke.c (recycled)</text>
  <text x="357" y="334" text-anchor="middle" class="lbl-mut">best-of-templates</text>

  <rect x="474" y="234" width="226" height="118" class="box"/>
  <text x="587" y="254" text-anchor="middle" class="lbl-sm">action dispatch</text>
  <text x="587" y="272" text-anchor="middle" class="lbl-mut">command / key / text</text>
  <text x="587" y="286" text-anchor="middle" class="lbl-mut">button / scroll / ignore</text>
  <text x="587" y="300" text-anchor="middle" class="lbl-mut">keymap (xkbcommon)</text>
  <text x="587" y="314" text-anchor="middle" class="lbl-mut">run_command (fork)</text>
  <text x="587" y="334" text-anchor="middle" class="lbl-mut">UinputInjector</text>

  <!-- child processes column -->
  <rect x="740" y="44" width="240" height="322" class="layer-o"/>
  <text x="760" y="64" class="lbl-grn">child processes</text>
  <text x="760" y="78" class="lbl-mut">forked by the daemon,</text>
  <text x="760" y="91" class="lbl-mut">fed a line protocol over a pipe</text>

  <rect x="760" y="104" width="200" height="120" class="box"/>
  <text x="860" y="124" text-anchor="middle" class="lbl-sm">eswl-overlay</text>
  <text x="860" y="140" text-anchor="middle" class="lbl-mut">GTK4 + layer-shell</text>
  <text x="860" y="154" text-anchor="middle" class="lbl-mut">click-through trail</text>
  <text x="860" y="168" text-anchor="middle" class="lbl-mut">Cairo effects, OSD</text>
  <text x="860" y="188" text-anchor="middle" class="lbl-mut">B / P / E / A / a ...</text>
  <text x="860" y="202" text-anchor="middle" class="lbl-mut">self-healing respawn</text>

  <rect x="760" y="236" width="200" height="116" class="box"/>
  <text x="860" y="256" text-anchor="middle" class="lbl-sm">eswl-tray</text>
  <text x="860" y="272" text-anchor="middle" class="lbl-mut">StatusNotifierItem</text>
  <text x="860" y="286" text-anchor="middle" class="lbl-mut">GIO / GDBus, no GTK</text>
  <text x="860" y="300" text-anchor="middle" class="lbl-mut">enable / prefs / quit</text>
  <text x="860" y="320" text-anchor="middle" class="lbl-mut">E / D state down,</text>
  <text x="860" y="334" text-anchor="middle" class="lbl-mut">signals up to parent</text>

  <!-- engine -> children pipes -->
  <line x1="700" y1="284" x2="760" y2="164" class="ln"/>
  <line x1="700" y1="300" x2="760" y2="294" class="ln"/>

  <!-- injection connector -->
  <line x1="587" y1="352" x2="587" y2="404" class="ln"/>
  <rect x="474" y="408" width="226" height="40" class="sys"/>
  <text x="587" y="427" text-anchor="middle" class="lbl-mut">/dev/uinput</text>
  <text x="587" y="440" text-anchor="middle" class="lbl-mut">synthetic keys / buttons / scroll</text>

  <!-- config plane boundary -->
  <line x1="20" y1="486" x2="980" y2="486" class="bound"/>
  <text x="500" y="481" text-anchor="middle" class="lbl-yel">configuration plane  --  no live IPC socket; files + signals</text>

  <!-- config GUI -->
  <rect x="20" y="502" width="460" height="200" class="layer-c"/>
  <text x="40" y="522" class="lbl-pur">eswl-config  --  GTK4 + libadwaita GUI</text>
  <text x="40" y="536" class="lbl-mut">Actions table, recorder, paginated Preferences, History</text>
  <rect x="40"  y="548" width="200" height="60" class="box"/>
  <text x="140" y="572" text-anchor="middle" class="lbl-sm">edit gestures</text>
  <text x="140" y="588" text-anchor="middle" class="lbl-mut">record strokes, set actions</text>
  <rect x="252" y="548" width="208" height="60" class="box"/>
  <text x="356" y="572" text-anchor="middle" class="lbl-sm">tune preferences</text>
  <text x="356" y="588" text-anchor="middle" class="lbl-mut">trail, triggers, threshold</text>
  <rect x="40"  y="624" width="420" height="60" class="box"/>
  <text x="250" y="648" text-anchor="middle" class="lbl-sm">spawns the daemon for --record / --capture-trigger</text>
  <text x="250" y="664" text-anchor="middle" class="lbl-mut">installs the systemd --user autostart service on toggle</text>

  <!-- shared files -->
  <rect x="520" y="502" width="460" height="200" class="layer-e"/>
  <text x="540" y="522" class="lbl-blu">shared state  --  ~/.config/easystroke-wayland/</text>
  <text x="540" y="536" class="lbl-mut">the only thing the GUI and daemon both touch</text>
  <rect x="540" y="548" width="420" height="50" class="sys"/>
  <text x="750" y="568" text-anchor="middle" class="lbl-sm">gestures.json</text>
  <text x="750" y="583" text-anchor="middle" class="lbl-mut">gestures, actions, trigger, all settings</text>
  <rect x="540" y="608" width="420" height="44" class="sys"/>
  <text x="750" y="628" text-anchor="middle" class="lbl-sm">history.jsonl</text>
  <text x="750" y="643" text-anchor="middle" class="lbl-mut">one recognition result per line (read by History)</text>
  <rect x="540" y="662" width="420" height="34" class="box-hot"/>
  <text x="750" y="683" text-anchor="middle" class="lbl-yel">GUI writes -> SIGHUP -> daemon reloads (no restart)</text>

  <!-- GUI -> files -->
  <line x1="480" y1="574" x2="540" y2="574" class="ln-p"/>
</svg>
</div>

## 2. Process model

WeazyStroke is four small binaries, not one monolith. Only the daemon ever captures or injects input; the others render, edit, or sit in the tray.

| Binary | Role | Links |
| --- | --- | --- |
| `eswl-daemon` | The input engine: capture, recognition, action dispatch, injection. The hub. | `eswl_core` (libinput, libevdev, xkbcommon) |
| `eswl-overlay` | The live stroke-trail renderer: a click-through layer-shell window. | GTK4 + `gtk4-layer-shell` |
| `eswl-config` | The configuration GUI: edit gestures, record strokes, tune preferences. | GTK4 + libadwaita |
| `eswl-tray` | The system-tray icon: enable/disable, open preferences, quit. | GIO / GDBus only (no GTK) |

The daemon is deliberately the only privileged-ish component (it needs device access), and it owns the overlay and tray as **child processes**. It forks them, wires a pipe to each one's stdin, and streams a tiny line-based protocol down that pipe. Keeping them in separate processes means the engine links no GUI toolkit at all, and a crash in the trail renderer or the tray can never take down input capture.

The GUI is **not** a child of the daemon and there is no live socket between them. They coordinate entirely through two shared files and two signals: the GUI writes `gestures.json`, then raises `SIGHUP` so the daemon reloads; the GUI's pause control raises `SIGUSR1` to toggle the daemon's enabled state. The daemon appends each recognition outcome to `history.jsonl`, which the GUI's History tab reads back. This file-plus-signal coupling is intentional — it keeps the editor and the engine independent, restartable, and debuggable in isolation.

### Communication channels at a glance

| From | To | Mechanism | Carries |
| --- | --- | --- | --- |
| daemon | overlay | pipe (line protocol) | begin/point/end, colours, width, pressure, cue, OSD |
| daemon | tray | pipe (`E`/`D` lines) | current enabled state |
| tray | daemon | signals to the parent PID | toggle / open prefs / quit |
| GUI | daemon | `SIGHUP` / `SIGUSR1` | "reload config" / "toggle enable" |
| GUI | daemon | `gestures.json` (file) | the entire configuration |
| daemon | GUI | `history.jsonl` (file) | recognition log for the History tab |

## 3. The gesture pipeline

Inside the daemon, every stroke follows the same path regardless of which device drew it. The seam between each stage is a narrow C++ interface, so a stage can be swapped without touching its neighbours.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 980 250" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .box  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .boxh { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .lbl  { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbls { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut  { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .cy   { fill: #7dcfff; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .ttl  { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="980" height="250" class="bg"/>
  <text x="490" y="24" text-anchor="middle" class="ttl">one stroke, end to end</text>

  <rect x="14"  y="48" width="150" height="80" class="box"/>
  <text x="89"  y="74" text-anchor="middle" class="lbls">InputSource</text>
  <text x="89"  y="92" text-anchor="middle" class="lbl-mut">device event -></text>
  <text x="89"  y="105" text-anchor="middle" class="lbl-mut">Sample</text>
  <text x="89"  y="118" text-anchor="middle" class="lbl-mut">(x,y,t,pressure)</text>

  <rect x="190" y="48" width="160" height="80" class="box"/>
  <text x="270" y="74" text-anchor="middle" class="lbls">InputSink gate</text>
  <text x="270" y="92" text-anchor="middle" class="lbl-mut">trigger held?</text>
  <text x="270" y="105" text-anchor="middle" class="lbl-mut">accumulate path,</text>
  <text x="270" y="118" text-anchor="middle" class="lbl-mut">track max travel</text>

  <rect x="376" y="48" width="160" height="80" class="boxh"/>
  <text x="456" y="74" text-anchor="middle" class="lbls">recognize</text>
  <text x="456" y="92" text-anchor="middle" class="lbl-mut">normalize (stroke.c)</text>
  <text x="456" y="105" text-anchor="middle" class="lbl-mut">compare vs templates</text>
  <text x="456" y="118" text-anchor="middle" class="lbl-mut">best score vs floor</text>

  <rect x="562" y="48" width="160" height="80" class="box"/>
  <text x="642" y="74" text-anchor="middle" class="lbls">dispatch</text>
  <text x="642" y="92" text-anchor="middle" class="lbl-mut">find binding,</text>
  <text x="642" y="105" text-anchor="middle" class="lbl-mut">run its action</text>
  <text x="642" y="118" text-anchor="middle" class="lbl-mut">(unless disabled)</text>

  <rect x="748" y="48" width="218" height="80" class="box"/>
  <text x="857" y="74" text-anchor="middle" class="lbls">effect</text>
  <text x="857" y="92" text-anchor="middle" class="lbl-mut">uinput keys/buttons/scroll,</text>
  <text x="857" y="105" text-anchor="middle" class="lbl-mut">fork a command, or no-op</text>
  <text x="857" y="118" text-anchor="middle" class="lbl-mut">(report to history.jsonl)</text>

  <line x1="164" y1="88" x2="190" y2="88" class="ln"/>
  <line x1="350" y1="88" x2="376" y2="88" class="ln"/>
  <line x1="536" y1="88" x2="562" y2="88" class="ln"/>
  <line x1="722" y1="88" x2="748" y2="88" class="ln"/>

  <!-- overlay tap -->
  <rect x="190" y="166" width="346" height="56" class="box"/>
  <text x="363" y="188" text-anchor="middle" class="cy">live: each accumulated point is also streamed to eswl-overlay</text>
  <text x="363" y="205" text-anchor="middle" class="lbl-mut">begin on press, point on motion, end on release -> the trail you see</text>
  <line x1="270" y1="128" x2="270" y2="166" class="ln"/>
  <line x1="456" y1="128" x2="456" y2="166" class="ln"/>
</svg>
</div>

The stages, and the documents that go deep on each:

1. **Capture.** An `InputSource` reads the platform and emits normalized `Sample`s (`x, y, t`, plus pen pressure) to an `InputSink`. Three sources exist; see [Input Capture & Triggers](input-pipeline.gen.html).
2. **Gate & accumulate.** The `GestureRecognizer` (an `InputSink`) decides whether the trigger is held — via the trigger gate (button/pen) or the touch gate (two-finger) — and accumulates the path, tracking maximum travel so a mere click is rejected.
3. **Recognize.** On release the path is normalized and compared against every recorded template by the recycled `stroke.c` core; the best score is taken and tested against the threshold. See [Gesture Recognition](recognition.gen.html).
4. **Dispatch.** The winning binding's action runs — unless the global enable toggle is off. See [Actions & Injection](actions.gen.html).
5. **Effect.** The action reaches the system through the `UinputInjector`, a forked shell command, or nothing (`ignore`). Meanwhile every point was streamed live to the [trail overlay](overlay.gen.html).

## 4. Permission model

WeazyStroke never needs to run as root, but it does need two kernel interfaces that are not world-accessible by default.

| Capability | Why | How it is granted |
| --- | --- | --- |
| read `/dev/input/event*` | capture pointer / pen / touch events | `input` group + the shipped udev rule (`MODE=0640`) |
| read-write `/dev/uinput` | inject keys, buttons, scroll | `input` group + udev rule (`MODE=0660`, static node) |

The daemon opens devices through `libinput`'s udev backend (monitor mode) or directly via `libevdev` (grab mode), and creates one virtual output device through `/dev/uinput`. Capture in the default mode is *monitor-only*: the compositor still sees every event, so the trigger button also performs its normal action. The optional `--grab` mode opens mice with `EVIOCGRAB`, withholds the trigger, and replays it as a real click only if the motion was too small to be a gesture — the faithful Wayland equivalent of X11's passive button grab. The keyboard is never grabbed, so `Ctrl-C` always works and the kernel drops every grab when the process exits.

A single-instance `flock` on `$XDG_RUNTIME_DIR/weazystroke.lock` guarantees only one daemon runs; a second would double every injected action. The included systemd unit hardens the service further (`NoNewPrivileges`, `ProtectSystem=strict`, scoped `DeviceAllow`). See [Build, Install & Packaging](install.gen.html).

## 5. Source layout

Everything is under `src/`, with the engine sources compiled into a single static library and four thin `main` entrypoints on top.

| Path | Purpose |
| --- | --- |
| `src/stroke.{c,h}` | Recognition core, recycled verbatim from easystroke (pure C, no deps) |
| `src/gesture.{cc,h}` | C++ wrapper over `stroke.c`: normalize, compare, score |
| `src/gesture_recognizer.{cc,h}` | The stroke state machine: gates, accumulation, matching, dispatch |
| `src/*_source.{cc,h}` | Input sources: `libinput`, grab-mode `evdev`, raw touch `evdev` |
| `src/uinput_injector.{cc,h}`, `keymap.{cc,h}`, `button_map.{cc,h}` | Injection, key resolution, button numbering |
| `src/gesture_config.{cc,h}`, `json.{cc,h}` | Config model and a self-contained JSON parser/serializer |
| `src/process*.{cc,h}` | Subprocess helpers: `run_command`, overlay supervisor, tray supervisor |
| `src/main.cc` | `eswl-daemon` |
| `src/overlay_main.cc` | `eswl-overlay` |
| `src/eswl_config_main.cc` | `eswl-config` |
| `src/eswl_tray_main.cc` | `eswl-tray` |
| `src/*_gate.h`, `trace_overlay.h`, `input_*.h` | Interface headers (pure-logic gates, abstractions, event types) |
| `tests/` | Unit tests for recognition, config, JSON, keymap, and the gates |

## 6. The core library

`eswl_core` is the static library that makes the four-binary split possible. It bundles the recognition core, all input sources, the injector, the keymap, the config layer, and the process supervisors — and deliberately links **nothing** but libc/libstdc++, `libinput`/`libudev`/`libevdev`, and `xkbcommon`. No Boost, no GTK, no X11.

This is why the daemon stays toolkit-free while still driving a GTK trail: it talks to the renderer through the abstract `TraceOverlay` interface, whose only implementation here (`ProcessOverlay`) is a fork+exec of the GTK binary plus a pipe. The GUI process reuses just two of the core's source files directly (`gesture_config.cc`, `json.cc`) so the editor and the engine read and write byte-identical config — it does not link the sanitized `eswl_core` itself, avoiding a sanitizer/toolkit mismatch. The same library backs every unit test, so the gate logic, recognition, and config round-tripping are all verified headlessly. See [Configuration & Process Model](configuration.gen.html).

## 7. Design rules

A handful of rules keep the system coherent. They are why the four-process split pays for itself.

- **The engine links no GUI toolkit.** Rendering lives in separate processes reached through a line protocol behind the `TraceOverlay` abstraction.
- **The recognition core is recycled untouched.** `stroke.c` is compiled as-is from easystroke; the Wayland project wraps it, it does not fork it.
- **Process isolation is a safety boundary.** An overlay or tray crash must never interrupt input capture; the daemon respawns the overlay on demand and ignores `SIGPIPE`.
- **Config is a file, application is live.** All state lives in human-editable JSON; saving raises `SIGHUP` and the running daemon rebuilds its bindings with no restart.
- **One engine, every device.** Mouse, pen, stylus chord, and two-finger touch all feed the same recognizer through the same `Sample` type.
- **Never run as root.** Device and uinput access come from the `input` group and a udev rule; a `flock` enforces a single instance.

## 8. Document index

| Document | Covers |
| --- | --- |
| [Gesture Recognition](recognition.gen.html) | The `stroke.c` algorithm, normalization, the DP matcher, scoring, multi-example templates |
| [Input Capture & Triggers](input-pipeline.gen.html) | The three sources, device classes, and the trigger / touch gate state machines |
| [Actions & Injection](actions.gen.html) | The typed action model, `uinput` injection, the `xkbcommon` keymap, button mapping |
| [Stroke-Trail Overlay](overlay.gen.html) | The overlay process, its line protocol, Cairo effects, pressure, the cue, and self-healing |
| [Config GUI & Tray](config-gui.gen.html) | The GTK4/libadwaita app, the Actions table, the recorder, Preferences, and the tray |
| [Configuration & Process Model](configuration.gen.html) | The JSON schema, live reload, the singleton lock, and subprocess supervision |
| [Build, Install & Packaging](install.gen.html) | CMake targets, dependencies, sanitizers, the udev rule, autostart, and packaging |
