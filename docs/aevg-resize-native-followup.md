# AeVG `vg{}` resize — macOS & Win32 follow-up

> **Handoff note** for a sibling Claude working in a native macOS (AppKit) or
> Windows (Win32) environment. The GTK backend has live `vg{}`-scene resize
> working; AppKit and Win32 currently have **honest no-op stubs**. This doc is
> the exact contract + the GTK reference to mirror, so you can wire each
> platform without re-deriving the design.

## TL;DR — what you're implementing

When a window hosting an AeVG `vg { … }` scene is resized, the vector scene
must **rescale to fill the canvas** (re-map the viewBox→canvas transform and
re-flush the recorded shapes at the new size). On GTK this works; on your
platform it doesn't yet. Two pieces, both per-platform:

1. **Make the canvas widget fill its allocation** (so the window's size
   actually reaches the canvas — without this it stays at its initial size and
   gets centered, which is the bug we hit on GTK).
2. **Fire the registered resize closure** with the new `(width, height)` when
   the canvas's drawn size changes, **before** replaying the command buffer.

The Aether/vg layer is already done and platform-agnostic — you only touch the
native C/ObjC backend file. **Do not change** `aevg/*.ae`, `aether_ui.ae`, or
`aether_ui_backend.h` unless you find an ABI gap (none expected).

## The ABI contract (already declared, do not change)

`aether_ui_backend.h`:

```c
// Register a resize hook: the boxed closure (taking new w, h) fires when the
// canvas allocation changes. AeVG's vg{} scope uses it to re-map its viewBox
// and re-flush its shapes at the new scale, so a resized window rescales the
// vector scene. No-op on backends without live resize delivery.
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure);
```

- `boxed_closure` is an `AeClosure*` (`{ void (*fn)(void); void* env; }` — the
  struct is already defined at the top of every backend file).
- The closure takes **two `intptr_t` args** (new width, new height). Invoke it
  exactly as the slider/toggle callbacks do in your file:
  ```c
  ((void(*)(void*, intptr_t, intptr_t))c->fn)(c->env, (intptr_t)w, (intptr_t)h);
  ```
- On the Aether side the closure is `callback |rw: int, rh: int| { … }` and it
  does: rebuild `AevgContext` for `(rw, rh)` → `scene_set_ctx` →
  `canvas_clear` → `vg_flush`. **You don't write any of that** — it's in
  `aevg/vg_live.ae`'s `vg()` and already registered.

### Critical: the closure runs *inside* your paint/draw path

The closure rebuilds the canvas command buffer (it calls `canvas_clear` then
re-dispatches every shape). So fire it **before** you replay the buffer in the
same paint pass, and **the closure must NOT trigger another repaint** (no
`setNeedsDisplay` / `InvalidateRect` from within it) or you'll loop. The GTK
version fires it at the top of the draw func, then replays — mirror that
ordering. (The Aether closure deliberately omits a redraw call for exactly this
reason; see the comment in `vg_live.ae`.)

## GTK reference implementation (mirror this)

File `aether_ui_gtk4.c`. Three edits made it work — find the parallel spots in
your file.

### 1. `CanvasState` gained the hook + last-size

```c
typedef struct {
    CanvasCmd* cmds; int count; int capacity; int widget_handle;
    AeClosure* on_resize;   // the registered flush closure (NULL = none)
    int last_w;             // last drawn size, to detect change
    int last_h;
} CanvasState;
```

### 2. The draw func fires the hook on size change, before replay

```c
static void canvas_draw_func(GtkDrawingArea* area, cairo_t* cr,
                              int width, int height, gpointer data) {
    int canvas_id = (int)(intptr_t)data;
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs && cs->on_resize && cs->on_resize->fn &&
        (width != cs->last_w || height != cs->last_h)) {
        cs->last_w = width;
        cs->last_h = height;
        ((void(*)(void*, intptr_t, intptr_t))cs->on_resize->fn)(
            cs->on_resize->env, (intptr_t)width, (intptr_t)height);
    }
    canvas_replay(cr, cs);   // replays the (possibly just-rebuilt) buffer
}
```

### 3. The canvas widget fills its allocation (THE bug we hit)

This is the non-obvious one. A `GtkDrawingArea` with a fixed content size
won't grow — it sat at 400×300 inside a bigger window and the parent just
**centered it horizontally**, so the draw func never saw a new size and the
scene never rescaled. Fix in `canvas_create_impl`:

