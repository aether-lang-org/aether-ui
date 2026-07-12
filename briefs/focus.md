# Brief: focus, tab order, shortcuts (roadmap item 9)

**Status: READY FOR EXECUTION.** 2026-07-12. Independent. M-sized.
Swing's `InputMap`/`ActionMap` is the named ancestor (declarative
keystroke→action per scope); SwiftUI `.keyboardShortcut`, QML
`Shortcut` are the modern spellings.

## Mission

1. `ui.shortcut("Ctrl+R") callback { … }` — per-window accelerators.
2. Sane default tab order (build order) + `ui.focus(handle)` +
   `ui.focus_group(...)` where the default fails.
3. Driver: `GET /focus` (who has it) + `POST /widget/{id}/focus` +
   `POST /window/key?combo=Ctrl+R` so specs can assert all of it.
4. Proving consumers: gp gets `Ctrl+R` = Rescan and `Delete` = its
   existing delete flow via shortcuts instead of (or alongside) the
   canvas key handler; menu items SHOW their accelerator text.

## Ground truth (verified 2026-07-12)

- **Zero shortcut machinery today:** `grep GtkShortcut|accelerator`
  in `backend/aether_ui_gtk4.c` → 0. Menu items render label-only.
- **The focus dance (the wound):** canvases are made focusable at
  creation (`gtk4.c:2337-2338`), `canvas_on_key` registration grabs
  focus — but only "once the widget is actually on screen"
  (`:2543-2556`, a map-signal dance), and a legacy-controller
  button-press re-grab exists because "the gesture-based focus-click
  never fires on sommelier" (`:2460-2461`). Every app that wants keys
  repeats this dance implicitly by using the canvas.
- **gp's keyboard nav is app-wired:** one `canvas_on_key` handler
  (`apps/grand_perspective/grand_perspective.ae:283`) dispatching
  Up/Down/Left/Right/Delete itself. Works, but is per-app and
  canvas-scoped: keys die when focus is on a button.
- **GTK4 primitives:** `GtkShortcutController` (scope MANAGED/LOCAL/
  GLOBAL — GLOBAL on the window gives accelerator behavior),
  `gtk_shortcut_trigger_parse_string("<Control>r")`,
  `GtkCallbackAction`. Tab order: GTK gives a default focus chain from
  the widget tree already — the deliverable is mostly *not breaking
  it* + explicit `focus()`.
- **Driver key delivery today** is canvas-only
  (`POST /canvas/{id}/key?name=`). Window-level combo delivery is new.
- **Precedents:** closure boxing into C (CtxMenu pattern);
  route + uidriver helper + spec in the same commit (house rule 1);
  sommelier gating (`aeui_ctx_use_window` pattern) if popover-adjacent
  — shortcuts are NOT compositor surfaces, so no sommelier risk.

## Design decisions already made

- **Window-scoped GLOBAL GtkShortcutController** carries
  `ui.shortcut` — fires regardless of which widget has focus (Swing
  InputMap WHEN_IN_FOCUSED_WINDOW semantics). Per-widget scopes are a
  later need.
- **Combo strings use GTK parse syntax on the wire**
  ("Ctrl+R" → "<Control>r" translated in the wrapper; accept both).
- **`/window/key` route synthesizes through the SAME dispatch the
  controller uses** (activate the parsed trigger) — NOT a fake input
  event; this is the honest, compositor-independent test path (the
  /window/pick lesson).
- **Menu accelerator display only** — wiring a menu item to a shortcut
  is the app author writing both lines v1; auto-binding is a follow-up.
- **Canvas key handling is untouched** — gp's arrow-key nav stays;
  shortcuts complement (Ctrl+R works even when a button has focus).
- **Escape-to-dismiss overlays** — wire the modal scrim's Escape via
  this system (retires the overlay brief's old stretch item).

## Deliverables

- **D1** `ui.shortcut(combo, cb)` (+ `shortcut_of(win, ...)` for extra
  windows), boxed-closure C impl, combo translation, menu-item accel
  display (`menu_item(label, accel_text)` overload or a setter).
- **D2** Driver: `GET /focus` → `{"id":N,"type":...}`;
  `POST /widget/{id}/focus`; `POST /window/key?combo=`. uidriver
  helpers.
- **D3** `ui.focus(handle)` + verify default tab order on the testable
  example (spec: focus field, Tab via key route, assert /focus moved
  in build order). `focus_group` only if the probe shows GTK's default
  chain misbehaving in our stacks — otherwise document that the
  default IS the feature and skip the API (honest minimalism).
- **D4** Consumers: gp Ctrl+R→Rescan + Delete→delete-flow (spec: combo
  route → status text changes, existing delete spec still green);
  overlay modal Escape→dismiss (spec: open modal, POST /window/key
  Escape, scrim gone via /window/pick).
- Example: extend `examples/testable/` (it's the driver playground)
  rather than a new app; specs in its existing spec file + gp's.

## Phases — commits + hard gates

**A** — ci.sh baseline green. Probes: GtkShortcutController GLOBAL on
our window fires with focus on a button/canvas/entry (three probes);
trigger-activation path for the /window/key route; default Tab chain
across a vstack of mixed widgets.
**B** — D1+D2 (+specs). Commit. **C** — D3. Commit. **D** — D4.
Commit. **E** — roadmap item 9 DONE, delete brief. Item 9 is the last
ranked item — note in the closeout commit that the ranked roadmap is
complete and the backlog/strategic-fork sections are what remains.

## Acceptance

- [ ] ui.shortcut + focus + routes + helpers + specs; full ci.sh green.
- [ ] Menu items can display accelerator text.
- [ ] gp Ctrl+R/Delete + overlay Escape shipped and spec'd.
- [ ] Tab-order behavior documented (API only if needed).
- [ ] win32/macOS stubs compile.
- [ ] roadmap item 9 DONE; brief deleted.

## Traps (abridged)

- Standard cache/driver/Xvfb set (briefs history).
- Keyboard events + headless Xvfb: the /window/key route must NOT
  depend on a real seat/keymap — activate the trigger, don't inject
  XTest events.
- The sommelier focus-click lesson (gtk4.c:2460) — do not rely on
  gestures for anything focus-adjacent; controllers only.
- Entry widgets swallow keys: the GLOBAL scope choice matters — verify
  Ctrl+R fires while an entry has focus, but plain "r" must NOT.
- gp delete spec really trashes a fixture — isolation is per-spec
  fresh instances; don't reorder its phases.

## Out of scope

- Per-widget/conditional shortcut scopes; chorded shortcuts; a11y
  focus ring styling; IME anything; win32/macOS implementations;
  configurable keymaps; auto menu↔shortcut binding.

## Escalation

1. GLOBAL controller doesn't fire over focused entries/canvases →
   report GTK findings before inventing an event filter (the overlay
   brief's input-routing trigger, same spirit).
2. /window/key can't reuse the controller dispatch honestly → report;
   don't ship an XTest-based route.
