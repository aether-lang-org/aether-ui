#!/bin/bash
# ci.sh — full aether_ui test pipeline as a CI job would run it.
#
# Phases:
#   1. Build every example (catches C/Aether compile regressions).
#   2. Smoke-launch the non-driver examples to catch runtime crashes the
#      HTTP-driven tests can't reach (widget wiring, reactive state init,
#      platform-backend regressions). Each binary is launched, given 1.5s
#      to render, then killed; still-alive = pass.
#   3. Launch example_calculator with the AetherUIDriver test server and
#      run test_calculator.sh (11 assertions).
#   4. Launch example_testable and run test_automation.sh (17 assertions).
#
# Platform handling:
#   macOS    — runs directly (AppKit).
#   Linux    — runs directly if $DISPLAY or $WAYLAND_DISPLAY is set; otherwise
#              auto-wraps with xvfb-run when available. Falls back to build-only.
#   Windows  — native Win32 backend. Aether-level examples are skipped (the
#              DSL has a separate module-resolution issue on MINGW that
#              blocks `ae build`). The C-level backend test suite
#              (tests/test_widgets.c) and the HTTP driver test
#              (tests/test_driver.sh) run instead — invoked via
#              `make contrib-aether-ui-check`.
#
# Exits non-zero only when an implemented platform fails. Leaves no
# background processes.
#
# Usage: ./ci.sh [port]

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR"
PORT="${1:-9222}"

cd "$ROOT"
mkdir -p build

# All examples that must compile in Phase 1.
EXAMPLES=(counter form picker styled system canvas testable calculator context_menu)
# Examples without a test server — Phase 2 smoke-launches each.
# calculator and testable are exercised through their HTTP drivers in
# Phases 3-4, so they are not smoke-tested here.
SMOKE_EXAMPLES=(counter form picker styled system canvas)
FAIL=0

OS="$(uname -s)"
case "$OS" in
    Darwin)  PLATFORM=macos ;;
    Linux)   PLATFORM=linux ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM=windows ;;
    *)       PLATFORM=unknown ;;
esac
echo "=== aether_ui CI on $OS ($PLATFORM) ==="

if [ "$PLATFORM" = "windows" ]; then
    echo "NOTICE: Windows backend uses a separate test flow."
    echo "        Run: make contrib-aether-ui-check"
    echo "        (headless widget suite + HTTP driver integration test)"
    exit 0
fi
if [ "$PLATFORM" = "unknown" ]; then
    echo "ERROR: unrecognized platform '$OS'."
    exit 1
fi

# Decide how to launch GUI binaries. On Linux CI runners without a display,
# wrap with xvfb-run so GTK4 has a framebuffer. The screen must be big enough
# that Xvfb's pointer (parked at screen centre) starts OUTSIDE the app
# window: GTK synthesizes crossing/motion events at the pointer position on
# every relayout, and a pointer sitting over the canvas fires the app's hover
# handler between test steps — rewriting the status line the assertions read.
launch_xvfb() { xvfb-run -a -s "-screen 0 3200x2000x24" "$@"; }
LAUNCH_PREFIX=""
if [ "$PLATFORM" = "linux" ]; then
    if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
        if command -v xvfb-run > /dev/null 2>&1; then
            LAUNCH_PREFIX="launch_xvfb"
            echo "no display detected — wrapping GUI launches with xvfb-run"
        else
            echo "NOTICE: no display and xvfb-run missing — will build-only."
            LAUNCH_PREFIX="SKIP_RUNTIME"
        fi
    fi
fi

run_server_test() {
    # Launch a binary with AETHER_UI_TEST_PORT set, wait for the test server,
    # run the given test script against it, kill the binary, propagate status.
    local bin="$1" script="$2" name="$3"
    echo "--- launching $bin ---"
    AETHER_UI_TEST_PORT="$PORT" $LAUNCH_PREFIX "$bin" > "/tmp/ci_${name}.app.log" 2>&1 &
    local pid=$!

    # Wait up to 6s for the server to come up.
    local up=0
    for _ in $(seq 1 30); do
        if curl -sf -o /dev/null "http://127.0.0.1:$PORT/widgets"; then up=1; break; fi
        sleep 0.2
    done
    if [ "$up" -ne 1 ]; then
        echo "  FAIL: $name test server never responded"
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        tail -20 "/tmp/ci_${name}.app.log" | sed 's/^/       /'
        return 1
    fi

    "$script" "$PORT"
    local rc=$?
    # Close the app PROPERLY: ask the driver to shut down (the app exits by
    # the same path as the user closing the window). Signal-killing the
    # xvfb-run wrapper is only a fallback — it can leave the app child alive
    # and HOLDING THE PORT, and the next phase then interrogates the wrong
    # app (a whole family of "impossible" test failures traced back to this).
    curl -sf -m 2 -X POST "http://127.0.0.1:$PORT/shutdown" > /dev/null 2>&1
    local freed=0
    for _ in $(seq 1 25); do
        if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/widgets"; then freed=1; break; fi
        sleep 0.2
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    if [ "$freed" -ne 1 ]; then
        pkill -f "$bin" 2>/dev/null
        for _ in $(seq 1 25); do
            curl -sf -o /dev/null "http://127.0.0.1:$PORT/widgets" || break
            sleep 0.2
        done
    fi
    return $rc
}

