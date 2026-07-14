# Roadmap: what remains

**Status: PARED DOWN.** 2026-07-14. The nine-item ranked roadmap of
2026-07-12 (overlay layer, typography, `each`, table/list, effects,
transitions, layout, bindings, focus) was executed in full on
2026-07-12/13 — see git history for the original document with its
per-item DONE blocks, evidence tables, and commit trails (last full
version at 1ade88b; the wound-driven method writeup lives there too).
This document now lists ONLY what is not done, in three buckets:
platform parity, deferred follow-ups, and the backlog/strategic fork.

## 1. Platform parity — macOS and Win32 catch-up (the big one)

Everything below is GREEN on GTK4/Linux and stubbed-but-compiling on
the other two backends. The stubs were deliberate (ABI-first, so apps
and specs are portable on day one); paying them down is now the largest
coherent block of work. Both boxes are reachable: winbaz over ssh
(build.sh + tests/test_driver.sh is the proven loop), the Mac mini via
the run_on=host aeb-agent.

| Feature | GTK4 | win32 / macOS today | Real implementation needed |
|---|---|---|---|
| Text metrics (`vg.text_extent` etc.) | cairo | stubbed → 0 | GDI / CoreText metrics |
| splitview | GtkPaned | plain stack, no divider | Win32 splitter / NSSplitView |
| weight / on_layout / wrap | AeuiFlexLayout, GtkFlowBox | no-op / plain hstack | per-backend layout pass |
| shortcut / focus / /window/key | GtkShortcutController GLOBAL | no-op | accelerator tables / NSEvent monitors + focus APIs |
| Overlay layer (toast/modal/tooltip/dropdown) | GtkOverlay host | stubs | per-backend overlay host |
| Menu accelerator display | drawn ctx-menu items | inherits (shared path) | native menu accel columns when real menus land |
| GET /focus + /window/key routes | embedded GTK server | absent from shared test server | port routes to aether_ui_test_server.c |

Notes for whoever picks this up:
- The **shared test server** (`backend/aether_ui_test_server.c`) is the
  win32/macOS driver path — route additions on GTK4's embedded server
  must be mirrored there (the typed /state routes already were; the
  focus/key routes were not).
- **State/bindings are already at parity** (typed cells + PropBindings
  transplanted verbatim into all three backends, proven live on winbaz).
- macOS has had one full bring-up pass (AppKit backend green, driver
  17/17, all examples build) — parity work is incremental from there,
  not a bring-up.

## 2. Deferred from the ranked items (recorded at each closeout)

- **Typography:** multi-line wrap (`text_wrapped`), text-anchor
  middle/end.
- **Table/list:** virtualization (the GtkColumnView / recycled-cell vs
  handle-registry question), delegate cells (%-bars, chips), row
  double-click, multi-select, **tree mode**.
- **Effects:** shadows on paths/text (rect+circle only today),
  **backdrop blur** / materials (frosted scrim).
- **Transitions:** enter/exit transitions on overlay ENTRIES (the
  chrome fade-ins exist; per-entry slide/fade doesn't).
- **Layout:** weight-share min-clamping (weighted children can be
  squeezed below their minimum), RTL.
- **Bindings:** list-typed state (`each_bind`/`table_bind` — an update
  to a list state drives today's explicit `each_update`/`table_update`
  internally), two-way binding (textfield ⇄ string state), computed/
  derived state. Design notes in docs/design/reactivity-unification.md §5.
- **Focus/shortcuts:** per-widget/conditional shortcut scopes, chorded
  shortcuts, auto menu↔shortcut binding (accel display exists; wiring
  is two lines by hand today).
- **Menus:** GTK4 native menu wiring (GMenu/GActionGroup) is still the
  recorded stub — menu items don't fire on GTK4; the drawn context menu
  is the working surface.

## 3. Backlog (real, smaller, or gated)

- **File dialogs** (`ui.open_file`/`save_file` → GtkFileDialog/NSPanel/
  IFileDialog) — gp shells out to xdg-open today. S, do opportunistically.
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
   the GTK4 embedded server AND backend/aether_ui_test_server.c.
6. **Geometry specs pin the window** (item 7's lesson): GTK4 on a cold
   Xvfb can map windows at natural size — /window/resize first, then
   assert.
