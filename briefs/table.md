# Brief: table / list widgets (roadmap item 4)

**Status: GATED — do not start until item 3 (`each`) has LANDED.**
2026-07-12. `each` is briefed (briefs/each.md) but its execution is
blocked on an aether compiler bug (closure-captured heap strings dangle —
aether `ask/closure-heap-string-dangles`). Table needs `each`'s
reconciler (rows ARE dynamic children) and item 2's text metrics
(already shipped). Written for an autonomous Opus session; escalation
triggers are explicit. Re-verify anchors on pickup — this brief is
written ahead of its dependency.

## Mission

A real list/table widget so apps stop hand-drawing rows in a canvas:

1. `ui.listbox` — rows over a list, selection synced to app state,
   driver-visible rows.
2. `ui.table` — the column layer on top: column spec (title/width/
   alignment), per-cell content, sort hooks.
3. The proving consumer: **grand_perspective's hand-drawn left pane
   (~100 lines of canvas rows, hit-testing, "+N more…" folding) becomes
   the widget**.
4. Driver-first: rows/cells visible in `/widgets`, a `select_row`
   route, uidriver helpers + Aeocha spec in the same commit.

## Ground truth (verified 2026-07-12 — re-verify on pickup)

- **The wound:** gp's left pane is hand-drawn vg —
  `apps/grand_perspective/gp_render.ae:203-301` (list pane: rows,
  selected-row backdrop at ~271, ".." row with its own click zone at
  ~255, `"+ ${more} more…"` fold at :300); geometry constants
  `LIST_TOP/LIST_ROW_H/LIST_MAX` at `gp_model.ae:44-46` (20 rows before
  folding INSTEAD of scrolling); hit-testing lives app-side. Row text
  is baselined via the typography metrics (`row_baseline` comment at
  gp_render.ae:216-218) — item 2's machinery, reuse it.
- **GtkColumnView / GtkListView: ZERO uses** in
  `backend/aether_ui_gtk4.c`. Everything list-like today is GtkBox or
  canvas. GTK4's column view brings virtualization + sorting free, BUT
  it *recycles* cell widgets via factories — which collides with the
  handle registry (`aether_ui_register_widget` is append-only,
  handle-per-widget; a recycled cell is one widget showing many rows).
  This is THE design risk. See decisions.
- **`each` (item 3) provides:** the group-container + clear/rebuild
  reconciler + per-item closures. A non-virtualized listbox is `each`
  + selection + chrome. Do not re-implement reconciliation here.
- **Driver:** `/widgets` walks the real widget tree (`parent` field,
  `/widget/{id}/children` route). `/window/pick` does real hit-testing.
  Precedent for new routes + uidriver helpers: the overlay work
  (`GET /overlays`, `widget_int_field_by_id`).
- **win32/macOS:** stacks exist (WK_VSTACK etc. / NSStackView);
  no native table wrapped on either. Stubs-only this pass (house
  pattern), real ones on winbaz/Mac-mini passes.

## Design decisions already made (don't relitigate)

- **v1 is NOT virtualized.** `ui.listbox` = an `each`-backed column of
  row widgets inside the existing `scrollview`. gp folds at 20 rows
  today — real apps here are hundreds of rows, not millions.
  Virtualization arrives only with the GtkColumnView backend, phase 3,
  and only if the recycled-cell/handle-registry collision has a clean
  answer (see escalation 2).
- **Rows are real widgets with real handles** — driver-visible,
  pickable, sealable. That is the point of the widget vs the canvas.
- **Selection is single-select v1**, an int index state; `on_select`
  callback; selected-row styling via the CSS-class machinery
  (`.aui-row-selected`). Multi-select later.
- **`ui.table` = listbox whose row template is generated from a column
  spec.** Columns: `table_column(title, width, align)`; cell content v1
  is text-only via a per-cell string callback; delegate-style arbitrary
  cell widgets are phase-3+.
- **Sort hooks, not sort implementation:** clicking a column header
  fires `on_sort(col)` — the APP re-orders its list and calls the
  update. The widget never owns the data.
- **Data flow matches `each`:** explicit `listbox_update(lb, items)` /
  `table_update(t, rows)`. Reactive binding is item 8's job.

## Deliverables

