# WeazyStroke -- Actions & Injection

This page documents what happens after a stroke matches: the typed action model, how each type is bound to a callable, and the three ways an action reaches the system — a forked command, the `uinput` injector, or nothing at all.

## Table of Contents

1. [The typed action model](#1-the-typed-action-model)
2. [Binding a type to a callable](#2-binding-a-type-to-a-callable)
3. [The global enable toggle](#3-the-global-enable-toggle)
4. [The uinput injector](#4-the-uinput-injector)
5. [The keymap: keys and text](#5-the-keymap-keys-and-text)
6. [Buttons and scroll](#6-buttons-and-scroll)
7. [Running commands](#7-running-commands)

---

## 1. The typed action model

Each gesture binds exactly one action, modelled on easystroke's typed actions: a `type` string plus an `argument` whose meaning depends on the type. The pair lives in `GestureEntry` and is edited per-row in the GUI's Actions table.

| `type` | `argument` | Effect |
| --- | --- | --- |
| `command` | a shell command line | launch an app or script |
| `key` | a combo, e.g. `ctrl+shift+t` | send a key chord |
| `text` | literal text | type the text character by character |
| `button` | a button number, e.g. `2` | click that mouse button |
| `scroll` | `up`/`down`/`left`/`right` `[count]` | scroll in a direction |
| `ignore` | — | recognized, but deliberately does nothing |
| `misc` | `disable` | built-in: toggle WeazyStroke's enabled state |
| `""` | — | no action bound yet |

The model is intentionally flat — one type, one string — so it round-trips cleanly through JSON and stays editable by hand. A legacy format with separate `key`/`text`/`command` fields is still read on load and folded into `type`+`argument` (priority key > text > command), so older configs keep working. See [Configuration & Process Model](configuration.gen.html).

## 2. Binding a type to a callable

At startup and on every reload, `build_bindings` (`src/main.cc`) turns each `GestureEntry` into a `std::function<void()>` captured inside its `GestureBinding`. The recognizer never re-parses types at match time; it just calls the stored callable.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 960 300" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .box  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .boxh { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .sys  { fill: #1f2535; stroke: #565f89; stroke-width: 1; }
    .lbls { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut  { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .ttl  { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.4; fill: none; }
  </style>
  <rect x="0" y="0" width="960" height="300" class="bg"/>
  <text x="480" y="24" text-anchor="middle" class="ttl">action dispatch and its three exits</text>

  <rect x="30" y="50" width="170" height="80" class="boxh"/>
  <text x="115" y="84" text-anchor="middle" class="lbls">matched binding</text>
  <text x="115" y="102" text-anchor="middle" class="lbl-mut">stored callable</text>
  <text x="115" y="116" text-anchor="middle" class="lbl-mut">(built once)</text>

  <rect x="280" y="44" width="200" height="40" class="box"/>
  <text x="380" y="68" text-anchor="middle" class="lbls">command</text>
  <rect x="280" y="92" width="200" height="40" class="box"/>
  <text x="380" y="116" text-anchor="middle" class="lbls">key / text</text>
  <rect x="280" y="140" width="200" height="40" class="box"/>
  <text x="380" y="164" text-anchor="middle" class="lbls">button / scroll</text>
  <rect x="280" y="188" width="200" height="40" class="box"/>
  <text x="380" y="212" text-anchor="middle" class="lbls">ignore / misc</text>

  <rect x="600" y="44" width="330" height="40" class="sys"/>
  <text x="765" y="68" text-anchor="middle" class="lbl-mut">run_command: fork + setsid + /bin/sh -c</text>
  <rect x="600" y="100" width="330" height="72" class="sys"/>
  <text x="765" y="124" text-anchor="middle" class="lbl-mut">UinputInjector -> /dev/uinput</text>
  <text x="765" y="140" text-anchor="middle" class="lbl-mut">keymap resolves keys/text to keycodes;</text>
  <text x="765" y="156" text-anchor="middle" class="lbl-mut">buttons/scroll emit EV_KEY / EV_REL</text>
  <rect x="600" y="188" width="330" height="40" class="sys"/>
  <text x="765" y="212" text-anchor="middle" class="lbl-mut">no-op, or flip the enabled flag</text>

  <line x1="200" y1="84" x2="280" y2="64" class="ln"/>
  <line x1="200" y1="96" x2="280" y2="112" class="ln"/>
  <line x1="200" y1="104" x2="280" y2="160" class="ln"/>
  <line x1="200" y1="116" x2="280" y2="208" class="ln"/>

  <line x1="480" y1="64" x2="600" y2="64" class="ln"/>
  <line x1="480" y1="112" x2="600" y2="130" class="ln"/>
  <line x1="480" y1="160" x2="600" y2="140" class="ln"/>
  <line x1="480" y1="208" x2="600" y2="208" class="ln"/>

  <text x="480" y="268" text-anchor="middle" class="lbl-mut">key/text need an XKB keymap; button/scroll/command need the injector — if a</text>
  <text x="480" y="284" text-anchor="middle" class="lbl-mut">dependency is missing the binding logs a warning and becomes a harmless no-op</text>
</svg>
</div>

The build is defensive: each type is bound only if its dependencies exist. A `key` binding needs both the injector and a compiled keymap that can resolve the combo; a `text` binding needs the injector and a working keymap; `button`/`scroll` need the injector. If any prerequisite is missing, `build_bindings` prints a warning and the gesture falls back to a no-op that prints a notice — recognition still reports the match, it just has nothing to do.

| Type | Bound to |
| --- | --- |
| `command` | `[cmd]{ run_command(cmd); }` |
| `key` | `[inj, ks]{ send_keystroke(*inj, ks); }` after `keymap.from_combo(arg, ks)` |
| `text` | `[inj, &keymap, text]{ type_text(keymap, *inj, text); }` |
| `button` | `[inj, b]{ inj->click(b); }` for button number `b` |
| `scroll` | `[inj, dx, dy]{ inj->scroll(dx, dy); inj->flush(); }` after speed/invert scaling |
| `ignore` | `[]{}` |
| `misc` `disable` | `[]{ g_disabled = !g_disabled; }` |

## 3. The global enable toggle

WeazyStroke can be paused without quitting. A single atomic flag, `g_disabled`, gates every action. When set, recognized gestures still match and report — but their actions are suppressed. The one exception is the `misc disable` toggle itself, so you can always gesture your way back to enabled.

This is enforced structurally: `build_bindings` wraps every *non-misc* action in `[a]{ if (!g_disabled) a(); }`. The flag is flipped from three places, all converging on the same atomic:

- a `misc disable` gesture (handled in-band),
- `SIGUSR1`, raised by the GUI's Pause/Resume button (`pkill -USR1 -x eswl-daemon`) and by the tray's "Enabled" toggle (`kill(getppid(), SIGUSR1)`),
- mirrored down to the tray whenever it changes, so the tray checkmark and tooltip stay in sync.

## 4. The uinput injector

`UinputInjector` (`src/uinput_injector.cc`) is the only `InputInjector` implementation — the Wayland replacement for X11's `XTest`. On construction it opens `/dev/uinput` and creates one virtual device:

- event types `EV_KEY`, `EV_REL`, `EV_SYN`;
- relative axes `REL_X`, `REL_Y`, `REL_WHEEL`, `REL_HWHEEL`;
- the entire key/button code range (`1 .. KEY_MAX`), which covers `BTN_*` pointer buttons as well as keyboard keys;
- bus `BUS_VIRTUAL`, and a fixed device name, `kDeviceName`.

The fixed name matters: the grab-mode `EvdevSource` checks it so it never grabs WeazyStroke's own output device and feed itself a loop. The primitives are deliberately small — `move_relative`, `button`, `scroll`, `key`, `flush` — with `click` and `tap` as convenience helpers built on top. Every emit writes a raw `input_event` and the kernel timestamps it on receipt.

| Primitive | Emits |
| --- | --- |
| `move_relative(dx, dy)` | `REL_X`/`REL_Y` with sub-pixel accumulation (fractional remainder carried) |
| `button(logical, pressed)` | wheel buttons → a scroll detent; otherwise `EV_KEY` of the mapped `BTN_*` |
| `scroll(dx, dy)` | `REL_HWHEEL`/`REL_WHEEL` detents |
| `key(keycode, pressed)` | `EV_KEY` of a raw evdev keycode |
| `flush()` | a bare `SYN_REPORT` |

In grab mode the injector does double duty: besides running gesture actions, it forwards the grabbed pointer's own motion, wheel, and non-trigger buttons so the mouse keeps working while devices are grabbed, and replays the trigger as a real click when the gate decides it was not a gesture.

## 5. The keymap: keys and text

Key and text actions need to turn human-meaningful symbols into evdev keycodes. `Keymap` (`src/keymap.cc`) does that with `xkbcommon`, compiling the system default XKB layout (honoring the `XKB_DEFAULT_*` environment) and building a cache from keysym to `KeyStroke` (an evdev keycode plus the modifier flags needed to reach it).

The cache is built by walking every keycode and level in the layout. For each level it records which modifiers (`Shift`, `AltGr`/Level3) reach it, converting X11/XKB keycodes back to evdev by subtracting the well-known offset of `8`. First insertion wins, so the *simplest* way to produce a symbol (fewest modifiers, lowest level) is preferred.

Three resolution entry points:

- **`from_combo("ctrl+shift+t")`** — splits on `+`, recognizes modifier words (`ctrl`/`control`, `shift`, `alt`, `super`/`meta`/`win`/`cmd`/`logo`, `altgr`/`iso_level3_shift`), and resolves the last non-modifier token as a key name via `xkb_keysym_from_name`. Combo semantics use the *explicit* modifiers only.
- **`from_char(codepoint)`** — maps a Unicode code point to a keysym (`xkb_utf32_to_keysym`) and then to a stroke, including whatever modifiers the layout needs (e.g. Shift for an uppercase letter).
- **`from_keysym(keysym)`** — the cache lookup underneath both.

`send_keystroke` presses the low-level modifiers first (Ctrl, Alt, Super, AltGr, Shift), taps the key, then releases the modifiers in reverse order, and flushes. `type_text` decodes the argument UTF-8 byte stream one code point at a time and sends each as its own keystroke, skipping anything the layout can't produce.

> **Layout caveat.** Injection happens *below* the compositor, which re-applies its own layout to the injected keycodes. For text to come out right, the daemon's layout should match the compositor's active layout. Syncing to the compositor's live layout is a later refinement; the system default is a sound v0, and if no keymap compiles at all (e.g. `xkeyboard-config` missing) key/text actions are simply disabled with a warning.

## 6. Buttons and scroll

A `button` action clicks a logical button: `inj->click(b)` presses and releases it. Wheel "buttons" (logical 4–7) are special-cased inside the injector — they act on press only and emit a scroll detent rather than a key event, so binding "wheel up" as a button still scrolls.

A `scroll` action parses its argument with `parse_scroll`: a direction word plus an optional count (default `3`). `up`/`down` map to vertical detents, `left`/`right` to horizontal, with `+y` up and `+x` right. Two settings then scale the result before it is bound: `scroll_speed` multiplies the detent count and `scroll_invert` flips the sign. The scaled deltas are baked into the binding's callable, so changing either setting takes effect on the next reload.

```
"down"      -> dy = -3
"up 5"      -> dy = +5
"right 2"   -> dx = +2     (then * scroll_speed, * (invert ? -1 : 1))
```

## 7. Running commands

A `command` action runs a shell command without blocking the engine. `run_command` (`src/process.cc`) forks, calls `setsid()` in the child to detach it from the daemon's session, and `exec`s `/bin/sh -c <command>`. The parent returns immediately. The daemon sets `SIGCHLD` to `SIG_IGN`, so finished command processes are auto-reaped and never become zombies.

This is the most open-ended action type — anything you can run in a shell, a gesture can launch. Combined with `key`/`text`/`button`/`scroll` it covers the full easystroke action surface, and `ignore` lets a shape be deliberately claimed (recognized but inert) so it does not fall through to a near neighbour. See [Gesture Recognition](recognition.gen.html) for how the winning binding is chosen, and the [Config GUI & Tray](config-gui.gen.html) for the per-type argument editors that produce these strings.
