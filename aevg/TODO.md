# AeVG — open follow-ons

Tracked items not yet built. The live-region abstraction (raster + draw sources,
in-scene glitch-free composition, scaled blit, per-region z-index, animated
sources, raw-RGBA frame source) is feature-complete; these are the next layers.

## Live regions / video

- **A true video decoder** — the current real source is raw RGBA
  (`example_aevg_video`, concatenated `w*h*4` frames). A decoder (h264/etc.)
  would be a separate, larger piece — likely an FFI to a codec lib.

- **Region-dirty optimization** — only reflush changed regions (perf, not
  correctness; full reflush is fine until it isn't).

- **The multi-window system compositor** — still out of scope. (Region algebra,
  occlusion passes, per-view backbuffers, host blit ABI; see
  `docs/aevg-live-regions-plan.md`.)
