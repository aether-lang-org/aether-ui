# AeVG — open follow-ons

Tracked items not yet built. The live-region abstraction (raster + draw sources,
in-scene glitch-free composition, scaled blit, per-region z-index, animated
sources, raw-RGBA frame source) is feature-complete; these are the next layers.

## Build system (aeb)

- **Build aether-ui through `aeb` instead of `build.sh`/`ci.sh`.** `bootstrap.sh`
  (toolchain bootstrap, mirrors servirtium-vcr) is in; `aevg/.analog-clock.ae`
  is the working proof — `aeb` builds the GTK clock binary via the aether SDK's
  manual path with **`no_closure_regen()`** (the opt-out filed as
  `~/scm/aeb/asks/thin-aether-over-c-backend.md`, since landed: it compiles the
  entry with plain `aetherc` + links the C backend, instead of `--emit=lib`-ing
  the extern-backed import closure). **Run aeb from the repo ROOT** (`aeb
  aevg/.analog-clock.ae`) so its root-discovery puts the repo root on the
  aetherc `--lib` path — `analog_clock.ae` lives in `aevg/` but imports
  root-level `aether_ui`; from inside `aevg/` that import doesn't resolve.
  The Mac/Win/Lin backend-C/framework branching is now expressed in
  `.analog-clock.ae`'s `main()` via `os.platform()` (Aether 0.203),
  mirroring build.sh's `uname` case — linux verified, darwin/windows wired
  but untested on this host.
  NEXT: convert the other examples + the 41 Phase-0 tests to `.build.ae` /
  `.tests.ae` (the platform branch is a copy-paste block — factor it into a
  shared helper once a 2nd descriptor needs it), and retire the bash.

## Live regions / video

- **A true video decoder** — the current real source is raw RGBA
  (`example_aevg_video`, concatenated `w*h*4` frames). A decoder (h264/etc.)
  would be a separate, larger piece — likely an FFI to a codec lib.

- **Region-dirty optimization** — only reflush changed regions (perf, not
  correctness; full reflush is fine until it isn't).

- **The multi-window system compositor** — still out of scope. (Region algebra,
  occlusion passes, per-view backbuffers, host blit ABI; see
  `docs/aevg-live-regions-plan.md`.)
