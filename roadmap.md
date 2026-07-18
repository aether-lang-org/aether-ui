# Roadmap: what remains

**Status: PARED DOWN.** 2026-07-14. The nine-item ranked roadmap of
2026-07-12 (overlay layer, typography, `each`, table/list, effects,
transitions, layout, bindings, focus) was executed in full on
2026-07-12/13 — see git history for the original document with its
per-item DONE blocks, evidence tables, and commit trails (last full
version at 1ade88b; the wound-driven method writeup lives there too).
This document now lists ONLY what is not done, in three buckets:
platform parity, deferred follow-ups, and the backlog/strategic fork.

## 1. Platform parity

### macOS — DONE 2026-07-14 (tabs added 2026-07-17). Full parity; `./ci.sh` all phases pass.

The AppKit backend is no longer the stubbed one. Every item that was
listed here as a macOS gap is real, and the spec matrix is **85/85
green on the Mac** (`./tests/spec_matrix.sh`), including all five
grand_perspective app suites and `ui.tabs`.

| Feature | macOS today |
|---|---|
| Driver introspection + `/focus` `/window/key` `/window/resize` | **shared server** — the private one is deleted (see below) |
| Text metrics (`vg.text_extent`) | CoreText (`CTLineGetTypographicBounds`) on the SAME font the canvas draws with |
| splitview | real `NSSplitView`, draggable divider (NB its `vertical` is the INVERSE of GTK's) |
| tabs (`ui.tabs`) | real `NSTabView`; on_change fires via the delegate, deduped against last index (swallows AppKit's initial auto-select) |
| weight / on_layout / wrap | Auto Layout multiplier constraints / frame-change observer / `AetherWrapView` flow layout |
| shortcut (accelerators) | one `NSEvent` local monitor + a normalized-combo registry |
| focus (grab / `GET /focus` / Tab) | `makeFirstResponder:`; **Tab follows BUILD order** (matches GTK — see below) |
| Overlay layer (toast/modal/tooltip/dropdown) | `AetherOverlayHost` interposed under the window; real scrim, hit-test-blocking |
| Context menus | `-[NSView setMenu:]` (the native right-click shape) |
| Canvas on_click/on_key/on_release/on_resize, clip_rect, `canvas_write_png` | all real; write_png is headless (CGBitmapContext) |
| Implicit transitions (`ui.transition`) | hand-driven tween (NOT `-[NSView animator]` — see below) |
| toggle_group | real radio exclusivity |

**The big structural change: macOS adopted the shared test server.**
It used to carry its own ~370-line HTTP server, which had drifted badly
(no `enabled`/geometry/`classes` fields, no `/focus`, `/window/key`,
`/window/resize`, no tray/notification routes, no URL-decoding, and it
read NSView state straight off the server thread). It is gone; macOS now
fills in `AetherDriverHooks` exactly as win32 does. **The "three-servers
rule" is back to two** — GTK4's embedded one, and the shared one behind
both other backends.

**Routes added to the SHARED server** (so win32 gained them too — they
had been GTK4-only, which is why split/overlay/canvas specs were red on
*both* non-Linux backends regardless of backend quality):
`/text_extent`, `/overlays`, `/window/pick`, `/canvas/{id}/click|move|key`,
`/widget/{id}/split_position`, `/widget/{id}/context_menu[/{idx}]`,
`/shutdown`, plus `splitPosition` and `selected` in the widget JSON.

Traps found the hard way, all now commented at their site:
- **Tab order.** AppKit's key-view loop only walks text fields unless the
  user has *Full Keyboard Access* on system-wide — Tab order would depend
  on a setting outside the app. macOS therefore walks the widget registry,
  whose order IS build order. **macOS matches GTK here where win32 cannot.**
- **Geometry must report the ALIGNMENT rect, not the frame.** An
  `NSTextField`'s frame is ~2px wider per side than the box Auto Layout
  positions; reporting frames made adjacent siblings look like they
  *overlapped* by 4px and a row's widths sum past its container.
- **Expand must propagate UP, retroactively.** GTK: a box holding an
  expanding child expands. AppKit has no such rule, and the DSL assembles
  top-down (a container joins its parent *before* its children arrive) —
  so the canvas announces "I expand" long after its row was already
  height-chained to its siblings. `aeui_mark_expand` walks up and retracts
  those chains. Without it, Grand Perspective's treemap was pinned to the
  toolbar's height and everything below the fold was unclickable.
- **`-[NSView animator]` is invisible to the driver.** It sets the model
  value immediately and animates the *presentation* layer, so every
  mid-tween screenshot already shows the settled frame. Transitions are
  hand-stepped on a timer so intermediate states are real and assertable.
- **`ui.style_opacity` travels as CSS**, not through the `set_opacity` ABI
  call. Miss that in `apply_css` and every declared transition snaps.

**Buttons stretching vertically (mostly fixed 2026-07-17).** Buttons carry
contentHugging 200, so they absorb a vstack's leftover space. The fix was
NOT to change the button (the calculator *depends* on that stretch for its
grid, and has no canvas to hand the slack to): instead the CANVAS's
size-preference constraint was dropped to priority 150, below the button
hugging. A canvas and a button both grow to take slack; the lower-resistance
one wins, so the canvas now out-competes buttons. rubiks_cube's Reset/
Shuffle/Solve went from 146px tall to a natural 20px and the cube canvas
took the freed space; the calculator (no canvas) is untouched. Remaining:
in a stack with a **non-canvas** slack-taker missing (e.g. a lone button row
with nothing below it, or the **tabs page area** sharing a vstack with a
button row) the buttons still absorb the slack. The real end-state is
natural button hugging + explicit `ui.weight` opt-in, which wants a GTK
side-by-side first; the canvas-priority fix covers the common case.

### win32 — the gaps that remain

Real: focus/Tab (dialog order, not build order), driver introspection,
state/bindings, canvas `on_click`/`on_key`/`on_release` (`c21334c`),
picker (combobox + driver set_value + default selection, `962ed0e`),
**overlay layer** (`962ed0e` — raised child HWNDs over the client area:
toast/modal-scrim/tooltip, append-only `/overlays`, Escape-dismiss,
deep `/window/pick`; toast lifecycle proven green), and now everything
the shared server gained above.
Still stubbed: **splitview** (plain stack, no divider — `split_position`
honestly answers -1), **shortcuts** (`/window/key` answers
`fired:false` honestly), **text metrics** (GDI), **weight / on_layout /
wrap**.

**Two cross-cutting win32 bugs now block several suites (NOT feature
stubs — fix these next, they unlock overlay + vg_tooltip + gp):**
- **widget geometry reports `h:0`.** Every widget's driver rect has
  height 0 (the calculator shows it too — it's green only because its
  specs don't read geometry). This makes `/window/pick` at a widget's
  computed centre miss, so the overlay pick/scrim tests (4) fail even
  though the overlay layer works. Also likely behind gp_hover_and_resize
  and any future geometry assert. The HWNDs render fine — it's the
  `widget_rect` hook / stack-layout-timing that reports wrong.
- **canvas scene doesn't rescale** (`canvas_on_resize` is a stub): the
  vg scene stays at its initial viewBox size, so hover px coords don't
  map to scene hit regions → vg_tooltip hover finds no shape (4 fails).
  The overlay/tooltip plumbing underneath is correct.

~~Known harness gap~~ FIXED 2026-07-16: gp clips crumb-button labels
(>20 chars → 17+"..."), and on Windows the fixture's long native path
truncates while Linux's short `~/.gp-ci-XXXXXX` doesn't —
`gp_driver.root_crumb_label()` now mirrors the clip, so the spec
searches for the AS-RENDERED label. With that plus the canvas-events
commit, **gp_map_nav is 2/2 green on Windows** (drill + crumbs +
keyboard nav proven through the matrix). NB: the ae module cache can
serve a STALE spec helper — `rm -rf ~/.aether/cache` when a spec edit
seems to have no effect.

**Windows matrix baseline (2026-07-16 pm, `962ed0e`): 52 passing / 56
failing.** GREEN suites: calculator 9, each 6, listbox 6, table 4,
bindings 6, picker 3, gp_map_nav 2. Partial: overlay 3/7 (toast
lifecycle green; pick tests blocked by the h:0 geometry bug above),
vg_tooltip 1/5 (hover blocked by canvas-scene scaling above). Remaining
reds map to: **the two cross-cutting bugs** (overlay pick, vg_tooltip
hover), shortcuts (testable ×2, gp_scan_and_list ×4), splitview+layout
(split 13), GDI metrics (text_metrics), ctx-menu driver actions + gio
(context_menu, gp_fileops), toggle_group radio exclusivity
(gp_legend), Tab order platform-native (testable 1). (The tabs strip is
no longer a stub — win32 tabs went real 2026-07-18; LisMusic's spec is
5/5 on winbaz.)

**Windows spec-matrix baseline (2026-07-14 evening, winbaz, post
macOS-parity pull + timer fix):** `AEOCHA_DIR=... ./tests/spec_matrix.sh`
→ 44 passing / 81 failing over 17 suites. GREEN: calculator 9, each 6,
listbox 6, table 4, bindings 6. Every red maps to a NAMED win32 gap in
the table above, none to harness artifacts: text_metrics (stub),
testable 3 (shortcuts unwired ×2, Tab order is Windows-native),
context_menu (driver ctx actions unwired), overlay+vg_tooltip+picker
(overlay host stub), split (splitview stub + on_layout no-op),
gp suites (canvas click/key unwired, shortcuts, gio absent on
Windows). Fixed to get here: build.sh fallback + cygpath fixture paths
in spec_matrix.sh, and a REAL long-standing win32 bug — timers created
before app_run ran as thread timers whose system-assigned id never
matched ours, so ui.timer closures never fired (gp's scan pump was
dead on Windows, likely forever).

**Update 2026-07-18 — win32 tabs went real:** the tabs backend was a
stub (a vstack that appended pages, no strip, no switching; tabs_selected
= -1). It's now a real tab strip — a header hstack of one button per tab
over a zstack of pages; stack_do_layout special-cases WK_TABS; a strip
click or programmatic tab_select shows the page, bolds the active button,
fires on_change deduped. Verified on winbaz: the LisMusic driver spec is
5/5. Two harness bugs fixed in passing (both cross-backend): the
test-server now JSON-escapes ALL control chars in widget text (a stray
U+0001 in a search title was making the /widgets body invalid JSON, so
client json.parse silently returned 0), and a widget_id_last_of driver
helper disambiguates duplicate labels. Known residual (separate, not
tabs): a win32 transient-widget-text glitch garbles the echoed search
term in one result row — cosmetic, doesn't affect the spec.

## 2. Deferred from the ranked items (recorded at each closeout)

- **Typography:** ~~multi-line wrap (`text_wrapped`), text-anchor
  middle/end~~ **DONE 2026-07-18** (`text_wrapped` + `text_anchor`;
  wrap/anchor in the text JSON; 3/3 GTK4+win32). Remaining: text-anchor
  for CANVAS/vg text (this covers native labels).
- **Table/list:** virtualization (the GtkColumnView / recycled-cell vs
  handle-registry question — the one real remaining table item),
  ~~delegate cells (%-bars, chips)~~ **DONE** (`table_col_delegate` — a
  column whose cells a closure builds widgets into), ~~row double-click~~
  **DONE**, ~~multi-select~~ **DONE**, ~~**tree mode**~~ **DONE**
  (`tree`/`tree_node`/`tree_add_child` — hierarchical rows, disclosure
  toggle, independent subtrees; 3/3). All pure module.ae (backend-
  agnostic); tree+delegate+table_bind verified GTK4+win32.
- **Effects:** ~~shadows on paths/text~~ **DONE 2026-07-18** — vg.shadow
  now covers `<path>` (silhouette follows the real outline via
  fill_path_in_buffer_scaled + path_bounds, not a bbox rect) and `<text>`
  (offset dark copy under the glyphs; crisp — a blurred glyph shadow needs
  a glyph rasterizer vg lacks). test_effects 22 assertions. Remaining:
  **backdrop blur** / materials (frosted scrim) — needs framebuffer
  readback under a region (vg has none) or a native material
  (NSVisualEffectView / win32 acrylic / GTK has no clean backdrop-filter);
  a separate backend-heavy piece.
- **Transitions:** enter/exit transitions on overlay ENTRIES (the
  chrome fade-ins exist; per-entry slide/fade doesn't).
- **Layout:** weight-share min-clamping (weighted children can be
  squeezed below their minimum), RTL.
- **Bindings:** ~~list-typed state (`each_bind`)~~ **DONE 2026-07-18**
  (`ui_state_list` + `each_bind`; a `ui_set_list` re-runs `each_update`
  via a generic state-observer primitive), ~~two-way binding~~ **DONE**
  (`bind_value`/`textfield_bound`), ~~computed/derived state~~ **DONE**
  (`computed_s` — a void recompute closure fired on input change, seeds
  once). All three via the new `aether_ui_state_on_change` observer +
  `AEUI_STATE_LIST` cell, in all three backends; 5/5 (rbind) + 7/7
  (bindings) on GTK4. `table_bind` still open (needs the same over
  table_update). Design notes in docs/design/reactivity-unification.md §5.
- **Focus/shortcuts:** per-widget/conditional shortcut scopes, chorded
  shortcuts, auto menu↔shortcut binding (accel display exists; wiring
  is two lines by hand today).
- **Menus:** ~~GTK4 native menu wiring (GMenu/GActionGroup) is still the
  recorded stub~~ **DONE 2026-07-18** — the GTK4 menu bar is real
  (GtkPopoverMenuBar over a GMenu model; each item backed by a
  GSimpleAction on the app, registered in on_activate; separators split
  sections). `examples/menu` + `tests/menu/spec_menu` (GET /menus, POST
  /menu/{h}/activate, effect-observed counter) — **4/4 on GTK4 AND
  win32** (macOS expected-parity: same shared side-store + shared server
  routes, unverified this pass). Driver routes added to BOTH servers
  (house rule 5). `menu_popup` (standalone popup of a GMenu model) is now
  real on GTK4 too — `gtk_popover_menu_new_from_model` anchored under the
  widget, headless-no-op like the win32/macOS TrackPopupMenu/
  popUpMenuPositioningItem paths (so no driver assertion — the items stay
  reachable via /menu/{h}/activate). The drawn context menu remains the
  right-click surface.

## 3. Backlog (real, smaller, or gated)

- ~~**File dialogs** (`ui.open_file`/`save_file`)~~ **DONE 2026-07-18** —
  `open_file(title)` / `save_file(title, default_name)`, real on all three
  (GtkFileChooserNative + nested loop / GetOpenFileName+GetSaveFileName /
  NSOpenPanel+NSSavePanel). Headless-no-op (returns ""), so no spec —
  verified headless-safe (no hang) on GTK4 + win32. (gp's xdg-open is a
  file *launcher*, a separate thing, still there.)
- **Tab view** (`ui.tabs`) — navstack exists; tabs don't. S–M.
- **Drag & drop** — inter-widget first (list reorder), inter-app later. M.
- **Multi-window** — one window per app today (surface table is
  single-window by design). M, needs a driver story (window ids in
  `/widgets`). Defer until an app needs it.
- **Accessibility** — native widgets get GTK/AppKit a11y free; anything
  vg-DRAWN (the dropdown, a plan-B table) needs a semantics bridge
  eventually — this is the real cost of the drawn path, and why Flutter
  maintains a semantics tree. Track it; don't block on it.

## The strategic fork (not scheduled): vg-drawn controls

The Flutter turn — a widget set *drawn by vg* — would make every control
pixel-identical across backends, fully themeable, effect-capable, and
immune to compositor politics. The costs are exactly Flutter's costs:
you take on a11y (semantics tree), IME/text input (brutal), and
"platform-feel" drift. SwiftUI's hybrid is the likelier right answer for
a three-backend toolkit: **native text inputs, drawn chrome** — draw
what natives do worst across our platforms (menus, dropdowns, overlays,
maybe tables), keep native what they do best (entries, IME, a11y).
The overlay, typography, and effects layers were deliberately built as
the foundations of that hybrid, so this fork stays open without being
committed to. If/when it's serious, it gets its own comparison-doc
treatment first (the re-namespace plan's format: comparisons, verdict,
phased ci-gated migration).

## House rules (policy, carried forward)

1. **Driver-first widgets:** every new widget/effect ships with its
   `/widgets` JSON representation, driver routes, uidriver helpers, and
   an Aeocha spec in the same commit (the `toggle_group` precedent).
2. **Sommelier is a release gate** for anything compositor-adjacent —
   test on the Chromebook before calling it done; never let a Crostini
   workaround become the default (gate on `$SOMMELIER_VERSION`).
3. **Idle must cost zero** — the static-until-dirty discipline
   (region_set_animated lesson) applies to transitions and overlays too.
4. **Headless parity:** everything renderable must render via
   canvas_write_png so specs can screenshot it (the /screenshot path).
5. **Both-servers rule** (item 8's lesson): driver route changes land in
   the GTK4 embedded server AND backend/aether_ui_test_server.c. There are
   exactly TWO servers again (macOS gave up its private one, 2026-07-14),
   and the shared one now backs both win32 and macOS — so a route added
   there lands on two backends at once, and a route added ONLY to GTK4 is
   a spec that is red on two platforms for no reason. That is precisely
   how split/overlay/canvas/text_extent stayed red on both.
6. **Geometry specs pin the window** (item 7's lesson): GTK4 on a cold
   Xvfb can map windows at natural size — /window/resize first, then
   assert.
