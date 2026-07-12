# Brief: the in-window overlay layer (roadmap item 1)

**Status: PARTIALLY EXECUTED.** 2026-07-12. Phases A, B, and the toast +
modal-scrim half of Phase C are DONE (commit 526bd6a): the overlay host,
`overlay`/`overlay_modal`/`dismiss_overlay`/`toast` DSL, the modal glass
pane, `GET /overlays` + `GET /window/pick`, examples/overlay_demo + a
6/6 spec (ci.sh Phase 5b), win32/macOS stubs — full ci.sh green.
REMAINING: the drawn tooltip and the drawn dropdown (Phase C tail).
The rest of this brief is the original plan; the anchors still hold.

Written for an autonomous session; escalation triggers are explicit.

## Mission

Give every window a z-ordered overlay layer drawn *in-window* — Swing's
`JLayeredPane` / Flutter's `Overlay`, not compositor popups — so menus,
dropdowns, tooltips, and toasts work on every backend including hostile
compositors (sommelier/ChromeOS, where xdg_popups never display).
Concretely:

1. Backend primitive: open/close/z-order overlay surfaces above a
   window's root widget (GTK4 real; win32/macOS documented stubs).
2. DSL: `ui.overlay(…) { … }` + `ui.dismiss_overlay(…)`, content being
   any widget subtree (a `vg(…)` canvas is a widget, so drawn content
   comes free).
3. Three proving consumers: a **toast**, a **drawn tooltip** for vg
   scenes, and a **drawn dropdown** that replaces GtkDropDown-based
   `picker` on hostile compositors.
4. Modal scrim: a full-window overlay that eats clicks (Swing's glass
   pane).
5. Prove it: driver routes + Aeocha specs, full ci.sh green, screenshot
   verification.

## Ground truth (verified 2026-07-12 — start from here, not from digging)

- **GtkOverlay is already wrapped.** `aether_ui_zstack_create()`
  (`backend/aether_ui_gtk4.c:1060`) creates a `GtkOverlay`; the widget
  attach logic (`backend/aether_ui_gtk4.c:3546`) handles
  `GTK_IS_OVERLAY` — first child via `gtk_overlay_set_child`, later
  children via `gtk_overlay_add_overlay`. The DSL verb is `ui.zstack`
  (`ui/module.ae:474`). So the compositing widget exists; what's missing
  is a *per-window, runtime-managed* stack (open/dismiss after build
  time, z-order, scrim, input policy).
- **Windows set their root directly.** Main window: `on_activate`
  (`backend/aether_ui_gtk4.c` ~line 312) does
  `gtk_window_set_child(window, root)`. Extra windows:
  `aether_ui_window_create_impl` (line ~1485) +
  `aether_ui_window_set_body_impl` (~1500). To host overlays, interpose
  a GtkOverlay between window and root **for every window at creation**
  — root becomes the overlay's main child; overlay entries stack above.
  This is invisible when no overlay is open.
- **The sommelier wound, and the house two-surface pattern.** GTK4
  popovers (xdg_popup) report mapped=1 but never display under
  sommelier; a stock GtkMenuButton/GtkDropDown is equally invisible
  (verified — see the CtxMenu comment block,
  `backend/aether_ui_gtk4.c:395-450`). Detection:
  `aeui_ctx_use_window()` (line ~439) — `$SOMMELIER_VERSION` presence,
  `$AETHER_UI_MENU=window|popover` force-override for testing. The
  drawn dropdown should follow this exact pattern:
  `$AETHER_UI_PICKER=drawn|native` force flag + sommelier default.
- **Picker today:** `aether_ui_picker_create` = GtkDropDown
  (`backend/aether_ui_gtk4.c:927-960`), with `picker_add_item`,
  `picker_set_selected`, `picker_get_selected`. Keep this API surface —
  the drawn variant is a second *surface* behind the same handle, not a
  new widget type.
- **vg tooltips resolve but display natively.** `tooltip_at(ctx, x, y)`
  (`vg/grammar/events.ae:250`) resolves the topmost element's tooltip;
  today it feeds `gtk_widget_set_tooltip_text` on the canvas
  (`backend/aether_ui_gtk4.c:1319`) — native GTK tooltips are
  compositor surfaces too (same sommelier risk). The drawn-tooltip
  consumer replaces the *display*, reusing the existing resolution.
