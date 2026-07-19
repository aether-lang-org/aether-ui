# zen_demo themes — real-world palettes as .aecs files

- `solarized-{light,dark}.aecs` — Ethan Schoonover's Solarized (MIT); designed
  as a light/dark PAIR, wired through `use_styles_pair` (the mode-flip demo).
- `dracula.aecs` — draculatheme.com (MIT).
- `nord.aecs` — Arctic Ice Studio / Sven Greb (MIT).
- `water-{light,dark}.aecs` — Kognise's water.css (MIT), the classless
  framework whose element-type-only selector model matches AeCS 1:1.

All hex values are the palettes' published ones, so specs assert them
verbatim. The demo's own "Midnight"/"Paper"/"Terminal" sheets follow the
CSS Zen Garden concept (Dave Shea): one fixed tree, many skins — those
sheets are ours; no Zen Garden design assets are copied.

## water.css port table (honest vocabulary check)

| water.css property | AeCS |
|---|---|
| background-body / text-main / links / button colors | ✓ bg / color |
| border-radius on buttons | ✓ radius |
| font stack (system sans) | ✓ family (coarse) |
| borders, per-element padding/margins | ✗ (insets is per-widget, not boxed) |
| :hover / :focus states, transitions on them | ✗ (no pseudo-states — by design) |
| box-shadow on focus | ✗ |
