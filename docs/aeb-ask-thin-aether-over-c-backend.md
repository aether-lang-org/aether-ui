# aeb ask: a manual-path mode for "thin Aether over a C backend"

**Filed by:** aether-ui, while spiking `bootstrap.sh` + `.build.ae` to build
through `aeb` instead of the hand-rolled `build.sh`.

## Motivation

aether-ui is a GUI toolkit whose Aether surface (`aether_ui.ae`, and the AeVG
`vg`/`vg_live`/`grammar_*` modules) is a **thin Aether wrapper over a large C
backend** (`aether_ui_gtk4.c` / `aether_ui_macos.m` / `aether_ui_win32.c` +
helpers). The Aether modules declare the backend functions as `extern` and call
them; the bodies live in the C files, linked alongside.

`build.sh` builds an app the obvious way:

```
aetherc app.ae app.c            # plain — NOT --emit=lib
gcc app.c aether_ui_gtk4.c aether_ui_system_extras.c aether_ui_sni.c \
    $(pkg-config --cflags --libs gtk4) -laether $(ae cflags --libs) -o app
```

Plain `aetherc app.ae app.c` **tolerates extern-bodied imports** — the externs
resolve at the gcc link step against the C backend. This works today.

## What blocks aeb

`aether.program(b)`'s manual path (entered via `extra_source`/`link_flag`)
runs a **transitive import-closure regen**: it walks the entry's `import` graph
and `aetherc --emit=lib`s every project-local imported `.ae`
(`lib/aether/module.ae`, the BFS around line ~1206 feeding the regen list).

For aether-ui that means `aether_ui.ae` and `vg_live.ae` get `--emit=lib`'d
standalone — and they **can't be**: their function bodies are C externs, so
`--emit=lib` fails with a wall of `E0301 Undefined function
'aether_ui.canvas_*'` (the externs are only defined when linked with the C
backend, which `--emit=lib` doesn't do).

Reproduction (in this repo): `aevg/.analog_clock.build.ae` declares the GTK4
backend C files via `extra_source` and links the GTK/libaether flags via
`link_flag`. `aeb aevg/.analog_clock.build.ae` →
`aetherc --emit=lib failed for .../vg_live.ae`, 32 × E0301.

The import-closure walk is correct and useful for its **cache-key** use
(`_cache_key_for_aether_link`) — a change to an imported module should bust the
key. The problem is only the **regen-feeding** use: it assumes every imported
project `.ae` is a self-contained `--emit=lib` library, which isn't true for the
Aether-thin-over-C shape.

## What we'd want

A way to tell `aether.program(b)`: **compile the entry with plain `aetherc
entry.ae entry.c` and link the declared `extra_source` C + `link_flag`s — do NOT
`--emit=lib` the import closure.** The closure still feeds the cache key
(hashing imported `.ae`s for staleness); it just doesn't get regen'd.

Sketch (one of):
- a setter, e.g. `no_closure_regen()` or `link_only_entry()`, that suppresses
  the regen-from-closure pass while keeping plain entry compile + the cache-key
  hashing; or
- skip a closure module from regen when its symbols are provided by a declared
  `extra_source` (harder to detect reliably — the symbol↔C-file mapping isn't
  declared); or
- a target-level "the imports are extern-backed, build like `ae build` would for
  a plain program, but with my extra_source/link_flag" mode.

The first (an explicit opt-out setter) is the smallest and most predictable, and
matches aeb's "the `.build.ae` declares intent" ethos.

## What we're NOT asking

- Not asking aeb to parse `pkg-config` / detect GTK — embedding
  `$(pkg-config ...)` in a `link_flag` already works (aeb's own `lib/aether`
  does the same for zlib/openssl), expanded by the shell at gcc time.
- Not asking for cross-compilation (Mac/Win from Linux) — that's a separate aeb
  roadmap item; aether-ui builds the host backend.

## Acceptance

`aeb aevg/.analog_clock.build.ae` produces a runnable `analog_clock` binary
linking the GTK4 backend, with no `--emit=lib` attempted on `aether_ui.ae` /
`vg_live.ae` — equivalent to what `build.sh` does, expressed in the `.build.ae`.

## Status in aether-ui

`bootstrap.sh` (toolchain bootstrap, mirrors servirtium-vcr) is in place and
works. `aevg/.analog_clock.build.ae` is the reproduction. Until aeb gains the
opt-out, `build.sh` + `ci.sh` remain the build; the `.build.ae` is the
forward-looking spike. Tracked in `aevg/TODO.md`.