run_smoke_test() {
    # Launch a GUI binary, give it 1.5s to open its window, then kill it.
    # Pass iff the process is still alive at the deadline. A process that
    # exited early is a crash (missing widget impl, null deref on init,
    # backend API mismatch) — propagate non-zero and dump the tail.
    local bin="$1" name="$2"
    $LAUNCH_PREFIX "$bin" > "/tmp/ci_smoke_${name}.log" 2>&1 &
    local pid=$!
    sleep 1.5
    if kill -0 "$pid" 2>/dev/null; then
        echo "  OK   $name (alive 1.5s)"
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        return 0
    fi
    wait "$pid" 2>/dev/null; local rc=$?
    echo "  FAIL $name (exited early, rc=$rc)"
    tail -15 "/tmp/ci_smoke_${name}.log" | sed 's/^/       /'
    return 1
}

# AeVG port unit tests — pure Aether (no GTK/display), so they run even
# under SKIP_RUNTIME. Each is a self-contained `main()` that exits non-zero
# on the first failed assertion. Append new modules' tests here as they land.
AEVG_TESTS=(test_transform test_normalizer test_easing test_parser test_bbox test_blur test_rasterize test_grammar_utils test_grammar_context test_grammar_element test_grammar_rendering test_grammar_style test_grammar_shapes test_grammar_factories test_grammar_animations test_loader test_grammar_defs test_grammar_text test_grammar_css test_grammar_events test_path_builder test_render_as_raster test_grammar_bind test_grammar_reactive test_refresh test_reactive_bindpos test_backend_dispatch test_raster_roundtrip test_filter_routing test_gradient_fill test_vg test_transpiler test_grammar_interaction test_vg_interactive test_vg_when test_vg_bindto test_vg_bindpos test_vg_clock test_vg_hidpi test_vg_anim test_live_region)

# `ae cflags --libs` emits the transitive deps that libaether.a was
# built with (PCRE2 / OpenSSL / zlib / nghttp2 — see Aether CHANGELOG
# [current], the cmd_cflags fix). So `$(ae cflags)` alone is enough
# for any module that imports std.regex / std.cryptography / std.http
# / std.zlib. The earlier per-dep workaround is no longer needed.

echo "=== Phase 0: AeVG unit tests (pure Aether) ==="
for t in "${AEVG_TESTS[@]}"; do
    src="aevg/${t}.ae"
    cfile="build/aevg_${t}.c"
    bin="build/aevg_${t}"
    if ! aetherc "$src" "$cfile" > "/tmp/ci_aevg_${t}.log" 2>&1; then
        echo "  FAIL $t (compile)"
        tail -15 "/tmp/ci_aevg_${t}.log" | sed 's/^/       /'
        FAIL=$((FAIL + 1))
        continue
    fi
    if ! gcc "$cfile" $(ae cflags) -o "$bin" >> "/tmp/ci_aevg_${t}.log" 2>&1; then
        echo "  FAIL $t (link)"
        tail -15 "/tmp/ci_aevg_${t}.log" | sed 's/^/       /'
        FAIL=$((FAIL + 1))
        continue
    fi
    if "$bin" > "/tmp/ci_aevg_${t}_run.log" 2>&1; then
        echo "  OK   $t"
    else
        echo "  FAIL $t (run)"
        tail -15 "/tmp/ci_aevg_${t}_run.log" | sed 's/^/       /'
        FAIL=$((FAIL + 1))
    fi
done
echo

echo "=== Phase 1: build all aether_ui examples (aeb fan-out) ==="
# Each example is a per-app .build.ae node under examples/<app>/; the root
# .all.ae scans + deps them, so this one command builds all 11 (cached,
# parallel). Binaries land at target/build/examples/<app>/bin/<app>; EX_BIN
# resolves that for the smoke/driver phases below.
EX_BIN() { echo "$ROOT/target/build/examples/$1/bin/$1"; }
if ( cd "$ROOT" && aeb .all.ae ) > /tmp/ci_build_all.log 2>&1; then
    for ex in "${EXAMPLES[@]}"; do
        if [ -x "$(EX_BIN "$ex")" ]; then echo "  OK   $ex"; else
            echo "  FAIL $ex (no binary)"; FAIL=$((FAIL + 1)); fi
    done
else
    echo "  FAIL: aeb .all.ae fan-out build failed"
    tail -25 /tmp/ci_build_all.log | sed 's/^/       /'
    FAIL=$((FAIL + 1))
fi

