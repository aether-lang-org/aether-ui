# Brief: `each` — dynamic children (roadmap item 3)

**Status: READY FOR EXECUTION.** 2026-07-12. Self-contained hand-off —
everything needed is in this file or at the cited file:line anchors.
Written for an autonomous session; escalation triggers are explicit.

## Mission

Give containers dynamic children driven by a list: an `each` verb that
(re)builds a per-item widget subtree when the list changes, so apps stop
pre-allocating widget pools. Concretely:

1. `ui.each(parent) callback |item, i| { …build item widgets… }` + an
   update entry point that re-renders from a new list.
2. Whatever child-management backends still lack (insert-at; verified
   remove/clear coverage — see ground truth: more exists than the
   roadmap thought).
3. The proving consumer: **grand_perspective's breadcrumb pool of 8
   pre-made buttons becomes real dynamic crumbs** (the original wound).
4. Driver-first (house rule 1): an example + Aeocha spec asserting
   widgets appear/disappear in `/widgets` as the list changes; full
   ci.sh green.

## Ground truth (verified 2026-07-12 — start here; two roadmap claims are STALE)

- **Why `each` is not duplicating "DSL with scope"** (the design
  boundary — see aether/docs/closures-and-builder-dsl.md): the language
  already gives repetition over SPACE at build time (immediate blocks
  are inline code, so `while`/`for` inside `window {}` is a build-time
  Repeater — do NOT build a loop wrapper). What it cannot give is
  repetition over TIME: re-running the per-item build when the list
  CHANGES, scoping which children belong to the group, and removing the
  ones that fell off. That reconciler is why SwiftUI has ForEach and
  React has keys despite both host languages having loops. `each` is
  only that half.
- **Nearest ancestor: AngularJS `ng-repeat`, not ForEach/keys.** Like
  ng-repeat we mutate a RETAINED widget tree in place (not diff a
  virtual description): a directive owning its container's repeated
  children, a per-item template scope (`item` + `$index` ≡ our
  `|item, i|`), destroy-and-recreate on identity change (our v1
  clear+rebuild ≡ Angular without `track by`), and `track by` itself —
  whose name this codebase already inherited via `vg/grammar/bind.ae`'s
  `bind_trackby`. Use Angular's vocabulary for the stretch goal. The
  one deliberate difference: ng-repeat re-ran on every `$digest`; our
  v1 is explicit `each_update` (digest-less) — the automatic-trigger
  half belongs to roadmap item 8 (bindings), whose vg precedent is
  `scene_set_refreshing`.
- **Runtime creation + attach is SHIPPED CODE, not just a probe.**
  `examples/overlay_demo/overlay_demo.ae` builds its modal card
  imperatively INSIDE a button callback: `card = root_vstack(10)` then
  `_ = text(card, "…")`, `_ = btn(card, "Dismiss") callback {…}` — the
  handle-as-`_ctx` first-arg pattern attaches at runtime and displays
  (landed 526bd6a, spec-covered in ci Phase 5b).
- **`builder_context()` IS a language builtin** (aether repo:
  typechecker.c:2497 registers it; codegen_expr.c:3334 emits the
  context-stack read; documented in docs/closures-and-builder-dsl.md
  §Builder Context Stack). Nothing in aether-ui uses it yet — that's
  why a repo grep finds nothing. It reads the CURRENT stack, so it's
  live inside immediate blocks but NOT inside a later callback (the
  stack has unwound by then) — hence capturing the container *handle*
  remains the callback-safe mechanism, and roadmap item (c) is mostly
  *verify + document*, not build.
- **remove_child / clear_children ALREADY EXIST** (the roadmap's
  "removal doesn't" is stale): exported from `ui/module.ae:48`, wrappers
  at ~1182, impls on ALL THREE backends —
  `backend/aether_ui_gtk4.c:2895` (`gtk_box_remove`, GTK_IS_BOX only),
  win32 (`DestroyWindow` + `stack_do_layout`), macOS
  (`removeArrangedSubview`, NSStackView only). BUT: zero consumers,
  zero test coverage — treat them as unproven until the spec exercises
  them. GTK4's is box-only (no grid/overlay support; fine for v1).
