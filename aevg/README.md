# AeVG — Aether Vector Graphics

**AeVG (Aether Vector Graphics)** is a pure-Aether port of Tsyne's CVG
(Cosyne Vector Graphics, TypeScript). The driving inventory and port
plan live with the source:
`~/scm/tsyne/tsyne/cosyne/src/cvg/PORT_INVENTORY.md`.

These modules compile to C, depend on nothing platform-specific
(no GTK/AppKit/Win32 coupling at this layer), build and run headless,
and are wired into `ci.sh` as **Phase 0** (runs even with no display).
The platform-coupling layer lives behind `aevg_backend.ae`'s interface
— see "Backend" below.

## Status

| Module | Source (TS) | Aether | Test | State |
|--------|-------------|--------|------|-------|
| transform | `transform.ts` (346 LoC) | `transform.ae` | `test_transform.ae` (30 asserts) | ✅ matrix algebra + factories + projective. `parseTransform` (string→matrix) deferred — unblocked by `std.regex` v0.191; lands alongside `parser.ae`. |
| normalizer | `normalizer.ts` (427 LoC) | `normalizer.ae` | `test_normalizer.ae` (60 asserts) | ✅ `parse_path` (regex-based tokenizer) + `normalize_commands` (rel→abs, H/V→L, S/Q/T→C, arc→cubic via SVG F.6). `serialize_commands` deferred until `string.from_float` has caller-controlled precision (the librsvg parity gate wants fixed-point output). |
| easing | `grammar-types.ts` (subset, ~80 LoC) | `easing.ae` | `test_easing.ae` (50 asserts) | ✅ 7 easing fns (linear, in/out/inOut quad + cubic), `lerp`, `lerp_color`, `parse_hex_color` (with 3-char shorthand expansion). Uses `to_int_radix` + `from_int_radix` + `pad_start` (the latter two landed `[current]`, closing `aether/cvg_asked_for.md`). Feature-complete vs. the TS source. |
| parser | `parser.ts` (245 LoC) | `parser.ae` | `test_parser.ae` (39 asserts) | ✅ SVG → `SvgNode` tree. Index-walking tokenizer (mirrors TS source) + std.regex for header strip (`<?xml…?>`, multi-line `<!DOCTYPE…>`, comments). Hand-rolled attribute scanner avoids juggling regex captures across walks. Self-closing, nested, mixed-quote, text-content, namespaced/hyphenated attrs all covered. <!ENTITY> expansion deferred (~5% of real-world SVGs need it). |
| bbox | `bbox.ts` (281 LoC) | `bbox.ae` | `test_bbox.ae` (60 asserts) | ✅ AABB walker for circle/ellipse/rect/line/polyline/polygon/path/text + group recursion. Stroke-width margin from `style="stroke-width:…"` or attribute. CSS length units (cm/mm/in/pt/pc/px). Transform cascade via `parse_transform` (also landed this batch). SKIP_TAGS (defs, filter, …) honoured. |
| **— Tier B —** | | | | |
| blur | `blur.ts` (101 LoC) | `blur.ae` | `test_blur.ae` (25 asserts, property-based) | ✅ Two-pass separable Gaussian on RGBA `std.bytes` buffers. First downstream consumer of v0.192's `bytes.copy_from_bytes` (σ=0 fall-through). Uses libm `lrint` extern for fast float→int (no `as int` cast available). |
| rasterize | `rasterize.ts` (307 LoC) | `rasterize.ae` | `test_rasterize.ae` (33 asserts) | ✅ Software rasterizer. `parse_color_to_rgba` handles hex (3/6/8-char), `rgb()`/`rgba()`, and a 40-entry named-color table. `fill_rect`, `fill_circle` (distance test), and `fill_path` with full scanline rendering: tokenize → cubic Bezier flattening (recursive subdivision, 0.5px tolerance) → edge collection → per-scanline x-crossings → insertion-sort → nonzero/evenodd span fill. `apply_clip_mask` for alpha multiplication. |
| grammar_utils | `grammar-utils.ts` (305 LoC, ~50% subset) | `grammar_utils.ae` | `test_grammar_utils.ae` (49 asserts) | ✅ Tier-B subset: style-attr / length / font-size / dy-em / filter-region parsing, url(#id) extraction, preserveAspectRatio, points→path, path bounds, color-with-opacity emit (`rgba(R,G,B,α)`), base64 byte-encode (via `std.cryptography`). Tier-C resolve_* + transform_path_to_buffer deferred (need AevgContextLike). `normalize_color` deferred (needs `from_int_radix` + `pad_start` from `aether/cvg_asked_for.md`). |
| **— Tier C —** | | | | |
| grammar_context | `grammar-context.ts` (583 LoC, core subset) | `grammar_context.ae` | `test_grammar_context.ae` (40 asserts) | ✅ Core state holder. `ViewBoxMapping` + `AevgContext` structs (opaque ptr handles). Five registries (gradient/filter/clipPath/node/cssRule) via `register_*`/`get_*`/`has_*`. Three stacks (style/transform/when) with push/pop/top + safe over-pop. Initial transform-stack seeded with identity (matches TS). Animations / event tracking / bindings / polling — separate commits. |
| grammar_element | `grammar-element.ts` (525 LoC, core subset) | `grammar_element.ae` | `test_grammar_element.ae` (43 asserts) | ✅ Per-shape wrapper. `AevgElement` struct (~17 fields), setter/getter pairs for all event handlers (click/hover/drag/dragEnd/scroll/dblclick/rclick), reactive bindings (fill/stroke/opacity/text/pos), tooltip/cursor/when/visibility/destroyed flags. Pure hit-test: bounds inside-check + invisible/destroyed guards. Backend reach-through (text/fill/stroke/opacity) and animation `transition()` deferred until the aether-ui widget surface is wired. Callback setters take `ptr` (v0.193+1 `fn ↔ ptr` fix doesn't reach struct-field assignment — filed as a follow-up to `aether/fn_ptr_coercion.md`); call sites still pass bare function names cleanly. |
| grammar_rendering | `grammar-rendering.ts` (440 LoC, coordinate-mapping subset) | `grammar_rendering.ae` | `test_grammar_rendering.ae` (30 asserts) | ✅ Transform-stack push/pop with `parse_transform` composition; `map_point` (currentTransform ∘ viewBox); `map_x`/`map_y`/`map_length`; `parse_len_x`/`parse_len_y` resolving `"%"` against viewBox dimensions; `map_stroke_width` with sub-pixel opacity-factor fallback (raw=0.4 × scale=2 = 0.8 → returns (1.0, 0.8) to render at 1px with 80% alpha). Style cascade is `grammar_style.ae`; CSS class system, refresh() loop, gradient construction — separate follow-ups. |
| grammar_style | `grammar-rendering.ts` (style-cascade subset) | `grammar_style.ae` | `test_grammar_style.ae` (19 asserts) | ✅ SVG-style cascade: `push_style` / `pop_style` / `current_style` / `resolve_style`. SvgStyle modelled as `std.map string→string` (sidesteps Aether's no-nullable-struct-fields constraint; matches what TS's `SvgStyle` interface does at runtime via `?? `chains). Four-way priority: element attrs → inline style → CSS class → inherited parent. 16 properties enumerated. CSS-class lookup delegates to `grammar_css.get_css_props`. |
| grammar_css | `grammar-rendering.ts` (CSS subset) | `grammar_css.ae` | `test_grammar_css.ae` (22 asserts) | ✅ `register_css_style(ctx, css_text)` parses CSS rule blocks (`selector { prop: val; … }`) with multi-selector (`a, b { … }`) and accumulating semantics; `get_css_props(ctx, tag, class_attr)` returns merged tag+class properties (later classes overwrite earlier; class beats tag). Stores in `ctx.css_rules` (existing grammar_context registry). End-to-end via loader's `<style>` handler with CDATA support. No specificity computation, no `:hover`, no nesting — SVG inline CSS is shallow by convention and this matches the TS source. |
| aevg_backend | (new) | `aevg_backend.ae` | tested via `test_grammar_shapes` | ✅ Recording backend stub for Phase-0 testing. Seven entry points (`canvas_circle`, `canvas_rectangle`, `canvas_line`, `canvas_path`, `canvas_text`, `canvas_raster`, `canvas_tappable_raster`); each records the call kind + opts (as `std.map`) and returns a fresh handle. Tests inspect via `backend_kind(b, i)` and `backend_opt_get(b, i, key)`. Real backend wiring (to `aether_ui.canvas_*`) lives behind this same interface; the swap is one file. |
| grammar_shapes | `grammar-shapes.ts` + `<text>` from `grammar-defs.ts` | `grammar_shapes.ae` | `test_grammar_shapes.ae` (34 asserts) + `test_grammar_text.ae` (29 asserts) | ✅ Shape factories: `shape_circle`, `shape_rectangle`, `shape_line`, `shape_path`, `shape_group`, **`shape_text`**. Text resolves font-size + text-anchor + bold (case or numeric ≥600) + italic/oblique + monospace (font-family substring), applies baseline correction (`textSize × 1.07`), supports `<tspan>` children with x reset / dx / dy em-units. Bounds estimate uses 0.6×text_size char width heuristic for hit-testing. End-to-end pipeline per shape: maybe-push-transform → resolve_style → map_point + parse_len_x/y → resolve_fill/stroke_color → map_stroke_width → backend dispatch → wrap in *AevgElement with bounds → maybe-pop-transform. Group is unique (no backend call) — pushes style+transform, runs the body closure, pops. Projective-transform, raster-fallback, gradient, rounded-corner branches all deferred. |
| grammar_utils | (additions this commit) | `grammar_utils.ae` | (existing test still 49) | ✅ Adds `resolve_fill_color`, `resolve_stroke_color`, `effective_alpha`, `effective_stroke_alpha`, `style_num_default`. `url(#id)` gradient lookup deferred (needs grammar_defs); falls back to trailing color or "black". |
| grammar_factories | `grammar-factories.ts` (223 LoC, kernel subset) | `grammar_factories.ae` | `test_grammar_factories.ae` (31 asserts) | ✅ `create_aevg_context(opts)` — the meat: parses viewBox, computes the two-step `viewBox → viewport → canvas` affine via `preserveAspectRatio` (xMidYMid meet default; Min/Max alignment; meet/slice scaling), constructs a `*AevgContext` ready for shape calls. `aevg(backend, opts, body)` convenience wraps it. `AevgOptions` struct with width/height/viewBox/PAR setters. Letterbox/pillarbox math verified end-to-end: vb 100×50 + canvas 200×200 + meet → scale 2, offset_y 50. The TS `CvgBuilder` class doesn't port (no classes); callers use `shape_*` factories directly. `app.clip`/`app.canvasStack` widget machinery is real-backend territory (Phase 1). |
| grammar_animations | `grammar-context.ts` (animation sub-system) | `grammar_animations.ae` | `test_grammar_animations.ae` (19 asserts) | ✅ Animation manager separated from grammar_context. `add_animation(mgr, el, dur, delay, loop, yoyo, now) callback \|t: float\| { … }` registers; `tick_animations(mgr, now_ms)` drives one frame. Synthetic-clock-driven tests (no sleep). Closure receives **raw t** in [0,1] and applies its own easing — workaround for an Aether codegen bug where invoking a `fn`-typed param that returns float through closure-unbox emits an `int(*)` cast that mangles the return. Filed as follow-up to `aether/fn_ptr_coercion.md`. Real-frame tick via `os.now_monotonic_ms()` + backend timer is Phase 1. |
| grammar_events | `grammar-context.ts` (event dispatch sub-system) | `grammar_events.ae` | `test_grammar_events.ae` (37 asserts) | ✅ Event dispatch: `dispatch_click/hover/drag/drag_end/scroll/double_click/right_click`. Topmost-hit lookup walks `tracked_elements` in reverse; first element with both a matching handler AND `hit_test(x,y) == 1` wins. Hover keeps state — enter/leave transitions, no duplicate fires within same element. Drag is **sticky**: first dispatch captures the target, subsequent dispatches stay on it (canonical drag-and-drop UX) until `dispatch_drag_end` releases. All handlers void-returning (the fn-return-float codegen bug doesn't bite). Tooltips and OS-level cursor changes deferred (need real backend integration). |
| path_builder | `grammar-defs.ts` (PathBuilder class) | `path_builder.ae` | `test_path_builder.ae` (15 asserts) | ✅ Fluent imperative path construction. `pb_new(ctx, backend)` → `pb_move_to`/`pb_line_to`/`pb_cubic_to`/`pb_quadratic_to`/`pb_arc`/`pb_close` accumulate parts; `pb_fill(color)` or `pb_stroke(color, width)` dispatches through `grammar_shapes.shape_path` with the accumulated `d`. quadratic_to/arc route through `normalizer.normalize_commands` then re-serialise, dropping the leading `M` (TS source's trick). The TS class chained via `return this`; without classes in Aether, callers pass the `*PathBuilder` to each step explicitly. Required the heap-string-struct-field `string_retain` idiom — same one I documented in `cvg/README.md`. |
| render_as_raster | `grammar-defs.ts` (renderAsRaster) | `render_as_raster.ae` | `test_render_as_raster.ae` (20 asserts) | ✅ The Tier-B→Tier-C bridge. Pre-renders a shape into an RGBA buffer with optional Gaussian blur (filter) and clipPath alpha-mask, then dispatches as `canvas_raster`. Pipeline: compute expanded buffer dims in pixel space → allocate `std.bytes` buffer → invoke a `shape_filler: fn` closure (passed as `callback { … }` taking 9 args including buf, dims, offsets, RGBA) → `blur.gaussian_blur` → walk clipPath shapes into a mask via `rasterize.fill_rect_in_buffer` / `fill_circle_in_buffer` → `rasterize.apply_clip_mask` → `grammar_utils.base64_encode_bytes` → `aevg_backend.canvas_raster`. Crop-to-filter-region deferred (TS source comment confirms it's an optimisation, not a correctness requirement). 9-arg closure invocation works via `callback { … }` (env-aware wrapper); bare-named-function-as-fn-arg hits a separate codegen bug — filed as `aether/fn_bare_name_env_shift.md`. |
| grammar_bind | `grammar-context.ts` (bindTo + refreshBindingRegions) + `grammar-element.ts` (BindingRegion shape) | `grammar_bind.ae` | `test_grammar_bind.ae` (20 asserts) | ✅ Data-driven element regions. `new_region()` then four trailing-callback setters (`region_set_items` / `region_set_render` / `region_set_trackby` / `region_set_update`), then `region_init(ctx, region)`. One trailing-closure per setter — can't take all four in a single call because trailing-callback syntax is single-closure. `refresh_all(ctx)` walks all regions and per-region two-passes: (1) remove keys not in the new items() snapshot; (2) for each new key, call update() if it survived from `current`, else render() and insert. Identity-preserving across refreshes: items keyed by `trackby` retain their existing `*AevgElement` ptr unless removed. Element destruction hook stubbed (calls drop entry from map; real `element_destroy` lands when AevgElement.destroy ports). The fix in Aether `[current]` to closure-call return-type cast (`aether/fn_return_float_cast.md` layer-1) is what makes this safe: `trackby -> string` and `items -> *List` (ptr) returns now go through the right-typed cast and stop being segfault hazards. |
| grammar_reactive | `grammar-rendering.ts` (proto.refresh per-element loop, ~50 LoC) | `grammar_reactive.ae` | `test_grammar_reactive.ae` (23 asserts) | ✅ Per-element reactive property bindings: `.bindFill -> string`, `.bindStroke -> ptr` (map of `{color, width}`), `.bindOpacity -> float`, `.bindText -> string`, `.bindPos -> ptr` (map of property→value), `.when -> int` (predicate). `refresh_element_bindings(el)` evaluates `when()` first; if it returns 0 the element is hidden via `element_set_visible(0)` and bindings are skipped (matches TS contract); otherwise each non-null binding closure fires. `refresh_all_elements(ctx)` walks `ctx.tracked_elements` skipping destroyed elements. Each closure runs through a two-layer thunk (outer-ptr → inner-fn) so the closure-call trampoline's return-type cast resolves through the inner's enclosing-fn return type. Float-opacity validated end-to-end: returns 0.75 via `callback { … }` env-aware wrapper (the layer-2 bare-fn-float case still pending, but the AeVG convention of always wrapping handlers in `callback { … }` keeps it safe). Stroke/pos map shape mirrors TS's `Record<string, …>` returns. Caching the binding results onto the element (for Phase-1 backend redispatch) is deferred. |
| grammar_defs | `grammar-defs.ts` (591 LoC, gradient + filter + clipPath subset) | `grammar_defs.ae` | `test_grammar_defs.ae` (69 asserts) | ✅ **Gradients**: `<linearGradient>` / `<radialGradient>` parsing + registration. Stop parsing with `stop-color` / `stop-opacity` / `offset` (incl. `%` form); `gradientTransform` applied to coords; `units`/`spreadMethod` pass-through. Linear default coords (0,0)→(1,0); radial default (0.5, 0.5, 0.5) with `fx/fy` defaulting to `cx/cy`. `xlink:href` inheritance deferred. ✅ **Filters**: `<filter>` with optional `<feGaussianBlur>` child. Region attrs (x/y/width/height) with SVG-spec defaults (-10%, -10%, 120%, 120%). `stdDeviation='5 2'` two-value form supported. ✅ **ClipPaths**: `<clipPath>` with `<rect>` / `<circle>` child shapes. Polygons and other child types silently skipped (matches TS). All three register into `context.gradients` / `.filters` / `.clip_paths` via grammar_context's existing `register_*` API. Text / use / pathBuilder / renderAsRaster still deferred. |
| **— Tier D —** | | | | |
| loader | `loader.ts` (178 LoC) | `loader.ae` | `test_loader.ae` (51 asserts) | ✅ End-to-end SVG → backend dispatch. Two-pass: first `index_nodes` builds the id→node registry, then walks the tree dispatching shapes. Handles `g/path/circle/rect/line/polyline/polygon/text/style` plus `<linearGradient>` / `<radialGradient>` / `<filter>` / `<clipPath>` registration via `grammar_defs` plus `<use href="#id" x y />` plus `<style>` (CSS via `grammar_css`, CDATA-aware). `<defs>` walks only definition-style children. Silently skips `ellipse` (still pending). Polyline/polygon → emit as `<path>`. |

Also landed: **`parse_transform`** (deferred since the first commit; ~22
extra assertions in `test_transform.ae`, total 52) + cross-module
ptr-accepting wrappers `affine_mul_p` / `affine_apply_p` /
`affine_is_identity_p` (struct-types-don't-cross-modules idiom forces
this pattern for any Tier-A module that wants to call another).

`types.ts` is **not ported** — it's a TS-only idiom (12 type-only
`interface` declarations). Aether struct types don't cross module
boundaries, so a shared `types.ae` would be dead weight; each
consumer declares the structs it uses locally and exposes accessor
functions for cross-module reads.

**Tiers A and B complete; Tier C deeply progressed; Tier D loader
done.** The rendering core is operational, the utility helpers are
in place, the context state holder + registries exist, data-driven
binding regions diff and update, and per-element reactive bindings
re-evaluate on refresh. **Total: 24 modules, 874 assertions, all
passing in Phase 0** (pure-Aether, no GTK/display dependency).

Tier C breakdown (per inventory):
  - ✅ `grammar_context.ae` (core + registries)
  - ✅ `grammar_element.ae` (per-shape wrapper, hit-test, bindings)
  - ✅ `grammar_rendering.ae` (coordinate-mapping subset)
  - ✅ `grammar_style.ae` (style cascade)
  - ✅ `aevg_backend.ae` (recording stub; real backend wiring deferred)
  - ✅ `grammar_shapes.ae` (circle/rect/line/path/group factories)
  - ✅ `grammar_factories.ae` (`create_aevg_context`, viewBox → canvas
    mapping with preserveAspectRatio)
  - ✅ `grammar_animations.ae` (animation manager + tick loop)
  - ✅ `grammar_css.ae` (CSS class system: registerCssStyle/getCssProps)
  - ✅ `grammar_events.ae` (event dispatch: hover/click/drag/scroll)
  - ✅ `path_builder.ae` (fluent imperative path construction)
  - ✅ Binding regions (`grammar_bind.ae`) — unblocked once Aether
    `[current]` landed the layer-1 fix to the closure-call
    return-type cast (`aether/fn_return_float_cast.md` PARTIALLY
    RESOLVED). `trackby -> string` and `items -> *List` (ptr)
    returns now go through the right-typed cast at the trampoline.
    Float-bare-fn shape still pending the layer-2 fix; AeVG dodges
    by always wrapping handlers as `callback { … }`.
  - ✅ Reactive bindings (`grammar_reactive.ae`) —
    `.bindFill / .bindStroke / .bindOpacity / .bindText / .bindPos`
    re-evaluated on `refresh_all_elements(ctx)`. `when()`
    predicate gates the binding evaluation per element.
  - ✅ `grammar_defs.ae` (gradient subset; filter/clipPath/text/use deferred)
  - ⬜ `grammar-defs` (gradient/filter/clipPath/text/use construction)

**End-to-end now works from a raw SVG string**: `loader.load_svg(backend,
"<svg viewBox='0 0 100 100'><circle cx='50' cy='50' r='25' fill='red'/></svg>",
100, 100)` parses, walks, dispatches — and the recording backend confirms
a single `canvas_circle` call with `x=25, y=25, x2=75, y2=75, fillColor='red'`.
The whole port runs as a system. See `test_loader.ae` for nested groups,
transforms, polyline/polygon, and graceful-degradation cases.

Then Tier D: `loader.ae`, `transpiler.ae`.

## Running a test by hand

```sh
aetherc aevg/test_transform.ae /tmp/t.c && gcc /tmp/t.c $(ae cflags) -o /tmp/t && /tmp/t
```

Or run the whole Phase 0 gate via `./ci.sh` (it's the first phase).

## Aether idioms this port leans on

Found empirically against `aetherc v0.181.0`; not all are in the language
reference. Capturing them here so the next module doesn't rediscover them.

- **No classes.** TS classes (`AffineMatrix`) → a `struct` plus free
  functions (`affine_mul(a, b)`), prefixed by the type (`affine_*`,
  `proj_*`).
- **Structs are heap pointer-structs across function boundaries.** A
  by-value `-> Struct {` return trips the parser. Allocate with `malloc(N)
  as *T` and thread `*T`. Sizes: count fields × 8 (every `float` is a
  C `double`; `48` for affine's 6, `64` for projective's 8).
- **No float tuple returns.** `return x, y` lowers to `int` and emits
  broken C. Return a small struct instead — `apply()` returns `*Pt`.
- **Module surface needs an `exports (...)` block**; otherwise `import`
  surfaces nothing and the parser derails at the import site.
- **`import` resolves sibling `.ae` files** relative to the compiled
  source's directory (`import transform` next to `test_transform.ae`).
- **`std.math` qualified names drop the `math_` prefix**: the raw extern
  is `math_cos`, but you call `math.cos`. Likewise `math.abs_float`,
  `math.sqrt`, `math.tan`. Constants are zero-arg function-calls:
  `math.pi()`, `math.tau()`, `math.deg_to_rad()`, `math.rad_to_deg()`.
  (Landed in CHANGELOG `[current]` as part of the CVG-port batch — same
  PR added `std.floatarr`, `bytes.copy_from_bytes`, `os.now_monotonic_ms`.)
- **No `sizeof`** — still hardcode struct sizes (`malloc(48)` for affine).
- **`std.floatarr` is the typed float array** (mirror of `std.intarr`).
  Go-style wrappers: `a, err = floatarr.new(n)`, `v, _ = floatarr.get(a, i)`;
  `floatarr.set(a, i, v)` returns just an error string. `_unchecked`
  variants are tuple-free for hot loops.
- **Struct types don't cross module boundaries.** `exports (...)` covers
  functions, not struct *types*. `import normalizer` makes `normalizer.M_CHAR`
  callable but `*normalizer.PathCommand` is rejected ("`normalizer` is not a
  struct type"). The fix is to expose accessor functions (`cmd_type(h)`,
  `cmd_args(h)`) and hand callers a raw `ptr` handle — same shape as
  `std.list` / `std.regex` / `std.floatarr`. See `normalizer.ae`'s
  `cmd_type` / `cmd_args` for the pattern.
- **No `float as int` cast.** Loop on a float counter that decrements
  by 1.0 instead (`math.ceil` produces whole-number floats — exact).
- **Char-code interpolation gotcha.** `${string_char_at("M", 0)}` returns
  empty when the literal `"M"` appears inside an `${...}` block — the
  escaped quotes don't tokenize cleanly. Assign to a variable first.
- **No `as ptr` cast.** Only `as *Struct` / `as fn(...)` / `as T[]` are
  accepted. Functions that take `ptr` params accept `*Struct` values
  directly (the C-side widens).
- **No `as string` cast either.** When you have a `ptr` that's actually
  an `AetherString*` (e.g. `map.get`'s value), let interpolation handle
  it (`${map_get_raw(m, "k")}` prints fine) or return `string` directly
  from your function — the codegen widens. Don't try to spell the cast.
- **`map.put(m, k, str)` auto-routes to `put_string_owned`** for heap
  string values (per #467 in std.map). Don't reach for `put_string` —
  it doesn't exist as a qualified wrapper. `map.get(m, k)` returns
  `(ptr, string)` — destructure and treat the ptr as a string at call
  sites.
- **`if int_fn(...)` doesn't auto-coerce.** `if string.starts_with(s, p)`
  fails with "If condition must be boolean". Use `== 1`. Same for
  `node_has_attr` and other 0/1 returns.
- **Discarding tuples and returns**: `_ = expr` emits a codegen warning;
  use a named-but-unused variable (`put_err = …; if string.length(put_err) > 0 { }`)
  if you genuinely want to drop. The warning is benign; the program runs.
- **No `int as float` cast either** (paired with the earlier "no `float
  as int`"). Aether implicitly promotes int→float in mixed arithmetic
  (`1.5 * some_int` works), so most call sites are fine without the cast.
- **`string.to_double` is strict.** Rejects trailing garbage — `to_double("1in")`
  returns `(0, "invalid double")`. TS `parseFloat("1in") → 1`. For SVG
  CSS-length strings, scan a numeric-char prefix manually then `to_double`
  on the prefix. See `bbox.parse_length_to_px`.
- **`match` is a reserved keyword** (actor-model hangover, per Aether LLM.md
  alongside `state` / `message`). Pattern-match in regex code names tokens
  `tok` not `match`. Compiler error is "Expected statement in block" — not
  obviously about the keyword.
- **Cross-module struct access via ptr-wrappers.** When module A's `*Affine`
  needs to be threaded through module B, B can't `as *transform.Affine`
  (qualified cast rejected). Pattern: A exposes `*_p(handle: ptr) -> *_p`
  variants that take the cast inside A. See `transform.affine_mul_p`,
  `affine_apply_p`, `affine_is_identity_p` — symmetric with the
  `normalizer.cmd_type` / `cmd_args` shape.
- **Float → int via libm `lrint`.** Aether has no `float as int` cast
  and no `math.to_int(f)`. For hot inner loops (the rasterizer's
  per-pixel rounding, the blur kernel's centre-index derivation),
  declare `extern lrint(x: float) -> long` and call directly. Returns
  long; int-typed callees auto-coerce. Banker's rounding to even at
  .5 — matches IEEE-754 default. The counter-loop float→int hack
  (`while remaining > 0.5 { count += 1; remaining -= 1.0 }`) works
  for small bounded counts (kernel size derivation: ~10 iterations
  once per blur) but is O(n) so DO NOT use inside per-pixel loops.
- **Heap string in a struct field across a function return needs
  explicit `string_retain`.** Pure-local strings get auto-released
  on function exit via Aether's per-scope `_heap_<field>` tracking.
  When you build a struct in function `make_x()` whose string fields
  come from `string.copy` / `regex.capture` / similar heap producers,
  the locals are auto-released as the function exits — leaving the
  struct fields dangling. Symptom: garbled bytes when reading the
  field at the caller. Fix: `string_retain(local)` before storing into
  the malloc'd struct, OR before the function returns. See
  `grammar_utils.parse_preserve_aspect_ratio` for the pattern. (The
  struct-with-`string`-field auto-destructor described in the Aether
  LLM.md is for *stack* structs; malloc'd structs have no destructor,
  so retains are the caller's responsibility.)
- **`$(ae cflags)` emits all transitive deps** — including
  `-lpcre2-8`, `-lssl -lcrypto`, `-lz`, `-lnghttp2` for whichever
  of those `libaether.a` was built with. (Earlier in the port this
  was a downstream paper cut; resolved in CHANGELOG `[current]` via
  the `cmd_cflags` fix. Filed as `aether/regex-lib-fix.md`.)
- **`fn` and `ptr` coerce in both directions at coercion sites.**
  Storing a `fn` value in a `ptr`-typed slot (`list.add`, `map.put`,
  struct fields) boxes it as an `_AeClosure`. Reading back from a
  `ptr` slot and passing into a `fn`-typed param unboxes. Captured-
  state closures survive the round-trip — `bindFill` / animation
  tick / `whenStack` patterns can store any closure (with or without
  captures) and invoke later. Resolved in CHANGELOG `[current]` via
  Option B1 of `aether/fn_ptr_coercion.md`.
