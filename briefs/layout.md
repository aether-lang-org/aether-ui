# Brief: layout — flex weights, split panes, wrap, on_layout (roadmap item 7)

**Status: READY FOR EXECUTION.** 2026-07-12. Independent. M-sized.
Flutter `Expanded(flex:)` / SwiftUI `layoutPriority` +
`GeometryReader` / GtkPaned are the models.

## Mission

1. `ui.weight(handle, n)` — proportional space sharing among stack
   children.
2. `ui.splitview("h"|"v") { … }` — a user-draggable two-pane splitter.
3. `ui.wrap { … }` — flow container (wraps to next line).
4. `ui.on_layout(handle) callback |w, h|` — the generalized
   GeometryReader (the canvas resize hook, for every widget).
5. Driver-first specs for each.

## Ground truth (verified 2026-07-12)

- **Today's only flex tool is `spacer()`** (`ui/module.ae:567`, GTK
  hexpand) plus `fill_width/fill_height` (:862) and
  distribution/alignment enums (`DIST_FILL`, `set_distribution` externs
  :164-165). No weights.
- **GtkPaned: ZERO references** in `backend/aether_ui_gtk4.c` —
  native splitters exist on all three platforms (GtkPaned /
  NSSplitView / Win32 splitter) and none is wrapped.
- **The bespoke GeometryReader:** `canvas_on_resize`
  (`ui/module.ae:16`, extern :75; consumed at `vg/live.ae:125` for the
  viewBox remap) fires from the canvas draw func on allocation change.
  `on_layout` generalizes exactly this to any widget
  (GTK: a `notify::` on allocation or a size_allocate-adjacent hook —
  Phase A picks the mechanism; GTK4 removed the size-allocate signal,
  candidates are GtkWidget tick + width/height compare like the canvas
  does, or GtkDrawingArea-style resize where available).
- **The wound:** gp's three panes are fixed-width regions inside ONE
  canvas (`gp_model.ae` geometry constants) — real splitters get
  interesting only when panes are widgets (item 4 does that for the
  list pane), so gp is NOT this brief's proving consumer; a demo is.
- **GTK4 flex reality (probe first):** GtkBox has no weight concept —
  candidates: homogeneous box (equal only), hexpand ratios do NOT do
  proportions, GtkGrid column/row weights don't exist either.
  Honest options: (a) equal-share via homogeneous + hexpand for
  weight=1 children only, (b) a small custom GtkLayoutManager
  (GTK4-idiomatic, C), (c) size_request scaling. Phase A DECIDES; the
  brief pre-authorizes (b) if (a)'s equal-only is deemed too weak —
  a ~100-line C layout manager is in-scope.
- **`ui.wrap`:** GtkFlowBox exists, unwrapped — thin wrapper.

## Design decisions already made

- **splitview is the flagship deliverable** (it's the visible one and
  purely native); weight ships with whatever semantics Phase A proves
  (documented equal-only is acceptable v1 — say so loudly in the doc
  comment rather than faking proportions).
- **on_layout mirrors canvas_on_resize's contract**: fires after
  allocation settles, delivers content w/h ints, safe to build/mutate
  widgets from inside (that's its point).
- **splitview children:** exactly two, declared in the block; position
  settable (`split_set_position(px)`) + readable, and a driver route
  `POST /widget/{id}/split_position?px=` so specs can drag it.
- **No constraint solver, no anchors.** QML anchors explicitly out.

## Deliverables

- **D1** `ui.splitview` (GtkPaned wrap: create/attach/position get-set)
  + win32/macOS stubs + driver route + `examples/split_demo/` + Aeocha
  spec (ci.sh next free phase): assert children real, move position via
  route, `/widgets` w/h of panes change accordingly (uses the existing
  x/y/w/h fields from the overlay work).
- **D2** `ui.on_layout` + spec: resize window (`/window/resize` route
  exists), callback fires, app writes new size to a status label,
  spec reads it. Then (same commit or follow-up) REPLACE
  `canvas_on_resize`'s bespoke plumbing? NO — leave canvas_on_resize
  alone (load-bearing for vg); on_layout is additive. Note the
  dedup as a backlog line in roadmap.
- **D3** `ui.weight` per Phase-A decision + `ui.wrap` (GtkFlowBox) +
  spec assertions (weights: two weighted children's /widgets widths in
  ratio after a resize; wrap: children's y differs when narrow).

## Phases — commits + hard gates

**A** — ci.sh baseline green. Probes: (1) GtkPaned inside our
container attach path (the `GTK_IS_OVERLAY/BOX` attach switch at
gtk4.c:3546-area needs a GTK_IS_PANED arm — confirm); (2) the weight
mechanism decision (document findings in the commit); (3) allocation-
change hook candidate for on_layout.
**B** — D1. Commit. **C** — D2. Commit. **D** — D3. Commit.
**E** — roadmap item 7 DONE, delete brief.

## Acceptance

- [ ] splitview/on_layout/weight/wrap exported + doc'd (weight's real
      semantics stated honestly).
- [ ] split_position driver route; specs for all four; full ci.sh
      green.
- [ ] win32/macOS stubs compile.
- [ ] roadmap item 7 DONE; brief deleted.

## Traps (abridged)

- Standard cache/driver/Xvfb set (briefs history). Layout-frame waits
  for geometry asserts — doubly so here (that IS the feature under
  test): `wait_body_contains` everywhere.
- The widget attach switch in gtk4.c handles BOX/SCROLLED/OVERLAY —
  new container types need their arm or children silently vanish.
- gp specs assert canvas geometry — don't touch gp in this brief.

## Out of scope

- gp pane migration (item 4's follow-up), anchors/constraint layout,
  three+-pane splitters (nest two), animated layout (item 6 handles
  opacity only), win32/macOS implementations, RTL.

## Escalation

1. No acceptable weight mechanism (even the custom LayoutManager
   fights) → ship splitview/wrap/on_layout, mark weight deferred with
   findings; don't fake it.
2. on_layout hook can't be made reliable post-GTK4-signal-removal →
   ship the canvas-style compare-in-draw variant for drawing widgets
   only, report the general case.