```c
gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), width);   // = natural/min
gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), height);
gtk_widget_set_hexpand(da, TRUE);     // <- grow to fill
gtk_widget_set_vexpand(da, TRUE);
gtk_widget_set_halign(da, GTK_ALIGN_FILL);
gtk_widget_set_valign(da, GTK_ALIGN_FILL);
```

Content size becomes the *minimum/natural* size (still right for the headless
PNG path and a canvas in a scroll view); expand+fill makes it take the whole
allocation. **Your platform has the analogous trap** — see below.

### 4. `on_resize_impl` registration + first-draw seeding

```c
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    cs->on_resize = (AeClosure*)boxed_closure;
    cs->last_w = -1;   // force the first real allocation to fire the hook,
    cs->last_h = -1;   // so the scene fits the ACTUAL size from frame one
}
```

The `-1` seeding matters: it makes the first paint re-map to the real
allocation (which differs from the requested size after window chrome), so the
scene fits correctly on first show, not just after a manual drag.

---

## macOS (AppKit) — `aether_ui_macos.m`

Current stub is at **`aether_ui_canvas_on_resize_impl`** (~line 1493) and does
nothing. The canvas is an `AetherCanvasView : NSView` (~line 1311) with
`- (BOOL)isFlipped { return YES; }` and a `- (void)drawRect:` (~line 1318) that
calls the shared replay over `CanvasState`. `canvas_create_impl` is ~line 1462.

### Steps

1. **Add to `CanvasState`** (the macOS copy, ~line 1290): `AeClosure* on_resize;
   int last_w; int last_h;`. Init them in `canvas_create_impl` (`on_resize=NULL`,
   `last_w/last_h = width/height`).

