# Plan: glitch-free live content inside an AeVG scene (the TaOS bar, in-scene)

## Context

The north star is Chris Hinsley's 1993 TaOS demo — a tumbling cube in one window
dragged over a tumbling cube in another, **zero repaint glitch, all on CPU**, a
property Windows/Mac took decades to match and that ChrysaLisp reproduces today
via a region-accurate, two-pass, backbuffered compositor (see
`ChrysaLisp_AI_made_apps_experiment/docs/ai_digest/gui_composition.md`). We want
the equivalent for aether-ui: **video and games living in the middle of composed
AeVG windows, glitch-free.**

Decision (this plan): meet the bar **within a single AeVG canvas** — multiple
independently-animating sub-regions (a video, a tumbling cube, widgets)
composited glitch-free into one vg{} scene. The outer window composition stays
GTK4's job (it gets it right today; fighting it isn't our achievement). And the
live content is an **abstracted live region**: a video decoder, a game loop, or
a generated animation all feed the same region the same way.

## Key insight (de-risks the whole thing)

Within one AeVG canvas, glitch-free is **already guaranteed** by the existing
pipeline and needs no region algebra:

- The animation loop does `canvas_clear → clip → vg_flush(all shapes) →
  canvas_redraw`.
- `canvas_redraw` → `gtk_widget_queue_draw` → `canvas_draw_func` →
  `canvas_replay(whole command buffer)` onto **one backbuffer**, which GTK
  presents **atomically** (GtkDrawingArea double-buffers).
- So the entire frame — every sub-region's current content — is rebuilt and
  shown in one present. No partial paint, no tearing, no stale strip. Two live
  cubes in one scene, redrawn each frame, are already glitch-free.

ChrysaLisp's region algebra + two-pass occlusion is a **performance
optimization** (redraw only the dirty sub-region, skip occluded areas), NOT the
correctness mechanism. We get correctness free from atomic full-frame present;
we add region-dirty tracking later only if per-frame full reflush proves too
slow.

The blit sink already exists: `aether_ui_canvas_draw_image_impl(cid,x,y,w,h,
rgba,len)` copies the RGBA (per-frame-safe) → `CANVAS_DRAW_IMAGE` → premultiplied
cairo blit. The animation loop (`vg.animate` + 16ms timer) already drives
per-frame redraw.

## Approach — the `live_region` abstraction

A live region is a rectangular area of a vg scene whose pixels are produced
**per frame** by a source. One abstraction, two source kinds:

1. **Raster source** — the source owns an RGBA buffer (W×H×4). Each frame the
   region blits the current buffer at its rect via `canvas_draw_image`. A video
   decoder pushes a decoded frame into the buffer; a CPU game renders into it.
2. **Draw source** — a per-frame redraw callback `fn(region, t)` that emits vg
   shapes (a tumbling cube as projected polygons). No buffer; draws directly
   into the scene's command buffer each frame.

Both are registered on the scene and driven by the existing animation timer; the
loop already does atomic full-frame present, so adding a live region is "register
a per-frame producer + blit/draw it during flush."

### Layers

- **`aevg/live_region.ae` (new, pure)** — the abstraction:
  - `live_raster(scene, x, y, w, h) -> *LiveRegion` — a region backed by an
    RGBA buffer the caller writes into (`live_region_buffer(lr)` →
    ptr+len, or `live_region_push(lr, rgba, len)`).
  - `live_draw(scene, x, y, w, h, redraw_fn)` — a region drawn by a callback
    each frame.
  - Registered into the scene (new `live_regions` list on VgScene, like
    pending/anim_mgr). Marked always-animating so the timer ticks.
- **vg flush integration** — after the normal shape flush, walk live regions:
  raster → `canvas_draw_image(cid, rect, buf)`; draw → invoke redraw_fn (emits
  shapes). They compose into the same backbuffer → atomic present.
- **vg DSL surface** — `vg.video_region(x,y,w,h, frame_source)` and
  `vg.canvas_region(x,y,w,h) callback |region, t| { ...vg shapes... }`. The
  draw callback is **capture-free** (receives `region`, like the animation tick
  fix — block-locals dangle).
- **A frame source seam** — `frame_source` is an abstraction with `next_frame(buf)
  -> int` (1 = new frame ready). First impls: a **generated** source (procedural
  RGBA, e.g. a moving gradient — needs nothing external) and a **file/pipe**
  source stub (decode wired later; the abstraction is what matters now).

### The demo (the bar, proven)

`example_aevg_live.ae`: one window, one vg scene containing
- a **tumbling cube** draw-region (CPU 3D: 8 verts, rotate by `t`, project,
  draw 12 edges as `vg.line`s) — the literal TaOS cube;
- a **live raster region** (a generated animated gradient / plasma) overlapping
  the cube;
- a static widget border around both.
All redraw each frame into one backbuffer → atomic present. The cube spinning
*over* the moving raster, with the raster's changing pixels showing correctly
around the cube's edges, is the in-scene TaOS property.

## Verification

- **Headless unit (`test_live_region.ae`, Phase 0):** register a raster region +
  a draw region on a deferred scene with the recording backend; advance the
  clock; assert (a) the raster blit is dispatched at the right rect each tick,
  (b) the draw callback fired with the right `t`, (c) `scene_is_animating` stays
  true while a live region exists. (Pixel correctness of the atomic present is a
  GTK property we trust + screenshot-spot-check, not unit-assert.)
- **Driver screenshot caveat:** the hot 16ms timer starves `/screenshot` (known
  limitation from the animation work). So the live demo is verified by: builds +
  runs + no-crash liveness, plus a **headless PNG** render of one frame
  (`canvas_write_png`) showing cube+raster composited — the offscreen path
  doesn't need the live timer.
- **Glitch-free argument:** rests on the atomic-full-frame-present property
  (documented above), demonstrated by the single-frame PNG being a correct
  composite; there is no partial-paint path to glitch.

## Risks / notes

- **Per-frame full reflush cost.** Redrawing every shape + blitting every region
  each frame is O(scene) per frame. Fine for the demo; if it bottlenecks, add
  ChrysaLisp-style region-dirty tracking (only reflush changed regions) — a
  follow-on, not this plan. Flagged, not built.
- **Capture-free callbacks.** The draw-region callback must receive `region`
  (not capture block-locals) — same dangle trap the animation tick hit.
- **Buffer ownership.** `draw_image_impl` copies the RGBA each call, so the
  region can reuse one buffer across frames safely.
- **Not the literal TaOS bar.** This is in-scene composition; AeVG does NOT own
  inter-window composite (GTK4 does). The full system-compositor version (region
  class + two-pass + per-view backbuffers + host blit ABI) is a separate, much
  larger effort — explicitly out of scope here, captured for later.
- Baby commits; tests with each; no push.

## Suggested commit sequence

1. `live_region.ae` + scene plumbing (list on VgScene, flush integration,
   always-animating) + `test_live_region.ae`.
2. vg DSL surface (`video_region` / `canvas_region`) + a generated raster
   frame-source + headless PNG composite test.
3. `example_aevg_live.ae` — the tumbling cube over the live raster; smoke-built;
   one-frame PNG verified.
4. (Later, optional) region-dirty optimization; real video decode source.
