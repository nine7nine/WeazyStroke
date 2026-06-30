# WeazyStroke -- Build, Install & Packaging

This page covers building the four binaries, the dependency and sanitizer story, the install footprint, the device permissions the engine needs, the two systemd units, and the Arch package.

## Table of Contents

1. [Dependencies](#1-dependencies)
2. [The build graph](#2-the-build-graph)
3. [Building](#3-building)
4. [Sanitizers](#4-sanitizers)
5. [Tests](#5-tests)
6. [Install footprint](#6-install-footprint)
7. [Device permissions](#7-device-permissions)
8. [systemd units](#8-systemd-units)
9. [Packaging](#9-packaging)

---

## 1. Dependencies

The build needs a C11/C++17 toolchain, CMake ≥ 3.18 (Ninja recommended), and these libraries:

| Dependency | Used by |
| --- | --- |
| `libinput`, `libudev`, `libevdev` | the engine's input sources |
| `xkbcommon` | the keymap (key/text actions) |
| `gtk4`, `gtk4-layer-shell` | the trail overlay |
| `gtk4`, `libadwaita` | the config GUI |
| `gio-2.0`, `gio-unix-2.0` (glib2) | the system tray |

The dependency split is deliberate and visible in `CMakeLists.txt`: the engine library links **only** `libinput`/`libudev`/`libevdev` and `xkbcommon`. GTK, libadwaita, and layer-shell are pulled in by their respective front-end binaries and nothing else. The core never links a GUI toolkit.

## 2. The build graph

One static library, `eswl_core`, holds the recognition core and every engine subsystem; the four executables are thin entrypoints on top of it (or, for the GUI, a deliberate partial reuse).

<div class="diagram-container">
<svg width="100%" viewBox="0 0 940 320" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .lib  { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .bin  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .dep  { fill: #1f2535; stroke: #565f89; stroke-width: 1; }
    .lbls { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl  { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut  { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .yel  { fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ttl  { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.4; fill: none; }
  </style>
  <rect x="0" y="0" width="940" height="320" class="bg"/>
  <text x="470" y="24" text-anchor="middle" class="ttl">build graph: one core, four binaries</text>

  <rect x="370" y="46" width="200" height="74" class="lib"/>
  <text x="470" y="68" text-anchor="middle" class="yel">eswl_core (static)</text>
  <text x="470" y="86" text-anchor="middle" class="lbl-mut">stroke.c, recognizer, sources,</text>
  <text x="470" y="100" text-anchor="middle" class="lbl-mut">injector, keymap, config, json</text>
  <text x="470" y="114" text-anchor="middle" class="lbl-mut">links: libinput, libevdev, xkb</text>

  <rect x="40"  y="170" width="190" height="64" class="bin"/>
  <text x="135" y="194" text-anchor="middle" class="lbls">eswl-daemon</text>
  <text x="135" y="212" text-anchor="middle" class="lbl-mut">main.cc + eswl_core</text>

  <rect x="260" y="170" width="190" height="64" class="bin"/>
  <text x="355" y="194" text-anchor="middle" class="lbls">eswl-overlay</text>
  <text x="355" y="212" text-anchor="middle" class="lbl-mut">gtk4 + layer-shell</text>

  <rect x="490" y="170" width="190" height="64" class="bin"/>
  <text x="585" y="194" text-anchor="middle" class="lbls">eswl-config</text>
  <text x="585" y="212" text-anchor="middle" class="lbl-mut">gtk4 + libadwaita</text>

  <rect x="710" y="170" width="190" height="64" class="bin"/>
  <text x="805" y="194" text-anchor="middle" class="lbls">eswl-tray</text>
  <text x="805" y="212" text-anchor="middle" class="lbl-mut">gio / gdbus only</text>

  <line x1="450" y1="120" x2="135" y2="170" class="ln"/>
  <line x1="585" y1="120" x2="585" y2="170" class="ln"/>

  <rect x="260" y="262" width="420" height="40" class="dep"/>
  <text x="470" y="286" text-anchor="middle" class="lbl-mut">eswl-overlay and eswl-tray are independent of eswl_core (own deps only)</text>
  <text x="585" y="252" text-anchor="middle" class="lbl-mut">reuses gesture_config.cc + json.cc directly (not the sanitized core)</text>
</svg>
</div>

`eswl-config` does not link the full `eswl_core`; it compiles `gesture_config.cc` and `json.cc` directly. This is intentional — it avoids a sanitizer mismatch and GTK leak noise while still sharing the exact config/JSON code with the engine, so both read and write identical files. `eswl-overlay` and `eswl-tray` share no engine code at all; they only consume the line protocols.

## 3. Building

A release build, then install:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF
cmake --build build
sudo cmake --install build
```

A development build is just the defaults — `Debug` with sanitizers on (drop the two flags). The repo also ships `reinstall.sh`, which builds a separate `build-release` tree (Release, no ASan — *not* the dev `build/`), installs it to `/usr`, and restarts the user service so the freshly installed daemon and overlay take effect:

```sh
./reinstall.sh        # build-release -> sudo install -> systemctl --user restart weazystroke.service
```

With the self-healing overlay, a later compositor restart no longer needs that restart step — the trail comes back on its own.

## 4. Sanitizers

`ENABLE_ASAN` is **on by default** so day-to-day dev builds catch memory and undefined-behaviour bugs early; release packaging turns it off (`-DENABLE_ASAN=OFF`). When on, AddressSanitizer + UndefinedBehaviorSanitizer instrument **everything**, the GUI and overlay included, so a bug surfaces in dev rather than only in an optimized release build. This was added precisely because a release-only out-of-bounds array read had slipped through (now fixed and guarded by a test). Run the GTK apps with `ASAN_OPTIONS=detect_leaks=0` to silence GTK's benign one-shot allocations.

The warning policy is layered: the project's own C++ sources get `-Wall -Wextra -Wpedantic`; the GTK/GIO front-ends get `-Wall -Wextra` (no `-Wpedantic`, whose macro noise is not worth it); and `stroke.c`, recycled verbatim, is compiled as C with only `-Wall` so upstream style does not generate noise.

## 5. Tests

`enable_testing()` registers a suite that links the same `eswl_core` the daemon uses, so the engine is exercised exactly as shipped. Run them with:

```sh
ctest --test-dir build
```

| Test | Covers |
| --- | --- |
| `recognition` | stroke scoring and orthogonal-shape separation |
| `multitemplate` | best-of-templates matching |
| `gate` | the pen "tip + side button" chord gate |
| `touch_gate` | the edge-anchor / draw-finger state machine |
| `trigger_gate` | the suppress-vs-replay decision |
| `config` | JSON config round-trip, typed actions, legacy formats |
| `json` | the JSON parser/serializer |
| `keymap` | keysym / character / combo resolution (skips if no XKB layout) |

The gates and recognition are pure logic, so the suite verifies the trickiest behaviour with no hardware and no display.

## 6. Install footprint

`cmake --install` lays down four binaries and the desktop integration:

| Installed | Path |
| --- | --- |
| `eswl-daemon`, `eswl-overlay`, `eswl-config`, `eswl-tray` | `bin/` |
| `weazystroke.svg` (app icon) | `share/icons/hicolor/scalable/apps/` |
| `weazystroke.desktop` (launcher, `Exec=eswl-config`) | `share/applications/` |

The udev rule is **not** installed by CMake — the Arch package installs it, and a from-source install copies it by hand (below). The autostart systemd `--user` service is not installed either; the GUI's "Start on login" toggle writes it on demand.

## 7. Device permissions

The engine never runs as root, but it needs two non-default kernel interfaces: read access to `/dev/input/event*` (capture) and read-write `/dev/uinput` (injection). The shipped udev rule grants both to the `input` group:

```
KERNEL=="event*", SUBSYSTEM=="input", GROUP="input", MODE="0640"
KERNEL=="uinput", SUBSYSTEM=="misc",  GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
```

From a source build, install it and join the group:

```sh
sudo cp packaging/99-easystroke-wayland.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG input "$USER"        # then re-login
```

If `/dev/uinput` is unavailable the daemon still runs — it just warns that injection is off, so `command` actions work but `key`/`text`/`button`/`scroll` do not. See [Actions & Injection](actions.gen.html) and the [permission model](architecture.gen.html#4-permission-model).

## 8. systemd units

There are two units, for two different roles — do not confuse them.

**The autostart user service** is the one most people use. The GUI's "Start on login" toggle writes `~/.config/systemd/user/weazystroke.service`, whose `ExecStart` is the daemon with `--overlay --tray`, and enables it with `systemctl --user enable --now`. It runs in your graphical session, picks up your `input`-group membership, and brings up the engine, trail, and tray together. See [Config GUI & Tray](config-gui.gen.html).

**The system service template** (`packaging/easystroke-wayland.service`) is a starting point for the future "small privileged daemon" model, where the engine has device capabilities and the GUI talks to it over IPC. It is hardened — `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome=read-only`, `PrivateTmp`, and scoped `DeviceAllow` for `/dev/uinput` and `char-input` — and is provided as a reference, not wired up by the install.

## 9. Packaging

The Arch package (`packaging/PKGBUILD`, `weazystroke-git`) builds straight from git:

```sh
cd packaging && makepkg -si
```

It configures a Release, no-ASan build with `-DCMAKE_INSTALL_PREFIX=/usr`, runs the CMake install (the four binaries, icon, and launcher), and additionally installs the udev rule to `/usr/lib/udev/rules.d/99-easystroke-wayland.rules`. Its `depends` array is the runtime set — `libinput`, `libevdev`, `libxkbcommon`, `gtk4`, `gtk4-layer-shell`, `libadwaita`, `glib2`, `systemd-libs` — and `pkgver()` derives a version from the git revision count and short hash.

> **GNOME note.** On GNOME everything works *except* the live trail: Mutter does not implement `wlr-layer-shell`, so the overlay window cannot be a click-through always-on-top surface. The engine, GUI, and actions are unaffected. The overlay already sits behind a swappable process + line-protocol interface, so a GNOME Shell-extension backend over DBus could restore the trail with no engine changes — see [Stroke-Trail Overlay](overlay.gen.html).
