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

Properties (v1): `st_color` (text, packed 0xRRGGBB like swiby's themes),
`st_bg`, `st_font_size`, `st_bold`, `st_radius`.

Resolution per property, first match wins: `class.kind` → `class` → `kind` →
`container` (stacks only) → `root`.

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

## v1 boundaries (deliberate)

- One class per widget resolves (the first token); multi-class later.
- No id selectors (swiby has them; our widget handles could serve — later).
- `st_bold` is set-only (no un-bold on re-theme).
- No `font_family` — there is no widget-level family setter yet (vg canvas
  text has family support; widgets don't). The known gap.
- `style_opacity` is not in the sheet (its CSS route is a win32 no-op).
- Styles apply at `apply_styles` time, not at widget construction — a
  "current sheet consulted by constructors" layer can come later without
  changing the model.
