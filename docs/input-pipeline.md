# WeazyStroke -- Input Capture & Triggers

This page covers how raw device input becomes a stroke: the source/sink seam, the three input sources, how every device class is folded onto one logical button scheme, and the two pure-logic gates that decide when a hold is a gesture.

## Table of Contents

1. [The source/sink seam](#1-the-sourcesink-seam)
2. [Logical buttons and devices](#2-logical-buttons-and-devices)
3. [LibinputSource: the default monitor](#3-libinputsource-the-default-monitor)
4. [EvdevSource: grab and the trigger gate](#4-evdevsource-grab-and-the-trigger-gate)
5. [TouchEvdevSource: contact size](#5-touchevdevsource-contact-size)
6. [The touch gate: two-finger edge gestures](#6-the-touch-gate-two-finger-edge-gestures)
7. [Pen quirks: proximity, chords, chatter](#7-pen-quirks-proximity-chords-chatter)
8. [The poll loop](#8-the-poll-loop)

---

## 1. The source/sink seam

The engine is split at one clean interface so gesture logic never knows what kind of device drew the stroke. An **`InputSource`** produces normalized events; an **`InputSink`** consumes them. The daemon wires concrete sources to one sink — the `GestureRecognizer`.

```
class InputSource {            class InputSink {
    int  fd() const;               on_button(Button, bool pressed, Sample);
    void dispatch();               on_motion(Sample, dx, dy);
};                                 on_scroll(dx, dy, Sample);
                                   on_touch_down/motion/up(slot, Sample);
                                   on_modifiers(mask);
                               };
```

Everything crossing that seam is expressed as a `Sample` — the timestamped pointer triple plus pen pressure:

```
struct Sample { double x, y; uint32_t time_ms; double pressure = -1.0; };
```

`pressure` is `0..1` for a stylus and **negative** for mouse and touch, so only real pen strokes drive pressure-sensitive trail width. `x`/`y` are in the daemon's screen-pixel space (default `1920×1080`, set by `--screen`); a source is responsible for mapping its device coordinates into that space. Because Wayland never tells a client where the cursor is, each source tracks the pointer position itself.

This seam is also why the recognizer is testable: a unit test plays `on_button`/`on_motion` calls directly, no hardware required.

## 2. Logical buttons and devices

Mouse buttons, wheel directions, and pen tools are all collapsed onto one logical numbering — kept identical to easystroke's X11 convention so the recognition logic and config stay compatible. The mapping lives in `src/button_map.cc`.

| Logical | Source | evdev code | Notes |
| --- | --- | --- | --- |
| `1` / `2` / `3` | mouse | `BTN_LEFT` / `BTN_MIDDLE` / `BTN_RIGHT` | primary buttons |
| `4` / `5` / `6` / `7` | wheel | — | wheel up / down / left / right (not physical keys) |
| `8` / `9` | mouse | `BTN_SIDE`/`BTN_BACK` / `BTN_EXTRA`/`BTN_FORWARD` | back / forward thumb buttons |
| `10` (`kPenTip`) | pen | `BTN_TOUCH` | tip in contact with the surface |
| `11` (`kPenButton`) | pen | `BTN_STYLUS` | primary barrel/side button |
| `12` (`kPenButton2`) | pen | `BTN_STYLUS2` | secondary barrel button |

Pen tools sit *above* the 1–9 mouse range so a stylus event drives the same trigger machinery as a mouse button without ever colliding with one. Wheel "buttons" 4–7 are special: as triggers they are ignored, and as *injected* actions they become scroll events rather than key presses (see [Actions & Injection](actions.gen.html)). `evdev_to_logical` and `logical_to_evdev` translate both directions; `is_wheel_button` flags the 4–7 range.

## 3. LibinputSource: the default monitor

`LibinputSource` (`src/libinput_source.cc`) is the default and the only source used unless `--grab` is passed. It opens devices through libinput's udev backend on `seat0`, in **monitor mode**: it does not grab anything, so the compositor still receives every event. The upshot is that in this mode the trigger button *also* performs its normal click — capture rides alongside, it does not intercept.

It translates libinput's event stream into the sink's vocabulary:

| libinput event | Handling |
| --- | --- |
| `POINTER_MOTION` | integrate relative `dx/dy` into the tracked position, clamp to screen, emit `on_motion` |
| `POINTER_MOTION_ABSOLUTE` | map absolute position to screen, emit `on_motion` |
| `POINTER_BUTTON` | map to logical button, emit `on_button` |
| `POINTER_SCROLL_WHEEL` | convert v120 deltas to detents, emit `on_scroll` |
| `TABLET_TOOL_AXIS` | absolute pen motion (+ pressure) → `on_motion` |
| `TABLET_TOOL_TIP` / `_BUTTON` | pen tip / barrel as logical pen buttons → `on_button` |
| `TABLET_TOOL_PROXIMITY` | on proximity-out, synthesize releases for any held pen buttons |
| `TOUCH_DOWN` / `_MOTION` / `_UP` | per-slot touch events → `on_touch_*` (unless `skip_touch_`) |
| `KEYBOARD_KEY` | observe modifier keys only; update the modifier mask |

A tablet tool is an absolute device, so each event carries the pen's mapped position; the source only overwrites an axis that actually changed, so a button-only event keeps the last known position. Keyboard keys are *observed, never consumed* — the source tracks Ctrl/Alt/Shift/Super state and forwards it via `on_modifiers` so the recognizer can gate the trigger on a modifier (mouse mode, e.g. Super+click), while the keys themselves pass straight through to the compositor.

## 4. EvdevSource: grab and the trigger gate

The default monitor mode cannot *suppress* the trigger — a right-drag gesture also fires a right-click. `EvdevSource` (`src/evdev_source.cc`), enabled with `--grab`, is the answer, and the Wayland equivalent of X11's passive button grab. It requires the injector, because suppression only works if a real click can be replayed.

It scans `/dev/input`, and for every device that looks like a mouse — a relative pointer with buttons that carries the trigger code, and is not WeazyStroke's own injected device — it takes an exclusive `EVIOCGRAB` and adds its fd to an epoll set. Only mice are grabbed; the keyboard stays free, so `Ctrl-C` always works, and the kernel drops every grab when the process exits. Non-trigger buttons and motion are forwarded straight through the injector so the pointer keeps working normally while grabbed.

The decision of whether a held trigger was a gesture or a plain click is made by a tiny pure-logic state machine, `TriggerGate` (`src/trigger_gate.h`), kept separate and unit-tested:

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 220" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .box  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .boxg { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.4; }
    .boxr { fill: #2a1f24; stroke: #f7768e; stroke-width: 1.4; }
    .lbls { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut  { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .grn  { fill: #9ece6a; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .red  { fill: #f7768e; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .ttl  { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.4; fill: none; }
  </style>
  <rect x="0" y="0" width="900" height="220" class="bg"/>
  <text x="450" y="24" text-anchor="middle" class="ttl">TriggerGate: suppress vs replay (grab mode)</text>

  <rect x="20"  y="58" width="150" height="64" class="box"/>
  <text x="95"  y="84" text-anchor="middle" class="lbls">idle</text>
  <text x="95"  y="102" text-anchor="middle" class="lbl-mut">withholding = false</text>

  <rect x="250" y="58" width="180" height="64" class="box"/>
  <text x="340" y="84" text-anchor="middle" class="lbls">withholding</text>
  <text x="340" y="102" text-anchor="middle" class="lbl-mut">press held; track travel</text>

  <rect x="540" y="24" width="200" height="56" class="boxr"/>
  <text x="640" y="48" text-anchor="middle" class="lbls">ReplayClick</text>
  <text x="640" y="66" text-anchor="middle" class="red">travel &lt; 16px -> inject click</text>

  <rect x="540" y="104" width="200" height="56" class="boxg"/>
  <text x="640" y="128" text-anchor="middle" class="lbls">Swallow</text>
  <text x="640" y="146" text-anchor="middle" class="grn">travel &gt;= 16px -> gesture</text>

  <line x1="170" y1="90" x2="250" y2="90" class="ln"/>
  <text x="210" y="82" text-anchor="middle" class="lbl-mut">press</text>
  <line x1="430" y1="80" x2="540" y2="56" class="ln"/>
  <line x1="430" y1="100" x2="540" y2="126" class="ln"/>
  <text x="490" y="74" text-anchor="middle" class="lbl-mut">release</text>

  <text x="450" y="196" text-anchor="middle" class="lbl-mut">the trigger press is withheld from the compositor until release; then it is either</text>
  <text x="450" y="210" text-anchor="middle" class="lbl-mut">swallowed (a gesture was drawn) or replayed as a real click — decided purely by travel</text>
</svg>
</div>

On press the gate withholds the click and records the origin; motion updates the maximum travel from origin; on release it returns `Swallow` if travel reached `16px` (`kGestureMinTravel`) or `ReplayClick` otherwise, in which case `EvdevSource` injects a real press+release so the application still gets its click. A known v0 limitation: there is no press-and-hold timeout, so holding the trigger to drag does not pass through until release — acceptable for the usual right/extra-button triggers.

## 5. TouchEvdevSource: contact size

libinput's touch API hides one signal WeazyStroke wants: the contact area (`ABS_MT_TOUCH_MAJOR`), which can drive trail thickness the way pen pressure does. So when touch gestures *and* touch-pressure are enabled, the daemon opens the touchscreen a second time, directly, via `TouchEvdevSource` (`src/touch_evdev_source.cc`).

It scans `/dev/input` for a *direct* (on-screen) multitouch device — one with `INPUT_PROP_DIRECT` and absolute MT position + tracking — preferring one that also reports `ABS_MT_TOUCH_MAJOR`. It then parses the MT slot protocol (type B) itself: per-slot tracking ID, position, and contact major, flushed on each `SYN_REPORT` into `on_touch_down/motion/up` calls carrying a pseudo-pressure.

The contact size becomes pressure through a fixed band, not a running maximum, so variation *within* a stroke shows:

| Knob (config) | Role |
| --- | --- |
| `touch_pressure_floor` | `TOUCH_MAJOR` at/below which the trail is thinnest (default `150`) |
| `touch_pressure_ref` | `TOUCH_MAJOR` mapped to full width (default `500`; `0` = device max) |

The narrow real range of contact variation is stretched across the width range, so the thin/thick effect is pronounced; both knobs are device-specific and live-tunable. This source reads **read-only** (the compositor still gets the touches), so it must coexist with libinput. To avoid delivering every touch twice, the daemon calls `LibinputSource::set_skip_touch(true)` whenever the raw source is active, and falls back to libinput's touch if the raw source can't open. `ESWL_DEBUG_TOUCH=1` enables verbose per-contact tracing for calibration.

## 6. The touch gate: two-finger edge gestures

Touch gestures use a second pure-logic state machine, `TouchGate` (`src/touch_gate.h`), to separate "the finger that arms" from "the finger that draws" — so an ordinary one-finger swipe is never mistaken for a gesture.

The interaction: the first finger that lands within `touch_band` px of the configured screen edge becomes the **anchor** (it is held; its own path is ignored, though it may drag the visual cue). The next finger down is the **draw** finger, whose path is the stroke. A third finger is ignored. Lifting the draw finger finalizes the stroke and leaves the anchor armed for another; lifting the anchor ends the session — cancelling an in-progress stroke if one was being drawn.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 250" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .box  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .boxh { fill: #2a2438; stroke: #e0af68; stroke-width: 1.4; }
    .lbls { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut  { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .ttl  { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.4; fill: none; }
  </style>
  <rect x="0" y="0" width="900" height="250" class="bg"/>
  <text x="450" y="24" text-anchor="middle" class="ttl">TouchGate state machine</text>

  <rect x="30"  y="60" width="150" height="58" class="box"/>
  <text x="105" y="84" text-anchor="middle" class="lbls">idle</text>
  <text x="105" y="102" text-anchor="middle" class="lbl-mut">no anchor yet</text>

  <rect x="250" y="60" width="170" height="58" class="boxh"/>
  <text x="335" y="84" text-anchor="middle" class="lbls">armed</text>
  <text x="335" y="102" text-anchor="middle" class="lbl-mut">anchor held in band</text>

  <rect x="490" y="60" width="170" height="58" class="box"/>
  <text x="575" y="84" text-anchor="middle" class="lbls">drawing</text>
  <text x="575" y="102" text-anchor="middle" class="lbl-mut">2nd finger path</text>

  <rect x="730" y="60" width="150" height="58" class="box"/>
  <text x="805" y="84" text-anchor="middle" class="lbls">finalize</text>
  <text x="805" y="102" text-anchor="middle" class="lbl-mut">match stroke</text>

  <line x1="180" y1="89" x2="250" y2="89" class="ln"/>
  <text x="215" y="81" text-anchor="middle" class="lbl-mut">edge down</text>
  <line x1="420" y1="89" x2="490" y2="89" class="ln"/>
  <text x="455" y="81" text-anchor="middle" class="lbl-mut">2nd down</text>
  <line x1="660" y1="89" x2="730" y2="89" class="ln"/>
  <text x="695" y="81" text-anchor="middle" class="lbl-mut">draw up</text>

  <line x1="805" y1="118" x2="805" y2="150" class="ln"/>
  <line x1="805" y1="150" x2="335" y2="150" class="ln"/>
  <line x1="335" y1="150" x2="335" y2="118" class="ln"/>
  <text x="560" y="144" text-anchor="middle" class="lbl-mut">anchor stays armed -> draw another without re-anchoring</text>

  <text x="450" y="196" text-anchor="middle" class="lbl-mut">anchor up mid-stroke -> Cancel (drop trail)  ·  anchor up while idle -> EndSession</text>
  <text x="450" y="216" text-anchor="middle" class="lbl-mut">a finger landing away from the edge with no anchor held is ignored and never promoted</text>
  <text x="450" y="236" text-anchor="middle" class="lbl-mut">default edge is the right edge (left/top/bottom often taken by compositor edge swipes)</text>
</svg>
</div>

`TouchGate::on_down` returns `Anchor`, `Draw`, or `Ignore`; `on_up` returns `Finalize`, `Cancel`, `EndSession`, or `Ignore`. The recognizer uses those return codes to start/append/finalize the stroke and to show or hide the overlay's "armed" ring cue. The gate is configured with the edge, screen size, and band width, and is exhaustively unit-tested (`tests/touch_gate_test.cc`) without any hardware.

## 7. Pen quirks: proximity, chords, chatter

A stylus is not a mouse, and three behaviours in `LibinputSource` and the recognizer exist specifically to make pen gestures feel right.

- **Proximity-out releases.** When the pen leaves proximity the hardware stops reporting buttons, so a tip or barrel button still "held" would leave a gesture open forever. The source tracks which pen buttons are down and, on proximity-out, synthesizes their releases in press order — the common "drew the gesture, then lifted the pen away" case finalizes cleanly.
- **Tip + side-button chord.** A "gate button" can be required: the trigger only starts a gesture while a second button (e.g. the pen side button) is *already held*, and releasing the gate ends an in-progress stroke. This frees the side button alone for other uses, like a right-click. The recognizer tracks `gate_held_` and consults it on every trigger press.
- **Tip debounce.** The pen tip (`BTN_TOUCH`) chatters under light pressure — rapid release/press flicker that would otherwise split one stroke into several. With a debounce window (`120ms` when the trigger is the tip), a release is deferred; a re-press inside the window resumes the same stroke, and only a release that stays released past the deadline finalizes. The daemon's poll loop calls `recognizer.tick()` to fire these deferred finalizations.

## 8. The poll loop

The daemon drives everything from one `poll()` loop over each source's fd (`run_loop` in `src/main.cc`). Touch contact-size adds a second fd (the raw touchscreen) alongside libinput, so the loop polls every source. A short `50ms` timeout makes the loop tick even when idle, so the `keep_going` predicate runs often — that predicate is where the debounced pen finalization, the live config reload (`SIGHUP`), and tray state mirroring happen between input events.

The same loop is reused, with a different predicate, for the daemon's two one-shot modes: `--record NAME` captures a single stroke and exits, and `--capture-trigger` prints the next pressed button or chord (used by the GUI's "Set…" trigger learn-mode) and exits. Both run monitor-only, with no singleton lock, overlay, or tray. See [Configuration & Process Model](configuration.gen.html) for those sub-modes and [Actions & Injection](actions.gen.html) for what happens after a stroke matches.
