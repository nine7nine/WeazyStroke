# WeazyStroke -- Stroke-Trail Overlay

This page documents the live trail: why it is a separate process, the line protocol the daemon streams to it, how the renderer self-heals across compositor restarts, the Cairo drawing pipeline behind Plain/Glow/Sparkle, and the touch cue and OSD.

## Table of Contents

1. [Why a separate process](#1-why-a-separate-process)
2. [The line protocol](#2-the-line-protocol)
3. [Sticky settings and self-healing](#3-sticky-settings-and-self-healing)
4. [The layer-shell window](#4-the-layer-shell-window)
5. [The drawing pipeline](#5-the-drawing-pipeline)
6. [Effects: Plain, Glow, Sparkle](#6-effects-plain-glow-sparkle)
7. [Completion retract](#7-completion-retract)
8. [The touch cue and the OSD](#8-the-touch-cue-and-the-osd)
9. [GNOME and the abstraction seam](#9-gnome-and-the-abstraction-seam)

---

## 1. Why a separate process

The trail is drawn by `eswl-overlay`, a standalone GTK4 + `gtk4-layer-shell` binary, *not* by the daemon. This keeps the input engine free of any GUI toolkit — the engine links no GTK at all — and means a crash in the renderer can never take down input capture. The daemon talks to it through the abstract `TraceOverlay` interface, whose only implementation, `ProcessOverlay` (`src/process_overlay.cc`), is a fork+exec of the binary plus a pipe to its stdin.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 940 270" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .boxe { fill: #1a2235; stroke: #7aa2f7; stroke-width: 1.5; }
    .boxo { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .box  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .lbls { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl  { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut  { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .blu  { fill: #7aa2f7; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .grn  { fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ttl  { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .bnd  { stroke: #6b7398; stroke-width: 1.2; stroke-dasharray: 6,4; fill: none; }
  </style>
  <rect x="0" y="0" width="940" height="270" class="bg"/>
  <text x="470" y="24" text-anchor="middle" class="ttl">daemon -> overlay: one pipe, a line protocol</text>

  <rect x="30" y="50" width="350" height="180" class="boxe"/>
  <text x="50" y="72" class="blu">eswl-daemon</text>
  <rect x="50" y="86" width="310" height="60" class="box"/>
  <text x="205" y="108" text-anchor="middle" class="lbls">GestureRecognizer</text>
  <text x="205" y="126" text-anchor="middle" class="lbl-mut">begin / add(x,y,p) / end / cue</text>
  <rect x="50" y="158" width="310" height="60" class="box"/>
  <text x="205" y="180" text-anchor="middle" class="lbls">ProcessOverlay (supervisor)</text>
  <text x="205" y="198" text-anchor="middle" class="lbl-mut">fork+exec, sticky cache, respawn</text>

  <line x1="380" y1="188" x2="560" y2="188" class="ln"/>
  <text x="470" y="180" text-anchor="middle" class="lbl-mut">pipe (stdin)</text>
  <text x="470" y="210" text-anchor="middle" class="lbl-mut">B P E W F C D A a R G M O</text>

  <rect x="560" y="50" width="350" height="180" class="boxo"/>
  <text x="580" y="72" class="grn">eswl-overlay</text>
  <rect x="580" y="86" width="310" height="60" class="box"/>
  <text x="735" y="108" text-anchor="middle" class="lbls">stdin_cb -> process_line</text>
  <text x="735" y="126" text-anchor="middle" class="lbl-mut">parse newline-framed commands</text>
  <rect x="580" y="158" width="310" height="60" class="box"/>
  <text x="735" y="180" text-anchor="middle" class="lbls">draw_cb (Cairo) on a layer-shell surface</text>
  <text x="735" y="198" text-anchor="middle" class="lbl-mut">click-through, full-screen, transparent</text>

  <line x1="470" y1="50" x2="470" y2="40" class="bnd"/>
  <text x="470" y="244" text-anchor="middle" class="lbl-mut">EOF on stdin or SIGTERM exits the renderer.</text>
  <text x="470" y="258" text-anchor="middle" class="lbl-mut">A write failure tears down the pipe; the next command respawns it.</text>
</svg>
</div>

## 2. The line protocol

The daemon streams newline-framed ASCII commands down the pipe; the renderer parses them in `process_line` and redraws. The protocol is tiny and split into two classes: **transient** commands that describe the current stroke or cue (replayed by drawing, never re-sent), and **sticky** settings that configure appearance (remembered and replayed after a respawn).

| Cmd | Args | Meaning | Class |
| --- | --- | --- | --- |
| `B` | — | begin a fresh stroke (clear the trail) | transient |
| `P` | `x y p` | append a point in screen space, pen pressure `p` (`<0` = none) | transient |
| `E` | — | end the stroke (animate it out) | transient |
| `A` | `x y` | show / move the touch "armed" ring at a point | transient |
| `a` | — | hide the armed ring (shrink + fade) | transient |
| `O` | `name` | flash a matched-gesture name (OSD) | transient |
| `W` | `px` | trail line width (the pressure-off width) | sticky |
| `F` | `id` | effect: `0` plain, `1` glow, `2` sparkle | sticky |
| `C` | `r0 g0 b0 r1 g1 b1` | gradient endpoints (start → end, `0..1`) | sticky |
| `D` | `ms` | completion fade-out duration | sticky |
| `R` | `px` | armed-ring radius | sticky |
| `G` | `grow out` | armed-ring grow-out and shrink/fade durations (ms) | sticky |
| `M` | `on min max` | pen pressure → width: enabled, min px, max px | sticky |

Points arrive in the daemon's screen-pixel space (set by `--screen WxH`); the renderer scales them to the real output size on every frame, so the daemon never needs to know the actual resolution. The recognizer emits `B` on trigger press, a `P` per motion sample (with the pen's pressure riding along), and `E` on release — the same accumulation that feeds recognition also feeds the trail.

## 3. Sticky settings and self-healing

`ProcessOverlay` keeps a map of the latest value of each sticky command, keyed by its command letter (`W F C D R G M`). Every appearance setter (`set_width`, `set_effect`, `set_colors`, …) records its line in that cache *and* sends it, via `send_sticky`. Transient stroke/cue commands are sent directly and never cached.

That cache is what makes the renderer **self-healing**. A compositor restart drops the overlay's Wayland connection, which surfaces as a write failure on the daemon's pipe. The flow:

1. The daemon ignores `SIGPIPE` process-wide, so a dead reader never kills it.
2. `raw_write` sees `fputs` fail, closes the pipe, and forgets the child (`drop_pipe`).
3. The next command calls `send`, which notices there is no pipe and calls `ensure_child`: re-fork the renderer, then `replay_config` — resend every sticky setting so the new renderer matches the daemon's current state.
4. `send` then performs one respawn-and-resend of the failed command itself, so it is not silently lost.

Without this, a single KWin restart would silently kill the trail until the whole daemon was restarted. With it, the trail simply reappears on the next stroke. On clean shutdown the supervisor closes the pipe (EOF makes the renderer exit its main loop) and `SIGTERM`s the child for good measure.

## 4. The layer-shell window

`eswl-overlay` (`src/overlay_main.cc`) creates a single full-screen window via `gtk4-layer-shell`, configured to sit above everything and intercept nothing:

| Property | Value | Why |
| --- | --- | --- |
| layer | `GTK_LAYER_SHELL_LAYER_OVERLAY` | above normal windows |
| anchors | all four edges | spans the whole output |
| exclusive zone | `-1` | never reserves space / shifts other windows |
| keyboard mode | `NONE` | never takes keyboard focus |
| namespace | `easystroke-trail` | identifiable to the compositor |
| input region | empty (`on realize`) | **click-through**: the compositor routes all pointer/pen input to whatever is underneath |

The window and drawing area are styled transparent via a CSS provider, so only the trail itself is visible. stdin is set non-blocking and watched with `g_unix_fd_add`; partial reads are buffered and split on newlines, so commands are processed as whole lines regardless of how the pipe chunks them. `SIGTERM`/`SIGINT` and stdin `EOF`/`HUP` all quit the GLib main loop.

## 5. The drawing pipeline

`draw_cb` redraws the whole surface each frame. It starts from a fully transparent clear, then renders the trail as a smooth, gradient-coloured, variable-width curve:

- **Smoothing.** Raw samples are joined with Catmull-Rom cubic curves (control points derived from each point's neighbours), so the trail flows instead of faceting between samples.
- **Per-segment width.** With pen pressure present (`M` enabled and a pressure value on the sample), each segment's width interpolates between `pressure_min` and `pressure_max` by the average pressure of its endpoints; otherwise it uses the constant `W` width. Mouse and touch (negative pressure) always use the constant width.
- **Onset taper.** Over the first few points the width ramps up from a hairline (a smoothstep envelope, floored at a visible `0.3` px), so every stroke eases in cleanly instead of popping to full width.
- **Direction gradient.** The colour eases from the start colour to the end colour along the stroke (`t = i/(n-1)`), defaulting to easystroke's blue→green direction cue. The `C` command sets both endpoints.

All of this is recomputed per frame from the point buffer, so a live stroke grows smoothly and a completing stroke can be re-parameterized (below) without rebuilding any geometry.

## 6. Effects: Plain, Glow, Sparkle

The `F` command selects one of three styles, all built from the same curve:

| Effect | `id` | Rendering |
| --- | --- | --- |
| **Plain** | `0` | a single gradient line — easystroke's classic look |
| **Glow** | `1` | two wide, faint underlay passes (`×4.6` and `×2.4` width at low alpha) beneath the line, for a soft bloom |
| **Sparkle** | `2` | the line plus sparse hard-edged square "pixels" (white / end-colour), antialiasing off, hashed on point index so they are deterministic and sparse |

Glow draws back-to-front (widest, faintest first) so the crisp line lands on top. Sparkle's squares step down in discrete sizes and thin out toward the tail (older points), giving an 8-bit shimmer that decays along the trail.

## 7. Completion retract

When a stroke ends (`E`), the trail does not blank — it animates away. The default is a *retract*: the line un-draws itself, its start point advancing toward the end over the fade so the trail appears to be reeled in. The animation is driven by a GTK tick callback (`fade_cb`) over `fade_ms` (the `D` setting; `0` clears instantly).

The retract front is a **fractional** position along the curve, not an integer point index, so it glides continuously instead of snapping point-to-point (which read as blocky on short strokes). At the front segment the cubic is split at the fractional position with de Casteljau's algorithm and only the remaining part is drawn. Sparkle handles completion by shrinking its squares to nothing as the transition ends. When the animation finishes the point buffers are cleared and the surface goes fully transparent again.

## 8. The touch cue and the OSD

Two more transient overlays share the same surface.

**Armed ring (touch cue).** While a two-finger touch gesture is armed, the daemon sends `A x y` to show an expanding ring at the held anchor finger, re-sending it as the finger drags so the ring follows. The ring grows out over `anchor_grow_ms`, holds as a halo under the fingertip, and on `a` (anchor lifted) shrinks and fades over `anchor_out_ms` from exactly whatever was on screen. Its colour follows the trail's start→end gradient, but alpha fades in fast and the green turn is delayed, so the blue start is actually visible and it flashes blue again on the way out. Radius and timing come from `R` and `G`. Under the Sparkle effect the ring is drawn as hard squares dotted around the circumference instead of a smooth arc.

**OSD.** The `O name` command flashes the matched gesture's name in a small rounded pill near the bottom-centre of the screen, auto-clearing after `1.1s`. The daemon sends it only on a match and only when the OSD setting is enabled. Newlines are stripped so a name always stays one command line.

## 9. GNOME and the abstraction seam

The trail is the *only* compositor-dependent part of WeazyStroke. `wlr-layer-shell` (via `gtk4-layer-shell`) is implemented by KDE/KWin and wlroots-based compositors, but **not** by GNOME's Mutter, and a normal Wayland window cannot be a click-through, always-on-top overlay. So on GNOME the engine, GUI, and actions all work — only the on-screen trail does not appear.

The fix is structural, not yet built: because the daemon already talks to the renderer behind the `TraceOverlay` abstraction over a swappable process + line-protocol interface, a GNOME backend that renders on the Shell stage (a Shell extension fed over DBus) would slot in with no engine changes. It is flagged as a future / community contribution. See the [Architecture Overview](architecture.gen.html) for where the overlay sits among the four processes, and [Configuration & Process Model](configuration.gen.html) for how `ProcessOverlay` is supervised alongside the tray.