- **Insert-at does NOT exist** (append-only via
  `aether_ui_widget_add_child_ctx`). GTK4 has
  `gtk_box_insert_child_after` when needed.
- **Widget registry** (`backend/aether_ui_gtk4.c:63-88`): append-only
  handle table with a weak-ref finalizer that NULLs the slot on widget
  destruction (`on_widget_finalized`) — so removed widgets don't leave
  dangling handles; every `aether_ui_get_widget` consumer NULL-checks.
  Handles are never reused; fine.
- **Closure lifetime:** `btn(...)` boxes its closure (`box_closure`) and
  the C side retains it. Removing/destroying a button does NOT free its
  boxed closure — a small leak per removed button. Accept + document
  for v1 (crumbs churn a handful per navigation); don't build a
  closure-release protocol in this brief.
- **The wound (proving consumer):** gp's crumb pool —
  `apps/grand_perspective/gp_model.ae:49` (`CRUMBS() -> int { return 8 }`),
  `gp_state.ae:37-40` ("widgets can't be created post-build" — the STALE
  belief, plus `crumb0: int` pool fields), relabel/hide loop at
  `gp_nav.ae:101-115` (`update_crumbs`), context-menu wiring loop at
  `grand_perspective.ae:338-345`. The pool also means each crumb carries
  ONE pre-wired context_menu_item whose closure reads `st.current.path`
  at fire time — replacing the pool must keep "Copy path" working.
- **Track-by prior art (LATER, not v1):** `vg/grammar/bind.ae:116`
  (`region_set_trackby`, closure returning a string key per item).
  Don't import its machinery; it's the naming/shape precedent only.
- **List state:** `ui_state` is doubles-only (roadmap item 8's problem —
  NOT yours). `each` v1 takes an explicit `std.list` of `ptr` items via
  an update call; do not build typed reactive list-state here.
- **Driver:** `/widgets` already reflects runtime-added widgets (proven
  in the overlay work); `parent` field + `/widget/{id}/children` route
  exist. Removal visibility in `/widgets` is what the spec must pin.

## Design decisions already made (don't relitigate)

- **v1 reconcile = clear + rebuild.** On update: `clear_children`-style
  removal of the each-group's widgets, then re-run the item builder per
  item. Correctness first; crumbs/rows don't hold focus or edit state.
  Keyed reuse (track-by) is a LATER item — leave a comment, not code.
- **`each` owns a plain child container.** The verb creates its own
  vstack/hstack inside the parent (orientation param) and rebuilds
  INSIDE that — so it never touches siblings outside its group, and
  clear-and-rebuild can't eat the rest of the window. This also
  sidesteps insert-at entirely for v1 (append into an empty box).
- **Update is an explicit call** (`each_update(e, items)`), imperative
  like everything else ui-side today. When item 8 lands typed/list
  state, `each` gets a binding overload; not now.
- **Item builder receives (item: ptr, i: int)** and builds into an
  injected parent handle using the existing handle-as-first-arg pattern.
- **No vg-side work.** vg already has bind/Repeater-ish machinery; this
  brief is the ui (widget) side only.

## Deliverables

### D1 — the `each` verb + reconciler (ui/module.ae, pure Aether)
- `each(_ctx: ptr, orientation: string, spacing: int, render: fn) -> ptr`
  — creates the group container (vstack for "v", hstack for "h") in the
  enclosing block, stores the boxed render closure + container handle in
  a small heap struct, returns the each-handle (ptr).
- `each_update(e: ptr, items: ptr)` — clears the group container's
  children and re-runs `render(item, i)` for each list element, with the
  group container injected so the render body's verbs attach to it.
  (Invoke the stored closure via the fn-typed-param bridge — the
  `events.ae` invoker pattern, `vg/grammar/events.ae:259`.)