# Phase 1.5: RUN the headless AeVG renderers (build was done by the .all.ae
# fan-out in Phase 1 — every aevg app is an aevg/apps/<name>/ node now). The
# value here is RUNTIME coverage the build-only fan-out can't give: each writes
# a PNG, exercising the raster-blit + draw-region compose path. Linux/GTK only.
AEVG_BIN() { echo "$ROOT/target/build/aevg/apps/$1/bin/$1"; }
if [ "$PLATFORM" = "linux" ]; then
    echo "  --- AeVG headless renderers (run → PNG) ---"
    run_png() {  # $1=app $2=desc
        local bin; bin="$(AEVG_BIN "$1")"
        if [ -x "$bin" ] \
                && AEVG_OUT="/tmp/ci_$1.png" "$bin" > "/tmp/ci_run_$1.log" 2>&1 \
                && [ -f "/tmp/ci_$1.png" ]; then
            echo "  OK   $1 ($2)"
        else
            echo "  FAIL $1"
            tail -15 "/tmp/ci_run_$1.log" 2>/dev/null | sed 's/^/       /'
            FAIL=$((FAIL + 1))
        fi
    }
    run_png aevg_live_png   "live raster + draw region composite"
    run_png aevg_video_png  "raw-RGBA clip in a live region"
    run_png analog_clock_png "one-frame clock render"
fi

if [ "$FAIL" -gt 0 ]; then
    echo
    echo "=== CI result: $FAIL build failure(s) — skipping runtime phases ==="
    exit 1
fi

if [ "$LAUNCH_PREFIX" = "SKIP_RUNTIME" ]; then
    echo
    echo "=== CI result: builds passed; runtime phases skipped (no display) ==="
    exit 0
fi

echo
echo "=== Phase 2: smoke-launch non-driver examples ==="
for ex in "${SMOKE_EXAMPLES[@]}"; do
    run_smoke_test "$(EX_BIN "$ex")" "$ex" || FAIL=$((FAIL + 1))
done

# Phases 3-6 are Aeocha specs (tests/<app>/spec_*.ae — Aether programs on
# the shared tests/lib/uidriver.ae client; tests/run_spec.sh is launcher
# glue). They need the aeocha clone.
AEOCHA_DIR="${AEOCHA_DIR:-$HOME/scm/aeocha}"
if [ ! -f "$AEOCHA_DIR/aeocha.ae" ]; then
    echo "NOTICE: aeocha not found at $AEOCHA_DIR — driver spec phases (3-6) FAIL."
    echo "        (clone github.com/aether-lang-org/aeocha or set AEOCHA_DIR)"
    FAIL=$((FAIL + 1))
    AEOCHA_OK=0
else
    AEOCHA_OK=1
fi

echo
echo "=== Phase 3: AetherUIDriver calculator spec ==="
if [ "$AEOCHA_OK" -eq 1 ]; then
    UI_SPEC=calculator/spec_calculator \
    run_server_test "$(EX_BIN calculator)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" calculator || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 4: AetherUIDriver testable spec ==="
if [ "$AEOCHA_OK" -eq 1 ]; then
    UI_SPEC=testable/spec_testable \
    run_server_test "$(EX_BIN testable)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" testable || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5: AetherUIDriver context-menu spec ==="
if [ "$AEOCHA_OK" -eq 1 ]; then
    UI_SPEC=context_menu/spec_context_menu \
    run_server_test "$(EX_BIN context_menu)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" context_menu || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 6: AetherUIDriver grand_perspective tests (Aeocha specs) ==="
# One Aeocha spec per app component (tests/grand_perspective/spec_*.ae —
# Aether programs driving the HTTP API via std.http.client; run_spec.sh is
# only include-path glue). Each spec runs against a FRESH app instance
# scanning a FRESH fixture — the fileops spec really trashes a fixture
# file, so isolation is what makes the specs order-independent. The app
# scans $AEVG_DIR on launch; specs assert against the same tree via
# $GP_FIXTURE. Fixture under $HOME: gio trash refuses /tmp on some OSes
# (FreeBSD: "Trashing on system internal mounts is not supported").
# Xvfb runs need the cairo renderer (GTK's NGL on llvmpipe churns memory).
if [ "$AEOCHA_OK" -eq 1 ]; then
    case "$LAUNCH_PREFIX" in *xvfb*) export GSK_RENDERER=cairo ;; esac
    for gp_spec in scan_and_list map_nav legend fileops hover_and_resize; do
        GP_FIX=$(mktemp -d "$HOME/.gp-ci-XXXXXX")
        mkdir -p "$GP_FIX/sub"
        head -c 400000 /dev/urandom > "$GP_FIX/big.bin"
        head -c 250000 /dev/urandom > "$GP_FIX/mid.bin"
        head -c 200000 /dev/urandom > "$GP_FIX/sub/inner.bin"
        export AEVG_DIR="$GP_FIX" GP_FIXTURE="$GP_FIX" UI_SPEC="grand_perspective/spec_${gp_spec}"
        run_server_test "$ROOT/target/build/aevg/apps/grand_perspective/bin/grand_perspective" \
                        "$SCRIPT_DIR/tests/run_spec.sh" "gp_${gp_spec}" || FAIL=$((FAIL + 1))
        unset AEVG_DIR GP_FIXTURE UI_SPEC
        rm -rf "$GP_FIX"
    done
    unset GSK_RENDERER
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "=== CI result: all phases passed ==="
    exit 0
else
    echo "=== CI result: $FAIL phase(s) failed ==="
    exit 1
fi
