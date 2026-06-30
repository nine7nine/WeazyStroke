# WeazyStroke -- Gesture Recognition

This page documents the recognition engine: how a raw path becomes a normalized stroke, how two strokes are compared by the recycled `stroke.c` core, how the cost becomes a score, and how multi-example templates and the match threshold turn that score into a decision.

## Table of Contents

1. [What recognition operates on](#1-what-recognition-operates-on)
2. [Normalization: stroke_finish](#2-normalization-stroke_finish)
3. [Comparison: stroke_compare](#3-comparison-stroke_compare)
4. [From cost to score](#4-from-cost-to-score)
5. [Multi-example templates](#5-multi-example-templates)
6. [The recognizer's decision](#6-the-recognizers-decision)
7. [What is verified](#7-what-is-verified)

---

## 1. What recognition operates on

Recognition is shape matching, not pixel matching. The engine never compares where you drew or how big — only the *shape* of the path. That property comes from the recognition core, `src/stroke.c`, recycled verbatim from easystroke (X11). It is pure C with no dependencies, wrapped by a thin C++ class, `Gesture` (`src/gesture.{cc,h}`), so the rest of the engine never touches the C API directly.

A captured stroke arrives as a `std::vector<Point>` — the raw `(x, y)` samples accumulated while the trigger was held. Three things happen to it:

1. **Validity.** Fewer than three points is not a stroke (`Gesture::from_points` returns an invalid gesture, mirroring easystroke's `PreStroke::valid()` rule of "more than two points").
2. **Normalization.** The points are loaded into a `stroke_t` and `stroke_finish` rewrites them into a scale- and translation-invariant representation (below).
3. **Comparison.** `stroke_compare` measures the dissimilarity between the candidate and a stored template, returning a cost.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 980 232" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg   { fill: #1a1b26; }
    .box  { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .boxh { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .lbls { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut  { fill: #8c92b3; font-size: 9px; font-family: 'JetBrains Mono', monospace; }
    .ttl  { fill: #7aa2f7; font-size: 13px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln   { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
  </style>
  <rect x="0" y="0" width="980" height="232" class="bg"/>
  <text x="490" y="24" text-anchor="middle" class="ttl">stroke.c data lifecycle</text>

  <rect x="14"  y="50" width="170" height="86" class="box"/>
  <text x="99"  y="72" text-anchor="middle" class="lbls">raw points</text>
  <text x="99"  y="90" text-anchor="middle" class="lbl-mut">vector&lt;Point&gt;</text>
  <text x="99"  y="104" text-anchor="middle" class="lbl-mut">screen pixels</text>
  <text x="99"  y="122" text-anchor="middle" class="lbl-mut">stroke_add_point</text>

  <rect x="212" y="50" width="190" height="86" class="boxh"/>
  <text x="307" y="72" text-anchor="middle" class="lbls">stroke_finish</text>
  <text x="307" y="90" text-anchor="middle" class="lbl-mut">arc-length t in [0,1]</text>
  <text x="307" y="104" text-anchor="middle" class="lbl-mut">center + scale to fit</text>
  <text x="307" y="122" text-anchor="middle" class="lbl-mut">per-segment angle</text>

  <rect x="430" y="50" width="190" height="86" class="box"/>
  <text x="525" y="72" text-anchor="middle" class="lbls">stroke_compare</text>
  <text x="525" y="90" text-anchor="middle" class="lbl-mut">constrained DP over</text>
  <text x="525" y="104" text-anchor="middle" class="lbl-mut">direction difference</text>
  <text x="525" y="122" text-anchor="middle" class="lbl-mut">-> cost in [0, 0.2]</text>

  <rect x="648" y="50" width="170" height="86" class="box"/>
  <text x="733" y="72" text-anchor="middle" class="lbls">Gesture::compare</text>
  <text x="733" y="90" text-anchor="middle" class="lbl-mut">score =</text>
  <text x="733" y="104" text-anchor="middle" class="lbl-mut">max(1 - 2.5c, 0)</text>
  <text x="733" y="122" text-anchor="middle" class="lbl-mut">in [0, 1]</text>

  <rect x="846" y="50" width="120" height="86" class="box"/>
  <text x="906" y="72" text-anchor="middle" class="lbls">decision</text>
  <text x="906" y="90" text-anchor="middle" class="lbl-mut">best vs</text>
  <text x="906" y="104" text-anchor="middle" class="lbl-mut">threshold</text>
  <text x="906" y="122" text-anchor="middle" class="lbl-mut">match?</text>

  <line x1="184" y1="93" x2="212" y2="93" class="ln"/>
  <line x1="402" y1="93" x2="430" y2="93" class="ln"/>
  <line x1="620" y1="93" x2="648" y2="93" class="ln"/>
  <line x1="818" y1="93" x2="846" y2="93" class="ln"/>

  <text x="490" y="172" text-anchor="middle" class="lbl-mut">templates are normalized once at load; the candidate is normalized once per stroke;</text>
  <text x="490" y="188" text-anchor="middle" class="lbl-mut">comparison runs candidate against every template and keeps the best score</text>
</svg>
</div>

## 2. Normalization: stroke_finish

`stroke_finish` is the heart of the invariance. It rewrites the point array in place so that two strokes of the same shape — drawn anywhere, at any size — end up nearly identical. It runs in four passes.

**Arc-length parameterization.** Each point is assigned a time `t` equal to the cumulative straight-line distance from the start, then the whole array is divided by the total length. So `t` runs from 0 at the first point to 1 at the last, spaced by *distance travelled* rather than by *sample index*. This is what makes recognition largely independent of drawing speed: a slow stretch and a fast stretch of the same curve land at the same `t`.

**Centering and scaling.** The bounding box of the points is computed, and every point is recentred on the box midpoint and divided by `scale = max(width, height)`, then offset by `0.5`. The single shared scale (not separate x and y) preserves aspect ratio, so a tall narrow "L" never gets squashed into a square. A degenerate stroke (scale `< 0.001`) falls back to scale `1` to avoid division blow-up.

**Per-segment direction.** For each segment `i → i+1` the code stores two derived quantities:

| Field | Definition | Meaning |
| --- | --- | --- |
| `dt` | `t[i+1] - t[i]` | the fraction of total arc length this segment spans |
| `alpha` | `atan2(dy, dx) / π` | the segment's heading, normalized to `(-1, 1]` |

After `stroke_finish` a stroke is, in effect, a **function from normalized arc length to heading** — a sequence of directions, each tagged with how much of the path it covers. Position and size are gone; only the turning of the curve remains. The helper `angle_difference` treats this heading as circular (it wraps at `±1`), so the jump from `+0.99` to `-0.99` is correctly read as a small turn, not a near-reversal.

## 3. Comparison: stroke_compare

`stroke_compare(a, b, …)` returns a single non-negative cost: small means similar, and a value at or above `stroke_infinity` (`0.2`) means "not comparable". Conceptually it minimizes

> the integral, over the shared arc-length parameter, of the **square of the angle difference** between the two strokes' headings,

over all reparametrizations whose local slope stays between `1/2` and `2`. That slope constraint is the key idea: it lets the two strokes be matched even if one lingers in a region the other rushes through, but forbids matching wildly different amounts of one curve to the other. The original easystroke comment in the source states this directly.

Mechanically it is a constrained dynamic program over the two index grids:

- `dist[x*N+y]` holds the best cost to align prefix `a[0..x]` with `b[0..y]`; it starts at `stroke_infinity` everywhere except `dist[0] = 0`.
- From each reachable cell, the inner `step` function considers advancing to a later cell `(x2, y2)`. It rejects the move if the two arc-length spans `dtx`, `dty` are too lopsided (`dtx ≥ 2.2·dty` or vice-versa) — the discrete form of the "slope between ½ and 2" rule — or if either span is below `EPS`.
- For an accepted move, `step` walks the overlapping segments, accumulating `(Δt) · (angle difference)²`, and scales by `(dtx + dty)`. If that yields a cheaper path to `(x2, y2)`, the cell and its back-pointers are updated.
- The outer loops expand the frontier up to four candidate steps per cell (`while (k < 4)`), always pushing toward the far corner `(m, n)`.

The cost read out of `dist[M*N-1]` is the alignment cost of the whole stroke. The `path_x`/`path_y` back-pointer arrays exist for drawing the matched correspondence; the engine passes `nullptr` for both because it only needs the cost. A `NaN` guard (`if (new_dist != new_dist) abort()`) catches pathological inputs rather than letting them propagate.

The algorithm is `O(M·N)` in the two strokes' point counts with a small constant, and allocates three `M·N` scratch arrays per comparison — cheap, because strokes are short and comparison happens once per template at release time, never in the motion hot path.

## 4. From cost to score

The raw cost is awkward to threshold directly, so `Gesture::compare` converts it to a bounded similarity score:

```
if (!a || !b)            return 0.0;   // an invalid stroke never matches
cost = stroke_compare(a, b);
if (cost >= 0.2)         return 0.0;   // beyond stroke_infinity: no match
return max(1.0 - 2.5 * cost, 0.0);     // else a similarity in (0, 1]
```

So a perfect alignment (`cost 0`) scores `1.0`, and the score falls linearly to `0` at `cost = 0.4` (clamped, since costs above `0.2` already short-circuit to `0`). This mirrors easystroke's own `Stroke::compare` scoring exactly. The match **threshold** is then a floor on this score, applied by the caller — never inside `stroke.c`.

| Score | Implied cost | Reading |
| --- | --- | --- |
| `1.0` | `0.0` | identical shape |
| `0.7` | `0.12` | close match (default-comfortable) |
| `0.6` | `0.16` | the default threshold floor |
| `0.0` | `≥ 0.4` | unrelated shapes |

The default threshold is `0.6` (`GestureConfig::match_threshold`), tunable from `0.30` to `0.90` in the GUI. Lower is more lenient (accepts sloppier strokes, risks false matches); higher is stricter. Because the score is a smooth function of cost, the threshold behaves like a sensitivity dial rather than an on/off switch.

## 5. Multi-example templates

A single recorded example makes a brittle target: your "C" has to resemble *that* "C". WeazyStroke lets one gesture carry several recorded examples — *templates* — and treats them as alternatives.

In the data model (`GestureEntry`) a gesture is a name plus `std::vector<std::vector<Point>> strokes`: one inner vector per recorded example. At bind time each example is normalized into its own `Gesture`, and the binding holds the whole set:

```
struct GestureBinding {
    std::string name;
    std::vector<Gesture> strokes;   // one per recorded example
    std::function<void()> action;
};
```

Recording the same gesture name again **appends** an example rather than replacing it (the daemon's `--record` path and the GUI's "Add example" both do this; "Replace stroke" clears first). At match time the candidate is compared against every template of every binding and the single best score wins — so a candidate matches if it resembles *any* example. Recording a gesture two or three times, with the natural variation of a real hand, is the recommended way to make recognition sturdy.

## 6. The recognizer's decision

`GestureRecognizer::recognize` ties it together. It is a pure function of the captured `Gesture` and the current bindings:

```
Recognition best;            // {matched, name, score, points}
best.points = g.size();
if (!g.valid()) return best; // <3 points -> no match, score 0

for (binding : bindings_)
    for (template : binding.strokes)
        score = Gesture::compare(g, template);
        if (score > best.score) { best.score = score; best.name = binding.name; }

best.matched = best.score >= threshold_;
if (!best.matched) best.name.clear();
return best;
```

Two guards sit *before* this, in `run_stroke`, so trivial input never even reaches the matcher:

- **Travel floor.** If the stroke's maximum distance from its start is below `kGestureMinTravel` (`16.0` px, matching the X11 default), it is treated as a click/tap, not a gesture — no comparison runs.
- **Point floor.** Two or fewer samples is likewise rejected.

Every non-trivial attempt is reported through the recognizer's reporter callback as a `Recognition` (matched flag, best name, best score, point count). The daemon uses that to print a line, append a row to `history.jsonl` for the GUI's History tab, and — on a match with the OSD enabled — flash the gesture name on the overlay. The matched binding's action then runs, unless the global enable toggle is off.

| `Recognition` field | Meaning |
| --- | --- |
| `matched` | did the best score clear the threshold |
| `name` | best-matching binding (empty when no match) |
| `score` | best score seen, **even when below threshold** (useful for tuning) |
| `points` | number of points in the captured stroke |

Reporting the best score even on a miss is deliberate: the History tab shows "no match — best 0.54", which tells you whether to lower the threshold or redraw more carefully.

## 7. What is verified

The recognition path is unit-tested headlessly, without any live input session, by feeding synthetic strokes:

| Test | Asserts |
| --- | --- |
| `tests/recognition_test.cc` | a straight right-line scores high against itself and low against a down-line; orthogonal shapes stay well separated |
| `tests/multitemplate_test.cc` | a candidate close to *any* recorded example matches; the same candidate is rejected when the gesture knows only the other example |
| `tests/config_test.cc` | gestures with multiple examples round-trip through save/load intact |

These run under `ctest` and link the same `eswl_core` the daemon uses, so the recycled C core and its C++ wrapper are exercised exactly as shipped. See [Architecture Overview](architecture.gen.html) for where recognition sits in the pipeline, and [Input Capture & Triggers](input-pipeline.gen.html) for how the path that feeds it is captured.
