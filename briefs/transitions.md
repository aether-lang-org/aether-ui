# Brief: implicit transitions (roadmap item 6)

**Status: READY FOR EXECUTION.** 2026-07-12. Independent; pairs with the
shipped overlay layer (enter/exit) and item 5 (nothing hard-depends).
S–M. QML `Behavior` is the model — declare once on a property, every
subsequent setter tweens — NOT SwiftUI's call-site `withAnimation`.

## Mission

1. `ui.transition(handle, prop, ms, easing)` — after this, existing
   setters (`set_hidden`, `style_opacity`, enable/disable) tween
   instead of snap. Props v1: `"opacity"` only (see decisions).
2. `vg.behavior(el, prop, ms, easing)` — same for vg elements, reusing
   the existing animation machinery. Props v1: `"fill"` (color lerp)
   and `"opacity"`.
3. Overlay enter/exit: toasts fade in/out; modal scrim fades.
4. CI stays deterministic: a no-animation switch; specs assert END
   state.

## Ground truth (verified 2026-07-12)

- **The machinery exists and idles at zero:**
  `vg/grammar/animations.ae` (`animate(..., easing_fn, loop, yoyo,
  now_ms) -> *AnimationHandle`, `tick_animations(ctx, now_ms)` once per
  frame); easings in `vg/geom/easing.ae`; `scene_advance`
  (`vg/module.ae:841`) advances WITHOUT re-render; the live loop
  (`vg/live.ae:135-141`) runs a 16ms `ui.timer` that no-ops unless
  `scene_is_animating`/`scene_is_refreshing` — the zero-idle discipline
  to preserve.
- **ui side has NO tween machinery** — `set_hidden`
  (`ui/module.ae:699`), `style_opacity`, enable are instant GTK calls.
  House timer exists (`aether_ui_timer_create_impl`, one-shot/interval)
  — the overlay toast auto-dismiss uses it.
- **The wounds:** gp Stop/Rescan ghosting + colour-scheme radio flips
  snap (roadmap evidence); overlay toasts pop in/out.
- **GTK4 has native CSS transitions** — `transition: opacity 150ms` in
  the per-widget CSS machinery (`global_css_append`/`aether_ui_apply_css`,
  `backend/aether_ui_gtk4.c:1160-1190`) makes GTK tween opacity BY
  ITSELF, no timer, no per-frame Aether code. Verify in Phase A; if it
  holds, the ui side is nearly free.
- **Driver determinism precedent:** `GSK_RENDERER=cairo`-style env
  gating is the house pattern; specs already `wait_body_contains`.

## Design decisions already made

- **ui-side v1 rides GTK CSS transitions** if the Phase-A probe
  confirms they fire (opacity via the existing opacity/CSS path). Only
  if CSS transitions don't work do we fall back to a house-timer tween
  driver — and then ONLY opacity, 60fps timer alive strictly while a
  tween runs (idle-zero).
- **`set_hidden` with a transition = fade then hide** (and show then
  fade-in) — visibility itself can't lerp; orchestrate around it.
- **vg-side reuses `animate`** — `vg.behavior` registers the property
  so `element_set_fill`/`set_opacity` route through a tween (start an
  animation from current→target) instead of writing directly. The
  scene's existing animating flag wakes the live loop; no new loop.
- **`AETHER_UI_NO_ANIMATION=1` short-circuits every tween to the end
  state** — exported into ci.sh's launch env for ALL spec phases, so
  the suite never races an animation. Specs that specifically test
  transitions unset it and assert intermediate ≠ end via two samples.
- **No enter/exit choreography DSL.** Overlay fade is hardcoded in the
  overlay layer (toast/scrim), not a general system.

## Deliverables

- **D1** `ui.transition(handle, "opacity", ms, easing)` + the
  no-animation env + doc comment. Easing names: "linear",
  "ease_out" v1 (map to CSS timing functions / easing.ae fns).
- **D2** `vg.behavior(el, "fill"|"opacity", ms, easing)` + unit test in
  AEVG_TESTS (`test_behavior.ae`: set fill, advance clock via
  tick_animations, assert mid-lerp colour ≠ end, then end == target —
  the manual-clock pattern animations.ae documents).
- **D3** overlay polish: toast fade-in/out, scrim fade-in
  (`.aui-overlay-scrim` CSS transition). Spec: with NO_ANIMATION unset,
  /overlays live:1 while a fresh screenshot differs from the settled
  one; with it set, everything is instant (existing specs unchanged).
- **D4** gp: colour-scheme radio flip + Stop ghosting get 150ms opacity
  transitions (two `ui.transition` lines). gp specs stay green because
  ci.sh exports NO_ANIMATION.

## Phases — commits + hard gates

**A** — baseline ci.sh green. Probe: GTK CSS `transition:` on a
widget's opacity via apply_css — does it animate? (headless: screenshot
at t0/t80ms differ). STOP-and-choose fallback path if not.
**B** — D1 + NO_ANIMATION wiring into ci.sh launches. Gate: full ci.sh
green (proves determinism). Commit.
**C** — D2 unit-tested. Commit. **D** — D3+D4, screenshots eyeballed.
Commit. **E** — roadmap DONE, delete brief.

## Acceptance

- [ ] ui.transition + vg.behavior exported/doc'd; test_behavior in
      AEVG_TESTS.
- [ ] AETHER_UI_NO_ANIMATION honored everywhere; ci.sh exports it for
      spec phases; full ci.sh green.
- [ ] Idle cost zero verified (no timer running with no live tween —
      assert via the scene_is_animating gate or timer bookkeeping).
- [ ] Toast/scrim fade; gp radio/ghosting tween (screenshots).
- [ ] roadmap item 6 DONE; brief deleted.

## Traps (abridged; full set in briefs history)

- Cache: `rm -rf ~/.aether/cache` after module edits. Driver: port
  9222 discipline + Xvfb recipe. Fresh-widget layout frames:
  `wait_body_contains`.
- animations.ae tests use a MANUAL clock (`tick_animations(ctx, t)`) —
  never wall-clock-sleep in units.
- Closure heap-string capture bug (aether ask) — irrelevant here
  (no per-item closures), but re-verify if D2 stores closures.

## Out of scope

- withAnimation-style call-site API; springs/keyframes; layout/position
  transitions (x/y/w/h); enter/exit DSL; win32/macOS (stubs OK — CSS
  path is GTK-only); backdrop effects.

## Escalation

1. GTK CSS transitions don't fire through our styling path AND the
   timer fallback can't keep idle-zero cleanly → report options.
2. NO_ANIMATION can't make an existing spec deterministic → stop;
   don't add sleeps to specs.
3. vg behavior fights the deferred/record path → the typography stroke
   re-emit precedent applies; if it needs VgPending surgery beyond a
   field, report first.