### D1 — `ui.listbox`
- `listbox(_ctx, render: fn) -> ptr` (each-backed; render = row
  template `|item, i, parent|`), `listbox_update(lb, items)`,
  `listbox_select(lb, i)` / `listbox_selected(lb) -> int`,
  `on_select(lb, cb)`. Row click = select + callback; `.aui-row-selected`
  CSS. Keyboard Up/Down moves selection when focused (cheap now, feeds
  item 9 later).
- Driver: rows appear in `/widgets` under the listbox container;
  `POST /widget/{id}/select_row?i=N` route + `"selected"` field on the
  listbox JSON; uidriver helpers.

### D2 — `ui.table`
- Column spec + header row (buttons firing `on_sort(col)`), cell text
  via `|item, col| -> string` callback, column widths honored (fixed px
  v1; weights when item 7 lands).
- Same driver surface as listbox + `/widget/{id}/cell?row=&col=` read.

### D3 — proving consumer: gp's left pane
Replace `gp_render.ae:203-301` canvas rows with `ui.table` (or listbox
if one column reads better): name + %-bar + size columns, selection
synced to gp's existing selection state, ".." handled as a row, fold
becomes a scrollview (LIST_MAX dies). Gate: all 5 gp specs green —
expect REAL assertion churn here (click zones move from canvas coords to
widget routes); rewrite those assertions to the widget surface and say
so in the commit; behavioral coverage must not shrink.

### Stretch (separate commits)
- GtkColumnView backend behind the same ABI (virtualization + native
  sorting) — only with a solved handle story.
- Multi-select; delegate (arbitrary-widget) cells; tree/outline mode.

## Execution phases — each ends with a commit; gates are hard

**Phase A** — confirm `each` landed + full ci.sh green baseline
(STOP if not). Probe: 200-row listbox via `each` in a scratch app —
scroll behavior inside `scrollview`, `/widgets` payload size sanity
(200 rows × N widgets each — if the JSON blows past the driver's buffer,
that's a route fix, do it first).
**Phase B** — D1 + example (`examples/listbox_demo/`) + spec (ci.sh
Phase 5f). Commit.
**Phase C** — D2 + spec additions. Commit.
**Phase D** — D3 gp migration; screenshot before/after; 5 gp specs.
Commit.
**Phase E** — roadmap item 4 DONE, delete this brief (precedent 2e72530).

## Acceptance checklist

- [ ] listbox + table exported, doc-commented, each-backed (no second
      reconciler).
- [ ] select_row route + selected field + uidriver helpers + Aeocha
      specs (add/update/select/sort-callback) in ci.sh.
- [ ] gp list pane is widgets; `LIST_MAX`/fold code gone; 5 gp specs
      green (rewritten where the surface legitimately moved).
- [ ] Full ./ci.sh green; win32/macOS compile (stubs fine).
- [ ] roadmap item 4 DONE; brief deleted.

## Environmental traps (the standard set, abridged — full list in git
history of briefs/each.md)

- `rm -rf ~/.aether/cache` after ANY imported-module edit.
- ui/module.ae imports go AFTER the exports block.
- Driver: port 9222, one app at a time, shutdown via POST /shutdown,
  Xvfb recipe `GDK_BACKEND=x11 WAYLAND_DISPLAY= GSK_RENDERER=cairo
  xvfb-run -a -s "-screen 0 3200x2000x24"`.
- Fresh widgets need a GTK layout frame — `wait_body_contains`, never
  immediate asserts.
- Closures: capture-by-value unless assigned; heap-string capture was
  buggy (aether ask pending) — re-verify fixed before relying on it.
- gp specs are behavioral; run individually while iterating.

## Out of scope

- Virtualization in v1 (stretch, GtkColumnView only).
- Editing cells (renderer/editor split is a later Swing lesson).
- Reactive/bound data (item 8).
- Tree mode (stretch note only).
- win32/macOS implementations.
- No sibling-repo changes; compiler bugs → probe, document, STOP.

## Escalation triggers

1. `each` still blocked upstream → this brief stays parked; do not
   hand-roll a private reconciler.
2. GtkColumnView cell recycling can't map to stable handles → ship the
   each-backed non-virtualized version as THE version, note the limit
   in roadmap, and stop (don't invent a shadow-handle scheme without a
   design round-trip).
3. 200-row driver JSON breaks routes → fix the route buffer as its own
   commit BEFORE the widget work.
4. gp spec churn turns into weakened assertions → stop, report.
