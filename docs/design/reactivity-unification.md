# Reactivity unification: typed ui state + bindings (roadmap item 8, D0)

**Status: DECIDED.** 2026-07-13. The design round-trip briefs/bindings.md
Phase A demands. Three questions, three answers, then the API that falls
out. Baseline: ci green at e20d671.

## 1. Push vs pull — keep both, unify the vocabulary

The repo has two reactivity systems and they are BOTH right for their
half:

| | ui (widgets) | vg (scenes) |
|---|---|---|
| model | PUSH: `state_set` walks that state's bindings NOW, on the GTK thread | PULL: refreshing scenes re-eval bind closures per frame (`scene_eval_bindings`) |
| write rate | rare (a click, a scan tick) | continuous while animating |
| idle cost | zero (no clock) | zero (gated on `scene_set_refreshing`) |

Forcing widgets onto the frame clock would violate the idle-must-cost-
zero house rule for the common case (a static form pays a clock to
watch nothing). Forcing scenes onto eager pushes would fight the
record-then-flush pipeline (a bind write mid-frame would tear the
recording). **Decision: push for widgets, pull for scenes, ONE
vocabulary** — `bind_<property>(target, state-or-closure)` on both
sides, documented together. vg code does not change in this item
(`element_bind_fill` et al. already match the naming).

## 2. Typed state — one handle space, tagged cells

Today `ui_state` is doubles-only (`state_values: double*`). Options
considered: (a) parallel handle spaces per type (`string_states[]`…),
(b) one space of tagged cells. **Decision: (b) tagged cells.**
One allocator, one driver namespace, `/state/{id}` stays meaningful for
every state, and a binding stores one int handle regardless of type.

```c
typedef struct { int type; double num; char* str; } StateCell;
// type: 0=float 1=int 2=bool 3=string; num holds float/int/bool,
// str holds string (owned, strdup on set)
```

Backward compatibility is the float facet: `aether_ui_state_create/
get/set` keep their exact signatures and make type-0 cells. Existing
apps, externs, and specs are untouched.

New DSL (explicit suffixed verbs — Aether has no overloading):

```
sid = ui_state_s("scanning…")   ui_set_s(sid, "done")   ui_get_s(sid)
iid = ui_state_i(0)             ui_set_i(iid, 42)       ui_get_i(iid)
bid = ui_state_b(1)             ui_set_b(bid, 0)        ui_get_b(bid)
```

Cross-type get/set on a cell is a no-op/zero-value read — never a
crash, never a coercion (v1; revisit if a real consumer wants "42").

## 3. Driver JSON — additive, never break `/state/{id}`

- Float cells: `GET /state/{id}` → `{"id":N,"value":%.6f}` — BYTE-
  COMPATIBLE with today (existing specs keep passing).
- Typed cells: `{"id":N,"type":"int","value":42}`,
  `{"id":N,"type":"bool","value":true}`,
  `{"id":N,"type":"string","value":"…"}` (JSON-escaped).
- `POST /state/{id}/set?v=` parses per the cell's type (atoi / "1"/
  "true" / raw string / atof). Same route, same idle-hop discipline
  (typed sets walk bindings, so they MUST run on the GTK thread —
  the existing action-4 `test_action_idle` path gains the typed cases).

## 4. Bindings — data-carrying links, no closures

`TextBinding` generalizes to `PropBinding {kind, state, widget, prefix,
suffix, decimals, invert}` with kinds TEXT / ENABLED / HIDDEN. A state
write walks its bindings and applies each per kind. Closures are
deliberately NOT stored (the aether closure-capture history says:
prefer data), so float formatting is a decimals int, not a callback:

```
bind_text(lbl, st)              // smart default (int-valued → "42")
bind_text_fmt(lbl, st, 1)       // "3.1"
bind_enabled(btn, bst, 0)       // sensitive while bst true
bind_enabled(btn, bst, 1)       // inverted: ghosted while bst true
bind_hidden(w, bst, 0)          // hidden while bst true
```

`text_bound` (prefix + float + suffix) stays; it is now literally a
TEXT-kind PropBinding on a float cell.

## 5. Future overloads (recorded, not built)

- `each_bind(e, list_state)` / `table_bind(t, list_state)` need LIST-
  typed state; when that lands, an update to the list state calls
  today's explicit `each_update/table_update` internally. The explicit
  calls stay the primitive.
- ~~Two-way binding (textfield ⇄ string state) is out until a consumer
  demands it~~ **DONE 2026-07-18.** `bind_value(widget, string_state)` +
  the terse `textfield_bound(placeholder, state)` (SwiftUI
  `TextField(text: $state)` idiom). State→widget is an `AEUI_BIND_VALUE`
  PropBinding; widget→state is the field's change handler writing the
  state. Both directions compare-first, which is what breaks the echo
  loop. All three backends (GTK4 `changed` signal, win32 `EN_CHANGE` +
  `Widget.bound_state`, macOS `controlTextDidChange` +
  delegate `stateHandle`). Proven in `spec_bindings_demo` (7/7 on GTK4 +
  win32; macOS peer-equivalence pending).
- Computed/derived states: out; compose in app code.

## 6. Proving consumers (D3)

gp: the Stop/Rescan ghosting pair collapses onto one `scanning` bool
state (`bind_enabled(stop, scanning, 0)` + `bind_enabled(rescan,
scanning, 1)`), deleting the paired imperative `set_enabled` calls in
scan start/finish. The status line stays imperative — it is one label
with one writer; a binding would add a handle for nothing. The
calculator's display already renders `42` via the int-valued-float
default (the brief's "42.000000" citation predates that fix); its spec
keeps asserting `42`.

win32/macOS: storage + setters gain the same tagged cells (parity),
binding application uses each backend's existing set_text/set_enabled/
set_hidden primitives.
