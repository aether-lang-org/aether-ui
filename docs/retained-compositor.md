# The Retained Compositor — aether-ui's rendering north star

> **Status: aspirational / design note.** Not built. This records the high bar
> for aether-ui rendering, what we already have that seeds it, the gap, and a
> staged, independent path to it. Nothing here is implemented yet; the current
> backend is immediate-mode (see "Where we are today").

## Licensing boundary (read first)

The technique described here — **dirty-region tracking with opaque-region
subtraction and multi-pass composition** — is a decades-old, widely-documented
approach to incremental redraw. It long predates any one implementation:
Amiga's layers/damage lists, the X11 DAMAGE and region (`XRegion`) machinery,
classic Mac/Windows `InvalidateRect`/update-region models, and the academic
literature on rectangle CSG and occlusion culling all describe it. It is prior
art, freely usable.

**This document is a clean-room specification written from that general prior
art and from our own existing code. It is NOT derived from, and must NOT be
implemented by porting, any GPL-licensed source** — including ChrysaLisp,
whose GUI is a fine *example* of the technique but whose code, data
structures, naming, and algorithms we will not read, transcribe, or adapt.
aether-ui is MIT-licensed; the compositor we build must be independently
authored from first principles and the public prior art. If you implement
this, do not open a GPL codebase for reference — work from this doc, the X11
region API shape, and cairo's clipping primitives.

## The bar

The target is the classic CPU compositor miracle: **overlapping, independently
animating regions** — think two windows each rendering a tumbling cube, one
dragged over the other — repainting with **no flicker, no glitch, and cost
proportional only to the pixels that actually changed and are actually
visible.** Drag the front window and only the newly-*exposed* slivers of the
back one repaint; the area still covered by the front window is never touched.
A live video or game rectangle in the middle of a vector scene repaints only
its own bounds at its own frame rate, while the static chrome around it costs
nothing per frame.

This was achieved on CPUs before GPUs existed. It is not about raw blit speed;
it is about **never drawing a pixel you don't have to.**

## The technique (from general prior art)

Three ideas, composed:

1. **A retained view/element tree.** UI/scene elements form a tree; each has a
   bounding rectangle in its parent's coordinates. (Contrast immediate mode,
   where you replay a flat command list every frame.)

2. **A region as a set of non-overlapping rectangles.** Not a single bounding
   box — a true 2D area built by rectangle CSG: union (add a rect, splitting/
   merging to keep them non-overlapping), subtraction (remove a rect, splitting
   survivors), intersection/clip, translate between coordinate spaces. This is
   the same shape as X11's `XRegion`/`pixman_region`. Each element carries:
   - a **dirty region** — the part of it that changed and needs redraw, and
   - an **opaque region** — the part of it that is fully opaque (nothing behind
     it shows through there).

3. **Multi-pass composition that subtracts occlusion.** The essential move:
   - Propagate dirty areas **up** the tree (a changed child dirties its
     ancestors over its bounds).
   - **Subtract** each opaque element's region from the dirty regions of
     everything behind it (ancestors and earlier-drawn siblings) — so covered
     pixels are removed from the work list entirely.
   - Distribute the surviving dirty region **down**, clipped to each element's
     bounds, producing per-element "what I must actually redraw" rectangles.
   - Draw each element **clipped to those exact rectangles** into an off-screen
     backbuffer, then flush the backbuffer's changed bounds to the screen once.

The occlusion subtraction is the whole game: it is why a dragged front window
costs only the exposed back-window slivers, and why a static scene around a
live rect costs zero per frame. Overdraw is eliminated, not merely buffered.

## Where we are today (immediate mode)

aether-ui's canvas backend is **immediate-mode**: a flat command buffer
(`begin_path`/`move_to`/`line_to`/`stroke`/`fill_rect`/`arc`/`fill`/
`fill_text`/`draw_image`/gradients) replayed **in full** on every paint, via
`canvas_replay()` over cairo (GTK4), CoreGraphics (macOS), GDI (Win32). AeVG
shape factories emit into this buffer through the `backend_dispatch` table.

What this gives us:
- **No flicker** — GTK/cairo (and the AppKit/Win32 equivalents) double-buffer,
  and the toolkit does widget-level damage tracking *between* widgets.
- **Correct static rendering** — fine for SVG, the transpiler's output, and the
  librsvg parity harness (all of which render once, to a window or a PNG).

What it does **not** give us:
- **No intra-canvas dirty regions.** A single canvas widget is all-or-nothing:
  to change one shape we replay every command. There is no opaque tracking, no
  occlusion subtraction, no exact-rect clipped redraw *within* the canvas.
