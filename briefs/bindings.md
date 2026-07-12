# Brief: declarative bindings + typed state (roadmap item 8)

**Status: READY FOR EXECUTION — but Phase A is a DESIGN round-trip, not
code.** 2026-07-12. The roadmap's own risk note: "unifying two
reactivity systems is the design work, not the code." M-sized.
SwiftUI `@State`/`@Binding`, QML property bindings, and our own
vg.grammar.bind are the models — the last one is in-repo prior art.

## Mission

1. Typed ui state: string/int/bool alongside today's float.
2. `bind_text(handle, state[, fmt])`, `bind_enabled(handle, state)`,
   `bind_hidden(...)` — widget properties that track state without
   imperative `set_text` calls at every write site.
3. One reactivity story: ui-side bindings and vg-side bindings driven
   by the same refresh discipline (not necessarily the same code — the
   DESIGN decides; see Phase A).
4. Driver: typed `/state` routes; specs prove set→propagate.
5. Proving consumers: the calculator's `42.000000` display becomes
   `42`; `each`/listbox gain their binding overload IF item 3/4 landed
   (else note follow-up).

## Ground truth (verified 2026-07-12)

- **ui_state is doubles-only end to end:** externs
  `ui/module.ae:104-107` (`state_create/get/set` all float;
  `state_bind_text(state, text, prefix, suffix)`), C at
  `backend/aether_ui_gtk4.c:118-139` (`double` storage; TextBinding
  array re-rendered on set). The calculator displays `42.000000` — the
  formatting lives in the C re-render (find the snprintf near the
  TextBinding walk).
- **text_bound exists** (`ui/module.ae:25`, wrapper near :670s) — it IS
  a binding (prefix + float + suffix). So "bindings" aren't new here;
  TYPED values and MORE properties are.
- **vg has the richer story:** `vg/grammar/bind.ae` (region items /
  render / trackby / update closures), `vg/grammar/reactive.ae`,
  `element_bind_fill` (`element.ae:76`), `scene_set_refreshing`
  (`vg/module.ae:238-241`) + `scene_eval_bindings` — per-frame
  re-evaluation gated by the refreshing flag (zero idle).
- **Driver:** `GET /state/{id}` returns `{"id":N,"value":%.6f}`;
  `POST /state/{id}/set?v=` exists (float). Typed routes are additive.
- **The two systems differ fundamentally:** ui-state is PUSH
  (set → walk bindings now, on the GTK thread); vg bindings are PULL
  (refreshing scenes re-eval closures per frame). The unification
  design must pick push, pull, or "push for widgets, pull for scenes,
  one vocabulary" — the last is the likely answer; Phase A argues it
  properly.

## Design decisions already made (the floor, not the ceiling)

- **No new reactive language.** States are handles; bindings are
  (state → property) links; a write propagates. No computed/derived
  states v1 (note as follow-up), no two-way bindings except where they
  already implicitly exist (textfield callback ≠ binding; leave it).
- **Typed state = separate handle spaces or a tagged cell — Phase A
  picks**, with the driver JSON shape decided at the same time
  (`{"id":N,"type":"string","value":"…"}`).
- **Float display formatting is part of D1:** `bind_text` for a float
  takes an optional fmt ("%d"-like or a decimals int) — the calculator
  wound dies as the proving consumer.
- **vg is NOT rewritten.** The design memo may align names (e.g.
  `bind_*` vocabulary) and the refresh discipline; vg code changes only
  if the memo shows a cheap true win.
- **Backward compatibility:** existing `ui_state/ui_get/ui_set/
  text_bound` keep working unchanged (they become the float facet).

## Deliverables

- **D0 (Phase A output)** — `docs/design/reactivity-unification.md`:
  the push/pull analysis, the typed-state representation choice, the
  driver JSON, what item 4's `table_update` and item 3's
  `each_update` overloads will look like when they arrive. SHORT
  (≤2 pages), decision-oriented, in the re-namespace comparison-doc
  tradition. Committed before any code.
- **D1** — typed state (string/int/bool) + `ui_get_s/ui_set_s` etc. (or
  the tagged API the memo picks) + typed driver routes + C storage.
- **D2** — `bind_text` (typed + fmt), `bind_enabled`, `bind_hidden`.
  Calculator shows `42`. `examples/bindings_demo/` + Aeocha spec
  (set via driver route → label/enabled/hidden all track; ci.sh next
  free phase).
- **D3** — gp adoption where it deletes code (status line, Stop/Rescan
  enabled-ness are candidates) — only the drop-in wins; gp specs green.

## Phases — commits + hard gates

**A** — ci.sh baseline; write + commit D0 (the memo). GATE: the memo
answers push-vs-pull, representation, and driver shape explicitly.
**B** — D1 (+ typed routes + route spec). Commit.
**C** — D2 (+ demo + spec; calculator fmt fix — its spec asserts the
display, update the assertion to the better output and say so). Commit.
**D** — D3. Commit. **E** — roadmap item 8 DONE, delete brief.

## Acceptance

- [ ] docs/design/reactivity-unification.md committed (the decision
      record).
- [ ] string/int/bool state + typed /state routes + specs.
- [ ] bind_text/bind_enabled/bind_hidden shipped; calculator shows
      `42` not `42.000000` (spec updated deliberately).
- [ ] ui_state float API untouched; full ci.sh green.
- [ ] roadmap item 8 DONE; brief deleted.

## Traps (abridged)

- Standard cache/driver/Xvfb set. ui/module.ae imports after exports.
- TextBinding re-render runs on state_set — thread-safety: driver
  routes hop via g_idle_add for actions; typed setters must keep that
  discipline (see test_action_idle precedent).
- The closure heap-string capture bug (aether ask) matters if bindings
  store formatting closures — prefer data (fmt string/int) over
  closures v1.
- gp/calculator specs assert display text — changing formatting is a
  DELIBERATE assertion update, documented in the commit.

## Out of scope

- Computed/derived state, two-way binding, expression bindings (QML's
  full language), reactive LIST state (item 3/4 update calls stay
  explicit; only note the future overload shape in the memo), vg
  rewrite, win32/macOS beyond storage parity (state lives in shared C?
  — verify: state table is per-backend; win32 has its own — keep
  parity for the typed additions).

## Escalation

1. The memo concludes unification needs deep vg changes to be honest →
   stop after D0, present the memo (that IS a valid deliverable).
2. Typed storage forces ABI churn on existing float routes → design
   around (new routes), never break `/state/{id}`.