2. **Fire the hook from `drawRect:`** — same shape as GTK. At the top of
   `drawRect:`, get the view's `bounds.size`, compare to `last_w/last_h`, and if
   changed, fire the closure with the new pixel size **before** the replay loop.
   Be careful with the coordinate space: AeVG works in pixels and the view
   `isFlipped`, so use the integer `bounds` size (points). If the display is
   Retina and the command buffer is built in points (it is — the rest of
   `drawRect:` draws in points), pass the **point** size, not backing pixels.
   Do **not** call `setNeedsDisplay` from inside (you're already drawing).

3. **Make the view fill its container** — the AppKit analogue of GTK's
   expand+fill. The canvas view is added into the stack/box hierarchy with
   autoresizing or Auto Layout. Ensure it stretches:
   - If the parent uses **autoresizing masks**:
     `v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;` and make
     sure `translatesAutoresizingMaskIntoConstraints` is left as-is for that path.
   - If the parent is an **`NSStackView`** (check how vstack is built in this
     file): set the view to fill — e.g. low hugging priority
     (`[v setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:…]`
     both axes) so the stack grows it, and verify the stack's distribution
     gives it the slack. The exact call depends on how the surface container is
     constructed here — find the vstack/stack creation and match its idiom.
   - Sanity check: the *initial* `NSMakeRect(0,0,width,height)` frame in
     `canvas_create_impl` is fine as a natural size; the goal is only that it
     *grows* when the window grows.

4. **`on_resize_impl`**: store the closure, seed `last_w/last_h = -1` (same
   reason as GTK).

### macOS verification

- A live test needs a display session (you're in the native env, so you can run
  the app). Resize the window; the scene should grow/shrink to fill.
- Headless-ish check: the re-map/re-flush logic is platform-agnostic and
  already covered by `aevg/test_vg.ae` (the `resize:` asserts). Your job is only
  to ensure `drawRect:` delivers the new size to the closure. A cheap proof:
  temporarily log `bounds.size` in `drawRect:` (gated on an env var, like GTK's
  `AEVG_TRACE_DRAW`) and confirm it changes as you resize — exactly how the GTK
  bug was found.

---

## Windows (Win32) — `aether_ui_win32.c`

Current stub is at **`aether_ui_canvas_on_resize_impl`** (~line 2352). Canvas
paint is `WM_PAINT` (~line 2721) replaying a recorded command stream;
`canvas_create_impl` ~line 2316. There's an `invoke_closure(AeClosure*)` helper
(~line 68) — but note it's the **no-arg** form; you need the two-arg cast inline
(don't route the resize closure through `invoke_closure`).

### Steps

1. **Add to the canvas struct** (find the canvas record — the union member
   `struct { int canvas_id; } canvas;` is ~line 206; the command-stream storage
   is near the `WM_PAINT` handler): `AeClosure* on_resize; int last_w; int
   last_h;`. Init in `canvas_create_impl`.

2. **Fire on `WM_SIZE` of the canvas window** (the canvas is its own `HWND`).
   There are several `WM_SIZE` handlers in this file (the stack container at
   ~585/747, others) — you want the **canvas window's** proc. On `WM_SIZE` with
   the new client size (`LOWORD/HIWORD(lParam)`), if it changed vs `last_w/h`:
   - update `last_w/last_h`,
   - fire the closure with the new size (two-arg cast),
   - then `InvalidateRect(hwnd, NULL, TRUE)` to schedule the repaint that
     replays the freshly-rebuilt buffer.

   **Ordering note (differs from GTK/macOS):** on Win32 it's cleaner to rebuild
   the buffer in `WM_SIZE` and *then* invalidate, rather than rebuilding inside
   `WM_PAINT`. So here firing the closure in `WM_SIZE` (not `WM_PAINT`) is fine
   — the closure's `canvas_clear` + re-dispatch happen, then the subsequent
   `WM_PAINT` replays. Just don't fire it *again* in `WM_PAINT`.

3. **Make the canvas `HWND` fill the client area.** The window has a layout
   pass (the stack `WM_SIZE` at ~585 repositions children by preferred size).
   Ensure the canvas child is told to fill rather than sit at its preferred
   400×300 — i.e. in the parent's layout/`WM_SIZE`, give the canvas child the
   full available rect (`MoveWindow`/`SetWindowPos` to the client size) instead
   of its natural size. This is the Win32 analogue of GTK's expand+fill and the
   macOS autoresize: **without it the canvas stays small and the resize hook
   never sees a new size.** Find where canvas children are positioned in the
   container layout and special-case "fill" for the canvas (or honor an
   hexpand/vexpand flag if the layout already has one — check the stack code).

4. **`on_resize_impl`**: store the closure, seed `last_w/last_h = -1`.

### Win32 verification

- Native env: run the app, resize, watch the scene rescale.
- Same trace trick: gate a `fprintf(stderr, …)`/`OutputDebugString` on an env
  var in the canvas `WM_SIZE`/`WM_PAINT` and confirm the size changes.

---

## Shared gotchas (bit us on GTK, will bite you too)

- **The "fill" step is the real fix.** The resize *hook* is easy; the trap is
  the canvas not growing, so the hook never fires with a new size. Verify the
  canvas actually changes size first (trace it), then wire the hook.
- **No re-entrant repaint from the closure.** It rebuilds the buffer; it must
  not request another paint, or you loop. (The Aether closure omits the redraw
  on purpose.)
- **Seed `last_w/last_h = -1` in `on_resize_impl`** so the first real paint
  fits the scene to the actual (post-chrome) allocation.
- **Pixels vs points (macOS).** Match whatever space the rest of `drawRect:`
  draws in. The command buffer is already built in that space.
- **`vg_flush` is re-runnable by design** — it folds each element's cached
  style back into attrs and re-dispatches; `canvas_clear` before each re-flush
  prevents doubling. You rely on this but don't implement it; it's in
  `aevg/vg.ae`.

## Done-when

- Resizing a window with a `vg{}` scene rescales the vector scene in **both
  axes**, preserving aspect ratio (`xMidYMid meet` — letterboxes on the
  non-limiting axis; that's correct SVG behavior, not a bug).
- `aevg/test_vg.ae` still passes (it's platform-agnostic; build+run it).
- Update the stub comment in your backend file from "honest stub / not wired"
  to a real description, and flip the parity note in
  `git log` commit `f7c4af0` / this doc's status.

## Reference commits (GTK)

- `607fed6` — vg{} scenes rescale on window resize (the hook + re-map/re-flush)
- `f7c4af0` — make canvas fill its allocation so vg{} actually resizes (the
  expand+fill fix — **the subtle one**)
- `728df1b` — deferred-flush colour fix (why `vg_flush` exists and is
  re-runnable; required reading for understanding the re-flush)
