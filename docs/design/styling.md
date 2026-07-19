# AeCS — Aether Cascading Styles

**AeCS** is aether-ui's stylesheet layer: a declarative, cascading styles
model over the imperative `style_*` setters, so a theme is a *document* and
re-theming a running app is one call. The name follows the family convention
(AeVG, aeb, Aeocha) and says what the layer actually does — the cascade
(`class.kind → class → kind → container → root`) is the mechanism.

## Lineage (credit)

The design is modelled on **Swiby** (Jean Lazarou & the Swiby committers,
BSD-licensed) — specifically `create_styles` / `apply_styles` / `load_styles`
and the banking demo's Settings dialog, which live-previews a theme on one
panel and then applies it to the whole running app. Swiby's resolution order
(id → class → element → root) is the model here, minus ids (v1). The
`themes_demo` themes are lifted from swiby's `demo/banking/theme/*.rb` values.

## API (pure module.ae)

```
s = create_styles()
st_font_size(s, "root", 11.0)         // the cascade's fallback
st_color(s, "text", 0x5C458A)         // element rule (driver kind names)
st_color(s, "button", 0x5C458A)
st_bg(s, "container", 0xD6CFE6)       // alias for vstack/hstack/zstack
st_bold(s, "header.text")             // class-scoped rule; widgets opt in
st_color(s, "header.text", 0x6030BF)  //   via add_css_class(w, "header")
apply_styles(0, s)                    // restyle the LIVE tree (0 = all;
                                      //   else a subtree root handle)
```

Properties: `st_color` (text, packed 0xRRGGBB like swiby's themes), `st_bg`,
`st_font_size`, `st_weight` (two-valued — `0` genuinely un-bolds on a
re-theme; `st_bold` is sugar for `st_weight(…, 1)`), `st_radius`,
`st_font_family` (verbatim to the platform — generic CSS families resolve on
GTK4/macOS; on win32 prefer real face names like `"Consolas"`).

Resolution per property, first match wins (swiby's full order):
`#id` → `class.kind` (**every** class, in add order) → `class` (every) →
`kind` → `container` (stacks only) → `root`.

Ids: `style_id(w, "submit")` names a widget for `"#submit"` rules. Stored as
a `#`-prefixed token in the class list — one shared store, tracked on all
three backends, visible in the driver's `classes` field.

Dark/light pairs: `styles_for_mode(light, dark)` returns the sheet matching
`is_dark_mode()` right now. Re-call + re-apply on a mode flip (no
appearance-change event yet).

## How it works

`apply_styles` walks the live widget registry in-process — four tiny ABI
getters per backend (`widget_count/kind/parent/classes_impl`, wrapping state
the driver hooks already kept) — resolves each property down the cascade, and
calls the existing cross-backend setters (`set_text_color`, `set_bg_color`,
`set_font_size`, `set_font_bold`, `set_corner_radius`). No new rendering
machinery; the sheet is an interpreter over what already worked.

Re-theming = `apply_styles` with a different sheet (the banking-demo move).
New subtrees built after an apply need a re-apply — same as swiby's
`use_styles` per new screen.

## Driver contract (both servers)

The backends' color setters stash the last explicitly-set values;
`aether_ui_styled_bg/fg_impl` read them back and the widget JSON emits
`"bg"/"fg":"#rrggbb"` when present. So a spec proves a theme swap changed
what the platform was actually given, not just the DSL model.
`spec_themes_demo`: element rule, class-beats-element, container bg, and a
live re-theme flipping the same widgets to a second theme's colors.

## The reset-sheet convention (fake un-styling, honestly)

AeCS **overwrites but never unsets**: applying a sheet without a color leaves
the previous color in place (there is no "revert to platform default" ABI).
The convention: **every theme is a full restatement** — each sheet sets every
property any sibling theme sets, so switching themes always overwrites
everything. With `st_weight` two-valued (v1.1), this now genuinely covers
weight too; `themes_demo`'s green theme is the worked example.

## v1 boundaries closed in v1.1 (2026-07-19)

- ~~One class per widget~~ — **all** classes resolve, in add order.
- ~~No id selectors~~ — `style_id` + `"#name"` rules, highest precedence.
- ~~`st_bold` set-only~~ — `st_weight(…, 0/1)`; re-themes un-bold.
- ~~No `font_family`~~ — `aether_ui_set_font_family` landed on all three
  backends (GTK4 Pango attr / CSS; win32 LOGFONT face name; macOS NSFont) +
  `st_font_family` in the sheet + `fontFamily`/`fontWeight` driver readback.
- Dark/light: `styles_for_mode(light, dark)`.

## Still-open boundaries (deliberate)

- `style_opacity` is not in the sheet (its CSS route is a win32 no-op).
- Styles apply at `apply_styles` time, not at widget construction — a
  "current sheet consulted by constructors" layer can come later without
  changing the model.
- No live re-matching, inheritance, pseudo-states, or descendant
  combinators — those would make AeCS a style *engine*; see the honest
  AeCS-vs-CSS comparison in the session notes.