- `each_count(e: ptr) -> int` — last rendered count (for tests/apps).
- Exports + doc comment showing the canonical usage:
  ```
  crumbs = each("h", 4) callback |it: ptr, i: int| {
      e = it as *FileEntry
      _ = btn(each_parent(crumbs), e.name) callback { … }
  }
  ...
  each_update(crumbs, stack_list)
  ```
  NOTE the render body needs the group handle — provide
  `each_parent(e) -> int` OR pass the parent handle as a third callback
  arg `|item, i, parent|`; pick whichever the closure/param plumbing
  makes cleaner (the 3-arg form avoids the chicken-and-egg of using
  `crumbs` inside its own defining block — RECOMMENDED).
- **Phase-A probe, optional upgrade:** the compiler's context stack has
  a C ABI (`_aether_ctx_push`/`_aether_ctx_pop` — see
  closures-and-builder-dsl.md §Implementation Notes). If those are
  extern-declarable from Aether and safe to call, `each_update` can
  push the group container around the render invocation — then the
  render body uses PLAIN declarative verbs (auto-injected `_ctx`, no
  parent param at all), reading exactly like the rest of the DSL. Probe
  it; if it works, prefer it; if it's fragile (internal ABI, stack
  depth 64), fall back to the 3-arg form without ceremony. Do NOT file
  an aether ask for this in this brief's scope.
- If GTK_IS_BOX-only `clear_children` proves insufficient for the group
  container, extend the C impl minimally (it's already box-only-safe:
  the group IS a box by construction).

### D2 — example + spec (driver-first)
- `examples/each_demo/` (aeb node like overlay_demo): a textfield-free
  demo with "Add item" / "Remove last" / "Reset" buttons mutating a
  local `std.list`, calling `each_update`; each item renders a
  label + a per-item button proving per-item closures fire correctly
  (e.g. clicking item i writes "clicked <name>" to a status line).
- `tests/each_demo/spec_each_demo.ae` (Aeocha): assert `/widgets` grows
  after Add, shrinks after Remove (the count of items in the group),
  per-item click round-trips to the status text, Reset → empty. Wire as
  a ci.sh phase (5e, following 5b/5c/5d precedent).
- uidriver helper if needed (e.g. counting widgets whose parent == the
  group container) — extend `tests/lib/uidriver.ae` like
  `widget_int_field_by_id` was.

### D3 — the proving consumer: gp breadcrumbs
Replace the CRUMBS()=8 pool in grand_perspective with `each`:
- `gp_state.ae`: drop the crumb pool fields (`crumb0`…) + the stale
  comment; hold the each-handle instead.
- `gp_nav.ae` `update_crumbs`: becomes `each_update(crumbs, stack)` with
  the render building one button per stack entry (label = clip_name,
  ellipsis logic for depth > visible as today — decide whether to keep
  the 8-visible cap as a RENDER decision (slice the list) rather than a
  pool size; keeping the cap keeps layouts identical and specs stable —
  RECOMMENDED).
