# easystroke-wayland

A ground-up Wayland re-architecture of [easystroke](https://github.com/thjaeger/easystroke)
mouse-gesture recognition. This is **not** an X11 port — the X11 version lives on
separately for those who need it. This project recycles easystroke's
platform-independent parts (gesture recognition, action database, config, GUI)
and replaces everything that touched X11 with a Wayland-native input engine.

Primary target: **KDE Plasma (Wayland)**. The input engine itself is
compositor-agnostic; only the on-screen trail (layer-shell) and per-app gesture
detection are compositor-specific and live behind interfaces.

## Why a rewrite, not a port

Wayland deliberately forbids the four things X11 easystroke relied on: global
input grabbing, synthetic input injection, focused-window introspection, and
global overlays. There is no API swap that fixes this. The chosen approach:

- **Capture** — read input below the compositor via `libinput` (and later raw
  `evdev` with `EVIOCGRAB` to *suppress* the trigger button, the equivalent of
  X11's passive `XIGrabButton`).
- **Inject** — synthesize input via `/dev/uinput` (the XTest replacement).
- **Overlay** — draw the stroke trail with `layer-shell` (KDE + wlroots).
- **Per-app** — query the active window via KWin's DBus (KDE) / foreign-toplevel
  (wlroots). Optional; degrades gracefully where unavailable.

## Architecture

```
eswl_core (toolkit-free, daemon-friendly, the future privileged service)
  InputSource    libinput_source  — capture, normalize, track pointer position
  InputInjector  uinput_injector  — inject buttons / motion / wheel / keys
  button_map                      — evdev BTN_* <-> logical X-style buttons
  InputSink      (interface)      — where the gesture state machine plugs in

eswl-daemon
  main.cc — wires source -> sink -> injector over a poll() loop
```

The capture/inject core has no GUI dependency, so it drops into a small
privileged **systemd** service (see `packaging/`), with the GTK config UI talking
to it over IPC later.

## Build

```sh
cmake -S . -B build -G Ninja      # ASan + UBSan on by default
cmake --build build
./build/eswl-daemon --screen 2560x1440
```

Release packaging: `-DENABLE_ASAN=OFF`. Run the tests with `ctest --test-dir build`.

Dependencies: `libinput`, `libudev`, a C11/C++17 compiler, CMake ≥ 3.18.
**No Boost** — the X11 version used it for `shared_ptr`, `filesystem`, and
serialization; here those are `std::shared_ptr`, `std::filesystem`, and a
self-contained config format (TBD), so the engine doesn't break on Boost ABI
bumps and stays easy to package.

## Permissions

The engine needs to read `/dev/input/event*` and read-write `/dev/uinput`.
For user-space bring-up, install the udev rule and join the `input` group:

```sh
sudo cp packaging/99-easystroke-wayland.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG input "$USER"    # then re-login
```

The eventual privileged daemon (`packaging/easystroke-wayland.service`) won't
require the user to be in `input`.

## Status

Working:
- libinput capture: device discovery, pointer motion (rel + absolute/tablet),
  buttons, wheel; pointer position tracked client-side.
- uinput injection: relative motion, buttons, wheel, full key range.
- Recognition: C core (`stroke.c`) recycled verbatim, behind a Boost-free
  `Gesture` wrapper; `GestureRecognizer` captures a stroke while the trigger
  button is held and matches it on release. Unit-tested with synthetic strokes.
- Persistence: gestures saved/loaded as self-contained JSON (own tiny parser,
  no Boost, no vendored lib). `--record NAME` captures a stroke. Round-trip
  unit-tested.
- Actions: each gesture runs a key combo (`"key": "ctrl+t"`), types text
  (`"text": "..."`), or runs a shell command (`"command": "..."`). Key/text use
  an `xkbcommon` keymap (keysym/char → evdev keycode + mods) driving the uinput
  injector. Keymap resolution unit-tested.
- Trigger suppression (opt-in `--grab`): `EvdevSource` grabs mice with
  EVIOCGRAB and forwards their events through the injector, so a gesture-drag no
  longer also fires the button's normal action; a plain click is replayed. Only
  mice are grabbed — the keyboard stays free, so Ctrl-C always works, and the
  kernel releases the grab on exit. Decision logic (`TriggerGate`) is unit-tested.
- Clean build under `-Wall -Wextra -Wpedantic`; all 5 test suites + daemon run
  ASan/UBSan-clean.

### Testing grab mode safely

`--grab` takes exclusive control of your mice. Test it where you can recover:

```sh
# From a terminal you can reach with the keyboard alone:
./build/eswl-daemon --grab --button 3 --screen <W>x<H>
#   - right-click without moving  -> normal context menu (replayed click)
#   - hold right + draw a stroke  -> gesture (click suppressed)
#   - Ctrl-C in the terminal      -> stops and releases the mice instantly
```

If anything feels stuck, Ctrl-C (the keyboard is never grabbed). A crash also
releases the grab automatically. Known v0 gap: holding the trigger to *drag*
isn't passed through (no press-and-hold timeout yet).

Usage:

```sh
./build/eswl-daemon --record open-term     # draw a stroke, save it
# edit ~/.config/easystroke-wayland/gestures.json for that entry, e.g.:
#   "command": "xterm"       run a shell command
#   "key": "ctrl+shift+t"    send a key combo
#   "text": "hello"          type literal text
./build/eswl-daemon                         # draw it -> action fires
```

Next:
1. Live-test `--grab` on real hardware; add a press-and-hold timeout so holding
   the trigger to drag passes through.
2. layer-shell stroke-trail overlay.
3. KWin-DBus active-window backend for per-app gestures.
4. Privileged daemon + IPC to the config UI (GTK4 if/when the UI is rewritten).
5. Sync the keymap with the compositor's active layout (today it uses the
   system default, which is correct when they match).

## License

Inherits easystroke's ISC license (see upstream).