- So an animated/live element (video, game, a moving shape) forces a
  whole-canvas redraw per frame: O(entire scene) work for an O(one rect)
  change. This is the architectural ceiling the bar above breaks through.

## What we already have that seeds it

The retained compositor is **not a rewrite from zero.** The reactive and
element layers already built are precisely its foundation:

- **Per-element bounds.** `grammar_element` stores an axis-aligned bounds rect
  per `AevgElement` (`bounds_x/y/w/h` + `has_bounds`) and a working
  `element_hit_test`. That bounds rect is exactly *one rectangle* of a future
  dirty/opaque region. We already recompute it when geometry changes
  (`grammar_reactive.recompute_bounds` on a `bindPos`).
- **A retained-ish element list.** The context tracks elements
  (`ctx_tracked_elements`, `ctx_track/untrack_element`); `grammar_bind` already
  adds/removes/destroys elements (data-driven regions). This is the seed of the
  view tree — it needs to become an ordered tree with parent/child + z-order,
  but the membership and lifecycle machinery exists.
- **A reactive refresh loop.** `refresh(ctx)` → `grammar_reactive` re-evaluates
  per-element bindings and `grammar_bind` diffs regions. Today a refresh
  re-dispatches the whole scene; the compositor change is to make refresh mark
  **only changed elements' bounds dirty** and composite just those.
- **A backbuffer + flush already exist** at the toolkit layer (cairo image
  surface / GTK double buffer; `canvas_write_png` already renders the buffer
  off-screen — proof we can composite to an off-screen target and flush).
- **cairo gives us clipping for free.** `cairo_rectangle` + `cairo_clip` (and
  `CGContextClipToRect` / GDI clip regions) implement the per-element "draw
  clipped to these exact rects" step natively. We do not need to write a
  software clipper; we need to *compute the rects* and set the clip.

So the missing pieces are specific and bounded:
1. A **region type** — a set of non-overlapping rectangles with union/subtract/
   intersect/translate. Independently authored (or wrap an MIT-licensed region
   lib like pixman, which cairo already links). **This is the one genuinely new
   data structure.**
2. An **ordered element tree** with z-order and opaque flags (extend the
   existing tracked-element list).
3. The **multi-pass composite** (dirty up, subtract opaque, distribute down,
   clipped redraw) replacing "replay the whole command buffer." The passes are
   plain tree walks over (1) and (2).

## Staged path

Each stage is independently shippable and leaves the static path working.

1. **Region type.** Author (or adopt pixman via cairo) a non-overlapping-rect
   region with paste/remove/clip/translate/bounds. Pure data structure; unit-
   testable headless. *(The one new primitive.)*
2. **Dirty invalidation, single buffer.** Give each element a dirty region
   (seeded from its bounds). On a reactive change, mark only that element's
   bounds dirty; composite = union of dirty rects, set cairo clip to them,
   replay the command buffer *clipped*. Still whole-buffer replay, but only the
   dirty pixels are touched → already fixes "animate one shape cheaply" for
   non-overlapping cases.
3. **Opaque subtraction + z-order.** Add opaque regions and the occlusion
   subtraction passes. This is the step that delivers the overlapping-windows /
   dragged-sub-region miracle and lets a live rect coexist with static chrome
   at zero static cost.
4. **Live/foreign content as a composited node.** A `video`/`game`/GL element
   is an opaque node with its own draw clock; the compositor region-clips
   around it and never redraws it from the vector pipeline. Until stage 3
   lands, this is the *escape hatch* (a native `GtkVideo`/`GtkGLArea`
   positioned by the vg layout, composited by the host) — and notably the
   escape hatch is the *same architectural shape* as the final design, so it is
   not throwaway.

## Relationship to the immediate-roadmap work

Everything on the near-term roadmap — the `vg {}` drawing DSL, the SVG→AeVG
transpiler, `render_to(png)`, the librsvg parity gate over the 208-sample
corpus — is **static**: rendered once, to a window or a PNG. None of it
animates. Deferred-flush immediate-mode is correct and sufficient for all of
it, and building the compositor first would delay all of it to serve content
(video/game/animation) that is not yet a target.

So: **ship the static path on immediate mode; treat the retained compositor as
this named, specified future track.** When live/animated content becomes a
goal, this is the design — built clean-room from the prior art and the
foundation we already have, never from GPL source.

## The lineage (acknowledgement, not a dependency)

The bar this doc holds up is the one set by CPU compositors that achieved
glitch-free overlapping animated windows long before GPUs — the Amiga and
TaOS-era systems, and carried forward today by systems like ChrysaLisp. We
admire the result and aim for the same bar. We will reach it by independent
implementation of the public technique, consistent with aether-ui's MIT
license — *not* by porting any GPL implementation of it.