- The per-crumb "Copy path" context_menu_item moves INTO the render
  closure (attached to each new button as it's built).
- Gate: all 5 gp Aeocha specs green UNCHANGED (they assert crumb
  behavior via click/drill flows — if a spec asserts a pool artifact
  that legitimately changed, fix the assertion and say so in the commit;
  do not weaken behavioral assertions).

### Stretch (only if D1–D3 green with time left; separate commits)
- Keyed reuse (track-by) — reuse existing child widgets when the item
  key at an index is unchanged. Follow `region_set_trackby`'s shape.
- `insert_child_at` backend + `each` splice path.
- win32/macOS verification is NOT in scope beyond "stubs/impls compile"
  (next time on winbaz / the Mac mini — note remove_child there is
  real code but untested).

## Execution phases — each ends with a commit; gates are hard

**Phase A — characterize.** `rm -rf ~/.aether/cache`; full `./ci.sh`
baseline (must be green; STOP if not). Probe in a scratch app (not
committed): (1) `clear_children` on a live GTK4 box actually removes
rendered children + `/widgets` reflects it; (2) re-adding after clear
works repeatedly (10 cycles, no crash/leak explosion); (3) a boxed
closure created inside a *callback* (not the build block) fires
correctly when its button is clicked later (the ae-0.255 drain-gate
territory — expected fine, verify anyway).

**Phase B — D1 + D2.** The verb, the demo, the spec, ci.sh Phase 5e.
Gate: new spec green + full ci.sh green. Commit.

**Phase C — D3.** The gp crumb replacement. Gate: 5 gp specs + full
ci.sh green; screenshot gp before/after at default size and after a
3-deep drill — crumbs must look the same (eyeball, attach findings to
the commit message). Commit.

**Phase D — closeout.** Mark roadmap item 3 DONE (house style: item 1/2
DONE blocks), delete this brief in the same commit (precedent 2e72530).

## Acceptance checklist

- [ ] `ui.each` + `each_update` + `each_count` exported, doc-commented.
- [ ] each_demo example + Aeocha spec in ci.sh (Phase 5e); add/remove/
      reset/per-item-click all asserted via the driver.
- [ ] gp: `CRUMBS()` pool GONE (`grep -rn "crumb0\|CRUMBS()" apps/` →
      only historical comments at most); crumbs fully dynamic; "Copy
      path" context menu still works (fileops spec covers it).
- [ ] `grep -rn "can't be created post-build" apps/` → no hits (the
      stale belief comment dies with the pool).
- [ ] Full `./ci.sh` green (all phases, incl. the 5 gp specs).
- [ ] roadmap.md item 3 marked DONE; briefs/each.md deleted.

## Environmental traps (every one has cost a real debugging session)

- **Compiler cache:** `rm -rf ~/.aether/cache` after ANY edit to an
  imported module (ui/module.ae edits especially). Stale caches produce
  impossible E0301s and silently run old code.
- **ui/module.ae imports go AFTER the exports block** (house style; it
  now has `import std.string` — add others there).
- **Driver apps:** end via `curl -X POST :9222/shutdown`, never by
  killing xvfb-run. One app at a time (`curl -sf :9222/widgets || echo
  free`). Port always 9222; `AETHER_UI_TEST_PORT` must be SET to arm.
- **Xvfb recipe:** `GDK_BACKEND=x11 WAYLAND_DISPLAY= GSK_RENDERER=cairo
  xvfb-run -a -s "-screen 0 3200x2000x24"`.
- **GTK layout frames:** freshly added/removed widgets need a frame
  before geometry/pick queries see them — specs use
  `wait_body_contains`, not immediate asserts (overlay-layer lesson).
- **Language:** `state`/`spawn`/`match` reserved; no top-level mutable
  globals; block-form callbacks need the `callback` postfix keyword;
  `${}` interpolation can't contain string literals; multiple
  `opt_s()`-style calls in ONE println interpolation corrupt — use temp
  vars. Closures into C: `box_closure`, C side retains.
- **Invoking a stored closure from Aether:** the fn-typed-param bridge
  (store as ptr, call via a helper taking `cb: fn`) — see
  `vg/grammar/events.ae:259-267`.
- **gp specs are behavioral** — they drill/click/read status. Run them
  individually while iterating (`UI_SPEC=grand_perspective/spec_map_nav
  tests/run_spec.sh`) before the full ci.sh.

## Out of scope — do not do these even if tempting

- No typed/reactive list state (item 8's job); `each_update` is
  explicit.
- No keyed reuse/track-by in v1 (stretch only).
- No vg-side repeater work.
- No closure-release/registry-compaction protocol (document the small
  per-removed-button leak instead).
- No table/list widget (item 4 — `each` merely unlocks it).
- No win32/macOS behavioral work (compile-only there this pass).
- No aeocha/aether/aeb (sibling repo) changes. Suspected compiler bug:
  minimal probe in scratchpad, document, STOP and report.

## Escalation triggers — stop and report instead of pushing through

1. Phase-A probe shows repeated clear+rebuild crashes or visibly leaks
   (GTK criticals spray, memory climbs fast) → the registry/closure
   story needs design work; report with the probe.
2. Invoking the stored render closure per-item from `each_update`
   hits closure/codegen issues (wrong item ptr, env corruption) →
   minimal repro, document, STOP (compiler territory — the
   builder-ctx/closure history says report, don't work around).
3. gp crumb replacement breaks a gp spec in a way that isn't a
   legitimate behavioral improvement → report; don't weaken specs.