- **In-scene z-order already exists** but is scene-internal:
  `_render_live_z_ordered` (`vg/live.ae:225`) composites live regions
  low→high z inside ONE canvas (`region_z`/`region_set_z`,
  `vg/region.ae:174`). The overlay layer is the *widget-level* sibling
  of this idea — do NOT try to build overlays inside a single vg scene;
  windows are widget trees, not canvases.
- **Driver/spec infra:** test server routes live in
  `backend/aether_ui_gtk4.c` ~2495+ (`GET /widgets` ~2956,
  `GET /screenshot` ~3033, click routing ~3117). Specs use
  `tests/lib/uidriver.ae` via `tests/run_spec.sh` (env
  `UI_SPEC=<app>/<spec>`, needs `AEOCHA_DIR=~/scm/aeocha`); ci.sh
  phases 3–6 run them.

## Design decisions already made (don't relitigate)

- **Overlay content = widget subtree**, hosted by the per-window
  GtkOverlay. Not a second compositor surface (that's the disease), not
  a vg-scene-internal hack. vg content mounts as a `vg(…)` canvas child.
- **Z-order = stacking order** (later `gtk_overlay_add_overlay` = on
  top). An explicit z parameter is NOT needed for v1 — dismiss/reopen
  reorders. Keep the ABI dumb.
- **Positioning:** overlay entries are positioned by
  halign/valign + margins (GtkOverlay child properties via
  `gtk_widget_set_halign/valign/margin_*`) for v1: enough for toasts
  (bottom-center), dropdowns (computed margins from the trigger
  widget's allocation via `gtk_widget_compute_bounds`), tooltips
  (pointer-adjacent margins). Free-floating x/y placement beyond that
  is out of scope.
- **Input policy:** a non-modal overlay entry receives clicks on its
  own content only (GTK default targeting). A **modal** overlay adds a
  scrim child first — full-window, styled translucent, with a click
  gesture that (a) eats the click and (b) fires an on-dismiss closure.
  No global event-grab machinery.

## Deliverables

### D1 — backend primitive + per-window host
- Interpose a GtkOverlay in BOTH window-creation paths (`on_activate`,
  `window_set_body_impl`). Store it in the window entry.
- New C ABI (names follow `aether_ui_*_impl` convention):
  - `aether_ui_overlay_open_impl(win_handle, content_handle, anchor: int,
    dx: int, dy: int, modal: int) -> int` — anchor enum encodes the 9
    halign/valign combos; dx/dy are margins; modal inserts the scrim.
    Returns an overlay handle.
  - `aether_ui_overlay_close_impl(overlay_handle)`
  - `aether_ui_overlay_set_on_dismiss_impl(overlay_handle, closure)` —
    scrim click / Escape. Closure retention: this is an extern C sink
    that RETAINS the closure — the ae-0.255 drain-gate fix covers it,
    but follow the CtxMenu boxed-closure pattern exactly.
- win32 (`backend/aether_ui_win32.c`) + macOS
  (`backend/aether_ui_macos.m`): documented no-op stubs returning 0
  (house pattern — `toggle_group` precedent), so the fan-out links.

### D2 — DSL surface (`ui/module.ae`)
- `overlay(anchor: string, dx: int, dy: int) { …content… } -> int` —
  builder verb; block children become the content subtree. Remember:
  `ui_backend` is a *builder*; bare setters bind to the injected `_ctx`;
  `_ctx` is not passable (aeb-migration memory). Block-form callbacks
  need the `callback` postfix keyword.
- `overlay_modal(…) { … } -> int` (scrim variant, takes `on_dismiss`
  callback), `dismiss_overlay(handle: int)`.
- Driver route: `GET /overlays` (JSON list: handle, modal, mapped) +
  make `/widgets` see overlay-hosted widgets (verify it already does —
  they're registered widgets; if the registry walk covers them, just
  add the spec assertion).

### D3 — the three consumers (each its own commit + spec)
1. **Toast:** `ui.toast(win, text, ms)` — bottom-center overlay,
   auto-dismiss via the existing house timer
   (`aether_ui_timer_create_impl`/`_cancel_impl`,
   `backend/aether_ui_gtk4.c:1439` — do not invent a new one). Spec: open, assert visible via /overlays + screenshot,
   assert gone after expiry.
2. **Drawn tooltip for vg:** on hover-dwell over an element with a
   tooltip, open a small overlay (label in a styled frame) near the
   pointer; close on leave. Reuse `tooltip_at` resolution — the change
   is display-side only. Gate the drawn path behind the same hostile-
   compositor detection (native GTK tooltips are fine elsewhere);
   `$AETHER_UI_TOOLTIP=drawn|native` force flag for testing.
3. **Drawn dropdown:** picker gains a second surface — a button-styled
   trigger + overlay list (GtkListBox or drawn rows) — selected by
   `$AETHER_UI_PICKER=drawn|native`, defaulting to drawn under
   `$SOMMELIER_VERSION` (mirror `aeui_ctx_use_window()`). Same picker
   ABI: add_item/set_selected/get_selected must behave identically on
   both surfaces (spec asserts this with the force flag on).

### D4 — modal scrim proof (small)
An example (`examples/overlay_demo/` per-app aeb node, like
`examples/context_menu/`) exercising: non-modal toast, modal
confirm-ish overlay (scrim + two buttons), drawn dropdown with the
force flag. This is the app the specs drive.

### Stretch (only if A–E green with time left; separate commits)
- Convert the CtxMenu sommelier window-fallback to an overlay
  (retiring the "menu behind the app / second-tap" wart documented at
  `backend/aether_ui_gtk4.c:409-414`). Do NOT touch the popover
  default path.
- Escape-key dismissal wired through the canvas key path.

## Execution phases — each ends with a commit; gates are hard

**Phase A — characterize before touching.**
1. `rm -rf ~/.aether/cache`; full `./ci.sh` for the baseline (must be
   green before you start; if it isn't, STOP and report).
2. Probe app (scratchpad, not committed): a window whose root is
   `ui.zstack` with a button as main child and a label added later —
   confirm (a) add_overlay stacking works at runtime post-show,
   (b) input targeting: does the button under a *partially covering*
   overlay child still click? (c) `gtk_widget_set_can_target(FALSE)`
   makes an overlay child click-transparent. Screenshot each state via
   the driver. These answers fix the input-policy implementation; if
   (a) fails (GTK refuses runtime add after realize), the interposed-
   overlay design still works but entries must be pre-created hidden —
   note it and adapt.
3. Write down the sommelier facts you CANNOT test here (no sommelier on
   this box): the design avoids compositor surfaces *by construction*,
   so Xvfb-passing = expected-sommelier-passing for overlay content.
   Say so in commit messages rather than claiming sommelier-verified.

**Phase B — D1 + D2.** Backend host + ABI + DSL + `GET /overlays`
route + a minimal spec (open non-modal, assert, dismiss, assert gone;
modal variant asserts the scrim eats a click — a counter button under
the scrim must NOT increment). Gate: new spec green + full ci.sh green
(the interposed GtkOverlay must not break ANY existing spec — every
window now has one; watch the 5 gp specs and screenshot-based asserts
for pixel shifts; there should be none, GtkOverlay adds no chrome).
Commit.

**Phase C — D3 consumers, one commit each,** in order toast → tooltip
→ dropdown (ascending risk). Each: implementation + spec + example
wiring in overlay_demo. Gate per commit: its spec + full ci.sh.

**Phase D — D4 example polish + docs.** overlay_demo in the ci.sh
example fan-out + spec phase; a `docs/guide/` page if that tree has
per-feature pages (check `ls docs/guide/` — follow whatever structure
exists; don't invent a new doc system).

**Phase E — verification + closeout.**
1. Screenshots (Xvfb recipe below), EYEBALL each: toast over content;
   modal scrim dimming; dropdown open list over widgets; tooltip near
   a vg shape. Attach findings to the final commit message.
2. `AETHER_UI_PICKER=drawn` run of any existing example using picker —
   confirm behavioral parity.
3. Mark roadmap item 1 DONE (house style: see item 2's DONE block,
   `roadmap.md:58` — cite commits), delete this brief in that same
   commit (precedent: 2e72530).

## Acceptance checklist (machine-checkable except the eyeballs)

- [ ] Every window hosts an overlay layer; zero visual/spec diff when
      no overlay is open (full ci.sh green at Phase B).
- [ ] `ui.overlay` / `ui.overlay_modal` / `ui.dismiss_overlay` exported;
      `GET /overlays` route live; specs cover open/dismiss/scrim-eats-
      click.
- [ ] Toast, drawn tooltip, drawn dropdown shipped, each spec'd.
- [ ] Picker parity: same ABI results on native and drawn surfaces
      (spec runs with `AETHER_UI_PICKER=drawn`).
- [ ] win32/macOS stubs in place; nothing new links against GTK in
      shared code paths.
- [ ] Full `./ci.sh` green (Phase 0 units, fan-out, smoke, example
      specs, gp specs).
- [ ] roadmap.md item 1 marked DONE; briefs/overlay.md deleted.

## Environmental traps (every one has cost a real debugging session)

- **Compiler cache:** `rm -rf ~/.aether/cache` after ANY edit to an
  imported module. Stale caches produce impossible E0301s and silently
  run old code.
- **Driver apps:** end them with `curl -X POST :9222/shutdown` — NEVER
  by killing the xvfb-run wrapper. Port is always 9222;
  `AETHER_UI_TEST_PORT` must be SET to arm the server (value ignored).
  One driver app at a time: `curl -sf :9222/widgets || echo free`.
- **Xvfb:** `xvfb-run -a -s "-screen 0 3200x2000x24"` +
  `GSK_RENDERER=cairo` (NGL on llvmpipe leaks GBs). On this box also
  `GDK_BACKEND=x11 WAYLAND_DISPLAY=` (gp memory). The big screen keeps
  the parked pointer out of the window.
- **Screenshots:** `GET /screenshot` 500s transiently — retry once.
- **Aeocha:** specs need `~/scm/aeocha` (AEOCHA_DIR); run via
  `tests/run_spec.sh`, not `ae run`.
- **Closures into C:** box + retain per the CtxMenu pattern; the
  escape-analysis UAF class is FIXED in ae 0.255 but the pattern is
  still the contract.
- **Language:** `state`/`spawn`/`match` are reserved; no top-level
  mutable globals; `main()` takes no return type; builder block-form
  callbacks need the `callback` postfix keyword; `${}` interpolation
  can't contain string literals — use temp vars; multiple `opt_s(...)`
  calls in ONE println interpolation corrupt — assign to vars first.
- **aeb:** per-app `.build.ae` nodes; binaries at
  `target/build/<tree>/<app>/bin/<app>`. `_ensure_built` in the
  conformance harness only builds MISSING binaries — after editing
  shared modules, `rm -rf target/build/apps/<app>` to force rebuilds.

## Out of scope — do not do these even if tempting

- No system-wide/multi-window compositor ambitions (live-regions memory:
  in-scene composition is free; cross-window is out of scope).
- No free x/y overlay placement API beyond anchor+margins (v1).
- No win32/macOS overlay IMPLEMENTATIONS (stubs; real ones next time on
  winbaz / the Mac mini).
- No replacing the CtxMenu popover DEFAULT path (stretch touches only
  the sommelier fallback, and only if A–E are green).
- No removing GtkDropDown; the native picker surface stays the default
  off-sommelier.
- No aeocha/aether (sibling repo) changes. Suspected compiler bug:
  minimal probe in the scratchpad, document, STOP and report.

## Escalation triggers — stop and report instead of pushing through

1. Phase-A probe shows GtkOverlay runtime-add fundamentally broken
   (not just needing the pre-created-hidden adaptation) → the design
   premise fails; report with the probe.
2. Interposing the overlay host breaks existing specs in a way that
   isn't a trivial re-anchor (e.g. allocation/size-request changes
   cascading into gp's canvas geometry) → report; do not paper over
   with spec edits.
3. Input routing needs a grab/capture mechanism GTK4 doesn't expose
   cleanly → report the options rather than building a global event
   filter.
4. The drawn dropdown can't reach ABI parity (get_selected timing,
   change-callback ordering) → ship toast+tooltip, report the delta.
