# Brief: effects — shadow + group opacity (roadmap item 5)

**Status: READY FOR EXECUTION.** 2026-07-12. Independent of every other
open item (pairs visually with the shipped overlay layer). S–M sized.
Self-contained; escalation triggers explicit. Backdrop blur is
explicitly NOT this brief (backlog, after this + overlays).

## Mission

1. `vg.shadow(dx, dy, blur, color)` — a drop shadow modifier on vg
   elements (blur + offset + tint of the element's alpha mask).
2. `vg.group_opacity(g, a)` — fade a whole subtree as one composite
   (not per-element color math).
3. ui-side shadow for native widgets via GTK CSS `box-shadow`.
4. Proving consumer: **grand_perspective's 5-rect bevel tiles gain a
   real drop shadow** (or the bevel is replaced by shadow+flat — see
   D3), plus shadowed overlay cards for free.
5. Driver-first: effects visible in a screenshot-based spec; conformance
   suite untouched (shadow is NOT an SVG feature here — see fences).

## Ground truth (verified 2026-07-12)

- **The blur already exists:** `vg/raster/blur.ae` — separable Gaussian
  (`gaussian_blur`, `build_kernel` exported; in-place on RGBA buffers,
  sigma_x/sigma_y; port of cosyne blur.ts). A drop shadow is: render
  element to an offscreen alpha mask → blur → tint → draw offset →
  draw element on top. All pieces exist.
- **The raster pipeline is the insertion point:**
  `vg/grammar/shapes.ae:128` — a shape with `filter=url(#id)` or
  `clip-path` already routes "via the raster pipeline"
  (render-to-buffer machinery: `vg/raster/render_as_raster.ae`,
  `rasterize.ae`). Shadow rides the same path; it is a new consumer of
  existing plumbing, not new plumbing.
- **The wound:** `apps/grand_perspective/gp_render.ae:83-…` —
  `render_shaded(root, r, h, s, l)`, the "3D-bevelled rect render"
  faking depth with stacked rects per tile.
- **Per-element opacity exists; GROUP opacity does not:**
  `vg/grammar/element.ae` has `element_set_fill / set_stroke /
  set_opacity` (comment at :166) and opacity bake-in on colors
  (`shapes.ae` resolve steps). But fading a SUBTREE today means
  re-coloring every element (gp's highlight-dim does exactly this) —
  and overlapped children double-darken. True group opacity = composite
  the group offscreen once, paint at alpha.
- **ui side:** the per-widget CSS machinery exists —
  `global_css_append`/`global_css_reload`
  (`backend/aether_ui_gtk4.c:1160-1184`) + per-widget class application
  (`aether_ui_apply_css` ~:1186). `box-shadow` is a one-rule extension
  of that. Overlay chrome CSS precedent: `.aui-toast`,
  `.aui-overlay-card` (added with the overlay layer).
- **House rule 3 (idle zero):** a static shadow must cost zero per
  frame — cache the blurred mask per element and invalidate on
  geometry/style change, like the recording cache.
- **Conformance guard:** the SVG compare harness
  (`vg/test/svg-compare-aevg.py`, 208 samples) is the regression gate
  for ANY raster-path touch — the shadow work must leave it at the
  current totals or better.

## Design decisions already made (don't relitigate)

- **Shadow is a vg element modifier** in the trailing-block style:
  `rect(…) { fill("#345") shadow(2, 3, 6.0, "#0008") }`. Deferred-path
  safe (must survive `record/flush` like stroke/anchor did — that
  machinery's precedent is the typography stroke re-emit).
- **Composite, don't approximate:** group opacity renders the subtree
  to an offscreen RGBA buffer once and blits at alpha — NOT walking
  children multiplying their colors (that's the bug it replaces).
- **Cache keyed on geometry+style:** blurred mask re-used across
  frames; invalidated by element mutation (element_set_* already
  funnels through one place — hook there).
- **ui-side shadow = CSS only.** No cairo work for native widgets.
  `ui.style_shadow(handle, dx, dy, blur, color)` emitting box-shadow
  through apply_css.
- **No SVG `<feDropShadow>`/filter-spec work.** This is a DSL feature;
  the SVG loader/transpiler do NOT grow shadow parsing in this brief.
- **Backdrop blur: out.** Separate later item.

## Deliverables

### D1 — `vg.shadow`
Modifier + VgPending/flush support + raster compositing (mask → blur →
tint → offset draw → element draw). Unit test in `vg/test/`
(`test_effects.ae`, added to ci.sh AEVG_TESTS): shadow pixels present
offset from the shape, alpha falls off, cache hit on second render
(assert via a render-count or hash equality — no wall-clock asserts).

### D2 — `vg.group_opacity`
On a `g()` group: composite once, paint at alpha. Unit test: two
overlapping filled rects in a 50%-opacity group → overlap pixel equals
non-overlap pixel (the double-darken killer assertion).

### D3 — proving consumers
- gp tiles: shadow under each tile. Decide on sight whether the bevel
  STAYS (shadow adds depth under it) or DIES (flat fill + shadow reads
  better) — screenshot both, pick one, record the choice + screenshots
  in the commit. gp specs must stay green (they assert behavior, not
  pixels — expect no churn).
- Overlay cards/toasts: `.aui-overlay-card` and `.aui-toast` gain
  box-shadow (pure CSS, one commit line each).
- gp highlight-dim: replace the re-color-every-tile dim with
  group_opacity IF it's a drop-in; if it entangles with the color
  scheme code, leave it and note the follow-up.

### D4 — `ui.style_shadow` (CSS box-shadow) + a styled-example line +
spec assertion via screenshot presence (pixel-diff a shadowed vs
unshadowed run of the same widget — coarse is fine).

## Execution phases — commits + hard gates

**Phase A** — full ci.sh + conformance baseline (BOTH must be green /
at current totals; STOP if not). Probe: render_as_raster round-trip of
a single rect through gaussian_blur in a scratch unit — timings sane at
AEVG_SIZE=400.
**Phase B** — D1 + unit + conformance re-run (gate: totals ≥ baseline).
Commit.
**Phase C** — D2 + unit. Commit.
**Phase D** — D3 + D4, screenshots eyeballed + attached, full ci.sh.
Commit.
**Phase E** — roadmap item 5 DONE, delete brief.

## Acceptance checklist

- [ ] vg.shadow + vg.group_opacity exported, unit-tested, in AEVG_TESTS.
- [ ] Shadow mask cached (asserted), idle cost zero (no per-frame blur
      for static scenes).
- [ ] SVG conformance totals unchanged or better (attach both numbers).
- [ ] gp tiles shadowed (screenshots in commit); overlay chrome
      shadowed; ui.style_shadow shipped.
- [ ] Full ./ci.sh green.
- [ ] roadmap item 5 DONE; brief deleted.

## Environmental traps (abridged standard set)

- `rm -rf ~/.aether/cache` after ANY imported-module edit; conformance
  harness `_ensure_built` only rebuilds MISSING binaries — `rm -rf
  target/build/apps/<app>` after shared-module edits.
- Conformance CSV is harness OUTPUT — regenerate, never hand-edit.
- Driver/Xvfb standard recipe (see briefs history).
- `${}` interpolation: no string literals inside; temp vars.
- Deferred-path parity: anything an element carries must survive
  record→flush (the typography stroke lesson) — test BOTH loader and
  transpiled columns if the SVG path is touched at all (it shouldn't
  be).

## Out of scope

- Backdrop blur / frosted materials.
- SVG filter parsing (`<feDropShadow>` etc.).
- Inner shadows, spread, multiple shadows per element.
- Animating shadows (item 6 may later tween opacity — fine, later).
- win32/macOS ui-side shadow (CSS is GTK-only; stubs elsewhere).

## Escalation triggers

1. Conformance totals drop and the delta isn't explainable/recoverable
   → stop, report with the diff.
2. Offscreen compositing for group_opacity fights the backbuffer/live
   region model (flicker, present-order bugs) → stop, report — the
   atomic-present design (live-regions memory) is load-bearing.
3. Blur cost at gp scale (hundreds of tiles) breaks interactivity even
   cached → report with measurements; do not ship a degraded default.
