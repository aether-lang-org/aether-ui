# AeVG — open follow-ons

Tracked items not yet built. The live-region abstraction (raster + draw sources,
in-scene glitch-free composition, scaled blit, per-region z-index, animated
sources, raw-RGBA frame source) is feature-complete; these are the next layers.

## Build system (aeb)

- **Build aether-ui through `aeb` instead of `build.sh`/`ci.sh`.** `bootstrap.sh`
  (toolchain bootstrap, mirrors servirtium-vcr) is in; `aevg/.analog_clock.build.ae`
  is a spike. BLOCKED on an aeb gap: its `aether.program` manual path
  `--emit=lib`s the whole transitive import closure, which fails for aether-ui's
  Aether-thin-over-C-backend modules (extern function bodies → E0301). Need an
  opt-out so the entry compiles with plain `aetherc` + links the C backend, like
  `build.sh` does. Full ask: `docs/aeb-ask-thin-aether-over-c-backend.md`. Once
  aeb supports it: convert the examples + the 41 Phase-0 tests to `.build.ae` /
  `.tests.ae` and retire the bash.

## Toolchain migrations

- **Replace `aether_local_secs_of_day()` with `std.os.now_local()`** — the
  analog clock's local-time source is currently a stopgap C helper in
  `aether_ui_system_extras.c` (`localtime_r`/`GetLocalTime` → secs-of-day),
  because `now_local()` (→ `*LocalTime` with year/month/day/hour/minute/second/
  nanos/tz_offset_minutes) is only in the Aether *source* tree, not the
  installed `/usr/local` std.os the compiler resolves. Once it ships installed:
  swap `analog_clock.ae` + `analog_clock_png.ae` to `os.now_local()` and delete
  the helper. (Verified 2026-05-31: installed aetherc reports `now_local`
  undefined.)

## Live regions / video

- **A true video decoder** — the current real source is raw RGBA
  (`example_aevg_video`, concatenated `w*h*4` frames). A decoder (h264/etc.)
  would be a separate, larger piece — likely an FFI to a codec lib.

- **Region-dirty optimization** — only reflush changed regions (perf, not
  correctness; full reflush is fine until it isn't).

- **The multi-window system compositor** — still out of scope. (Region algebra,
  occlusion passes, per-view backbuffers, host blit ABI; see
  `docs/aevg-live-regions-plan.md`.)
