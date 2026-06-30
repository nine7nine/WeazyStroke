# WeazyStroke -- Configuration & Process Model

This page documents the on-disk configuration: the self-contained JSON format and its schema, the legacy formats it still reads, how a save becomes a live reload, the daemon's signals and singleton lock, and how it supervises the overlay and tray as child processes.

## Table of Contents

1. [One file, hand-editable JSON](#1-one-file-hand-editable-json)
2. [The schema](#2-the-schema)
3. [Backward compatibility](#3-backward-compatibility)
4. [The JSON implementation](#4-the-json-implementation)
5. [Live reload](#5-live-reload)
6. [Signals and the singleton lock](#6-signals-and-the-singleton-lock)
7. [Process supervision](#7-process-supervision)
8. [Daemon command line](#8-daemon-command-line)

---

## 1. One file, hand-editable JSON

All persistent state lives in a single human-readable JSON file:

```
~/.config/easystroke-wayland/gestures.json     (honours $XDG_CONFIG_HOME)
```

`GestureConfig::default_path()` derives it from `XDG_CONFIG_HOME` (or `~/.config`). Everything the daemon needs is here — the trigger, the touch settings, the recognition and feedback knobs, and the gestures themselves with their recorded strokes and actions. The GUI reads and writes the same file; there is no separate database and no daemon-only hidden state. The design rule is that policy is a file you can read, diff, and edit by hand, and the daemon applies it live.

Two sibling files share the directory but are not part of the config proper: `gui.json` (the GUI's own appearance settings) and `history.jsonl` (the recognition log the daemon appends and the History tab reads).

## 2. The schema

The file is one object: a few top-level trigger fields, a nested `settings` object for the tunables, and a `gestures` array. Saving always writes the full, current schema.

```json
{
  "trigger_button": 10,
  "mode": "stylus",
  "trigger_modifiers": 0,
  "gate_button": 11,
  "touch_edge": "right",
  "settings": {
    "match_threshold": 0.6,
    "trace_width": 4,
    "pressure": true,
    "pressure_min": 2,
    "pressure_max": 14,
    "touch_pressure": true,
    "touch_pressure_floor": 150,
    "touch_pressure_ref": 500,
    "scroll_speed": 1.0,
    "scroll_invert": false,
    "show_osd": false,
    "trail_effect": "plain",
    "trail_fade_ms": 380,
    "trail_color_start": "#0000ff",
    "trail_color_end": "#00ff00",
    "touch_band": 30,
    "touch_cue": true,
    "touch_ring": 90,
    "touch_grow_ms": 450,
    "touch_out_ms": 220
  },
  "gestures": [
    {
      "name": "open-terminal",
      "type": "command",
      "argument": "kitty",
      "strokes": [
        [[820.0, 410.0], [835.0, 451.0], [861.0, 503.0]],
        [[818.0, 405.0], [840.0, 460.0], [869.0, 511.0]]
      ]
    }
  ]
}
```

| Top-level key | Type | Meaning |
| --- | --- | --- |
| `trigger_button` | number | logical trigger button (default `3`, right) |
| `mode` | string | activation mode label: `stylus` / `mouse` / `multitouch` |
| `trigger_modifiers` | number | required modifier bits for mouse mode (`Ctrl=1, Alt=2, Shift=4, Super=8`) |
| `gate_button` | number | second button that must be held (chord); `0` = none |
| `touch_edge` | string | anchor edge for two-finger touch (`none`/`left`/`right`/`top`/`bottom`) |
| `settings` | object | the tunables (see [Config GUI & Tray](config-gui.gen.html) for the full table) |
| `gestures` | array | the bound gestures |

Each gesture is `{ name, type, argument, strokes }`, where `strokes` is an **array of examples**, each example an array of `[x, y]` points. One gesture can carry several examples for sturdier matching ([Gesture Recognition](recognition.gen.html)); the action is the single `type`+`argument` pair ([Actions & Injection](actions.gen.html)).

## 3. Backward compatibility

Loading tolerates two older shapes so existing files keep working, folding each into the current model:

- **Typed vs. fielded actions.** If a gesture has no `type`, the loader looks for legacy `key` / `text` / `command` fields and folds the first present (in that priority) into `type`+`argument`.
- **Multi-stroke vs. single.** The current `strokes` array (multiple examples) is preferred; if absent, a legacy single `points` array is read as one example.

Saving always emits the current schema, so loading an old file and saving it once migrates it. Missing keys simply keep their defaults — the loader only overwrites a field when the file actually carries it, which also means a partial hand-edited file is valid.

## 4. The JSON implementation

The parser and serializer (`src/json.cc`, `src/json.h`) are hand-written and dependency-free — no Boost, no vendored blob. Scope is exactly what the config needs: objects, arrays, strings, numbers, bools, and null, with enough robustness to tolerate hand edits.

`json::Value` is a `std::variant` over those types, with typed accessors that throw on a mismatch and a chaining `operator[]` that returns a shared null `Value` for a missing key — so deep access like `root["settings"]["match_threshold"]` never throws on absence, it just reports "not a number" and the loader keeps the default. `dump()` pretty-prints with a configurable indent. The JSON layer is one of only two core files the GUI links directly (the other being `gesture_config.cc`), guaranteeing the editor and the engine read and write byte-identical files. It is unit-tested for round-trips, escapes, numbers, nesting, and malformed input (`tests/json_test.cc`).

## 5. Live reload

Edits apply without a restart. The flow is file + signal, not IPC:

<div class="diagram-container">
<svg width="100%" viewBox="0 0 920 220" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .box  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .boxc { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.4; }
    .boxe { fill: #1a2235; stroke: #7aa2f7; stroke-width: 1.4; }
    .sys  { fill: #1f2535; stroke: #565f89; stroke-width: 1; }
    .lbls { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut  { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .ttl  { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.4; fill: none; }
  </style>
  <rect x="0" y="0" width="920" height="220" class="bg"/>
  <text x="460" y="24" text-anchor="middle" class="ttl">save -> reload, with no restart</text>

  <rect x="20"  y="58" width="170" height="64" class="boxc"/>
  <text x="105" y="84" text-anchor="middle" class="lbls">eswl-config</text>
  <text x="105" y="102" text-anchor="middle" class="lbl-mut">edit + Save</text>

  <rect x="250" y="58" width="180" height="64" class="sys"/>
  <text x="340" y="84" text-anchor="middle" class="lbls">gestures.json</text>
  <text x="340" y="102" text-anchor="middle" class="lbl-mut">full schema written</text>

  <rect x="490" y="58" width="180" height="64" class="boxe"/>
  <text x="580" y="84" text-anchor="middle" class="lbls">eswl-daemon</text>
  <text x="580" y="102" text-anchor="middle" class="lbl-mut">g_reload set on SIGHUP</text>

  <rect x="720" y="58" width="180" height="64" class="box"/>
  <text x="810" y="80" text-anchor="middle" class="lbls">rebuild in place</text>
  <text x="810" y="97" text-anchor="middle" class="lbl-mut">clear + rebuild</text>
  <text x="810" y="111" text-anchor="middle" class="lbl-mut">bindings, re-apply</text>

  <line x1="190" y1="90" x2="250" y2="90" class="ln"/>
  <text x="220" y="82" text-anchor="middle" class="lbl-mut">write</text>
  <line x1="430" y1="78" x2="490" y2="78" class="ln"/>
  <text x="460" y="70" text-anchor="middle" class="lbl-mut">SIGHUP</text>
  <line x1="430" y1="104" x2="490" y2="104" class="ln"/>
  <text x="460" y="116" text-anchor="middle" class="lbl-mut">re-read</text>
  <line x1="670" y1="90" x2="720" y2="90" class="ln"/>

  <text x="460" y="170" text-anchor="middle" class="lbl-mut">the poll loop checks g_reload each tick (~50ms): re-load config, clear bindings, rebuild,</text>
  <text x="460" y="186" text-anchor="middle" class="lbl-mut">then re-apply threshold, trigger/gate/debounce, touch, and overlay settings live</text>
</svg>
</div>

`SIGHUP` sets an atomic `g_reload` flag; the daemon's poll loop notices it on the next tick and, inside a try/catch (so a malformed save can't crash the engine), re-loads the config and rebuilds everything that can change: the gesture bindings, the match threshold, the trigger/gate buttons and the pen-tip debounce, the touch edge/band and cue, the raw-touch pressure calibration, and the full overlay state (width, effect, fade, ring radius/timing, pressure range, colours). The GUI triggers this with `pkill -HUP -x eswl-daemon` after writing the file.

## 6. Signals and the singleton lock

The daemon is controlled entirely through Unix signals — there is no control socket — and protected by a single-instance lock.

| Signal | Handler | Effect |
| --- | --- | --- |
| `SIGINT` / `SIGTERM` | `on_signal` | clear the run flag → clean shutdown |
| `SIGHUP` | `on_reload` | set `g_reload` → live config reload |
| `SIGUSR1` | `on_toggle` | flip `g_disabled` → pause/resume actions |
| `SIGCHLD` | `SIG_IGN` | auto-reap forked commands (no zombies) |
| `SIGPIPE` | `SIG_IGN` | a dead overlay/tray pipe must never kill the daemon |

The **singleton lock** is a `flock(LOCK_EX|LOCK_NB)` on `$XDG_RUNTIME_DIR/weazystroke.lock` (falling back to `/tmp`), held open for the process lifetime. A second daemon would double every injected action, so if the lock is already held the new instance prints a notice and exits cleanly. The one-shot sub-modes (`--record`, `--capture-trigger`) skip the lock since they only monitor and exit. The tray has its own analogous lock (`weazystroke-tray.lock`).

## 7. Process supervision

The daemon owns the overlay and tray as child processes, each driven over a pipe to its stdin. Two small supervisors mirror each other:

| Supervisor | Child | Sends | Notes |
| --- | --- | --- | --- |
| `ProcessOverlay` | `eswl-overlay` | the trail line protocol | self-healing: respawns + replays sticky settings on write failure |
| `ProcessTray` | `eswl-tray` | `E` / `D` enabled state | one-way state push; tray signals back to the parent |

Both fork, wire the pipe's read end onto the child's stdin, and keep a line-buffered `FILE*` so each command flushes on its newline. On destruction they close the pipe (EOF tells the child to exit) and `SIGTERM` the child. The overlay supervisor adds the self-healing respawn described in [Stroke-Trail Overlay](overlay.gen.html); the tray supervisor is simpler because losing the tray is not visually disruptive. A third helper, `run_command`, is the fire-and-forget action launcher (fork + `setsid` + `/bin/sh -c`).

The daemon locates these siblings by reading `/proc/self/exe` and looking next to itself, so it finds `eswl-overlay` and `eswl-tray` whether running from the build tree or an install prefix.

## 8. Daemon command line

`eswl-daemon` takes its configuration from the file but exposes flags for overrides and the GUI's helper modes.

| Flag | Effect |
| --- | --- |
| `--config PATH` | use a specific config file (default: the XDG path) |
| `--record NAME` | capture one stroke, save it under `NAME`, and exit |
| `--capture-trigger` | print the next pressed button/chord (`CAPTURE trigger=N gate=N`) and exit |
| `--button N` | override the trigger button |
| `--screen WxH` | screen size for pointer tracking (default `1920x1080`) |
| `--threshold T` | override the match threshold floor |
| `--touch-edge E` | override the touch anchor edge |
| `--overlay` | spawn the live trail renderer |
| `--tray` | spawn the system-tray icon |
| `--grab` | grab mice (`EVIOCGRAB`) to suppress the trigger button |

The two helper modes are how the GUI talks to the engine without a socket: it shells out to `eswl-daemon --capture-trigger` for trigger learn-mode and reads the result from stdout, and the headless `--record` mirrors the GUI's in-window recorder. The autostart service runs `eswl-daemon --overlay --tray` so login brings up the engine, trail, and tray together. See [Build, Install & Packaging](install.gen.html) for the service and the device permissions these flags assume.
