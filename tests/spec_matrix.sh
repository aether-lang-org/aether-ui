#!/bin/bash
# spec_matrix.sh — run every Aeocha suite against its app and tabulate.
#
# The platform-parity baseline in one command. The Windows equivalent of this
# lived in a session scratchpad and was never committed (see roadmap.md), which
# meant the next person had to reconstruct it from ci.sh. This is that script,
# committed, and portable across all three backends.
#
#   ./tests/spec_matrix.sh              # every suite
#   ./tests/spec_matrix.sh split table  # just these
#
# Binaries come from `aeb .all.ae` (target/build/...). Run that first.
#
# Counting: the number printed is aeocha's own "N passing" — one per it()
# block. Failures are counted from its "N failing" tail. Those two numbers
# come straight from the tool, so the matrix is reproducible rather than
# hand-tallied (the roadmap's Windows row mixes it-counts and assertion-counts
# and is confusing as a result).

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT=9222
export AETHER_UI_HEADLESS=1
export AETHER_UI_NO_ANIMATION=1

# suite | binary | spec | extra env
SUITES=(
  "calculator|examples/calculator|calculator/spec_calculator|"
  "text_metrics|examples/calculator|text_metrics/spec_text_metrics|"
  "testable|examples/testable|testable/spec_testable|"
  "context_menu|examples/context_menu|context_menu/spec_context_menu|"
  "overlay|examples/overlay_demo|overlay_demo/spec_overlay_demo|"
  "vg_tooltip|examples/vg_tooltip|vg_tooltip/spec_vg_tooltip|AETHER_UI_TOOLTIP=drawn"
  "picker|examples/picker|picker/spec_picker|AETHER_UI_PICKER=drawn"
  "each|examples/each_demo|each_demo/spec_each_demo|"
  "listbox|examples/listbox_demo|listbox_demo/spec_listbox_demo|"
  "table|examples/table_demo|table_demo/spec_table_demo|"
  "split|examples/split_demo|split_demo/spec_split_demo|"
  "bindings|examples/bindings_demo|bindings_demo/spec_bindings_demo|"
  "tabs|examples/tabs_demo|tabs_demo/spec_tabs_demo|"
  "menu|examples/menu|menu/spec_menu|"
  "rbind|examples/rbind_demo|rbind_demo/spec_rbind_demo|"
  "typo|examples/typo_demo|typo_demo/spec_typo_demo|"
  "multiselect|examples/multiselect_demo|multiselect_demo/spec_multiselect_demo|"
  "dblclick|examples/dblclick_demo|dblclick_demo/spec_dblclick_demo|"
  "tree|examples/tree_demo|tree_demo/spec_tree_demo|"
  "tabledeleg|examples/tabledeleg_demo|tabledeleg_demo/spec_tabledeleg_demo|"
  "weightclamp|examples/weightclamp_demo|weightclamp_demo/spec_weightclamp_demo|"
  "shortcut|examples/shortcut_demo|shortcut_demo/spec_shortcut_demo|"
  "polish|examples/polish_demo|polish_demo/spec_polish_demo|"
  "vlist|examples/vlist_demo|vlist_demo/spec_vlist_demo|"
  "falling_blocks|apps/falling_blocks|falling_blocks/spec_falling_blocks|"
  "svg_tetris|apps/svg_tetris|svg_tetris/spec_svg_tetris|"
  "rubiks_cube|apps/rubiks_cube|rubiks_cube/spec_rubiks_cube|"
  "lismusic|apps/LisMusic|LisMusic/spec_lismusic|LIS_OFFLINE=1"
)

# grand_perspective: one spec per component, each against a FRESH app scanning
# a FRESH fixture — the fileops spec really trashes a file, so isolation is what
# makes these order-independent. Fixture lives under $HOME: `gio trash` refuses
# /tmp on some systems.
GP_SPECS=(scan_and_list map_nav legend fileops hover_and_resize)

WANT=("$@")
want_suite() {
    [ ${#WANT[@]} -eq 0 ] && return 0
    for w in "${WANT[@]}"; do [ "$w" = "$1" ] && return 0; done
    return 1
}

# Wait for the port to stop answering — a lingering app would let the NEXT
# suite interrogate the PREVIOUS app's widget tree, which produces a whole
# family of impossible failures.
port_free() {
    for _ in $(seq 1 40); do
        curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets" || return 0
        sleep 0.25
    done
    return 1
}

teardown() {
    local pid="$1"
    curl -s -o /dev/null --max-time 2 -X POST "http://127.0.0.1:$PORT/shutdown" || true
    port_free || { kill "$pid" 2>/dev/null; sleep 0.5; kill -9 "$pid" 2>/dev/null; }
    wait "$pid" 2>/dev/null
}

printf "%-14s %6s %6s   %s\n" SUITE PASS FAIL RESULT
printf -- "------------------------------------------------------------\n"

TOTAL_PASS=0 TOTAL_FAIL=0 SUITES_RED=0

for row in "${SUITES[@]}"; do
    IFS='|' read -r name appdir spec extra <<< "$row"
    want_suite "$name" || continue

    bin="target/build/$appdir/bin/$(basename "$appdir")"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            # aeb's fan-out can't build UI apps on MSYS yet (the
            # _orchestrator.c generation bug) — build.sh into build/ is
            # the Windows path, building on demand.
            bin="build/$(basename "$appdir").exe"
            if [ ! -x "$bin" ]; then
                ./build.sh "$appdir/$(basename "$appdir").ae" \
                    "$(basename "$appdir")" > "/tmp/smx_$(basename "$appdir").log" 2>&1 \
                    || true
            fi
            ;;
    esac
    if [ ! -x "$bin" ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "NO BINARY ($bin) — run: aeb .all.ae (or see /tmp/smx_*.log on Windows)"
        SUITES_RED=$((SUITES_RED + 1))
        continue
    fi

    port_free || { echo "port $PORT still busy; aborting"; exit 1; }

    log="$(mktemp)"
    # shellcheck disable=SC2086
    env AETHER_UI_TEST_PORT=$PORT $extra "$bin" >"$log" 2>&1 &
    pid=$!

    ready=0
    for _ in $(seq 1 40); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.25
    done
    if [ "$ready" -ne 1 ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "APP DID NOT START (see $log)"
        SUITES_RED=$((SUITES_RED + 1))
        kill "$pid" 2>/dev/null
        continue
    fi

    out="$(UI_SPEC="$spec" tests/run_spec.sh 2>&1)"
    rc=$?
    teardown "$pid"

    pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
    fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
    pass=${pass:-0}; fail=${fail:-0}

    if [ "$rc" -ne 0 ] && [ "$pass" = "0" ] && [ "$fail" = "0" ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "SPEC ERROR"
        printf '%s\n' "$out" | tail -4 | sed 's/^/                              | /'
        SUITES_RED=$((SUITES_RED + 1))
        rm -f "$log"
        continue
    fi

    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail))
    if [ "$fail" -gt 0 ]; then
        SUITES_RED=$((SUITES_RED + 1))
        printf "%-14s %6s %6s   %s\n" "$name" "$pass" "$fail" "RED"
        printf '%s\n' "$out" | grep -E '✗|FAIL|not ok' | head -6 \
            | sed 's/^/                              | /'
    else
        printf "%-14s %6s %6s   %s\n" "$name" "$pass" "$fail" "green"
    fi
    rm -f "$log"
done

# --- grand_perspective (the real vg app: canvas hit-testing, resize, fileops) --
GP_BIN="target/build/apps/grand_perspective/bin/grand_perspective"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        GP_BIN="build/grand_perspective.exe"
        if [ ! -x "$GP_BIN" ]; then
            ./build.sh apps/grand_perspective/grand_perspective.ae \
                grand_perspective > /tmp/smx_gp.log 2>&1 || true
        fi
        ;;
esac
for gp in "${GP_SPECS[@]}"; do
    want_suite "gp_$gp" || continue
    if [ ! -x "$GP_BIN" ]; then
        printf "%-14s %6s %6s   %s\n" "gp_$gp" - - "NO BINARY — run: aeb .all.ae"
        SUITES_RED=$((SUITES_RED + 1))
        continue
    fi
    port_free || { echo "port $PORT still busy; aborting"; exit 1; }

    fix="$(mktemp -d "$HOME/.gp-ci-XXXXXX")"
    mkdir -p "$fix/sub"
    head -c 400000 /dev/urandom > "$fix/big.bin"
    head -c 250000 /dev/urandom > "$fix/mid.bin"
    head -c 200000 /dev/urandom > "$fix/sub/inner.bin"
    # The app and the spec are NATIVE binaries — hand them a native path,
    # not an MSYS one (/c/Users/... opens as no-such-dir in the Windows CRT
    # and gp scans nothing).
    fix_app="$fix"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) fix_app="$(cygpath -m "$fix")" ;;
    esac

    log="$(mktemp)"
    env AETHER_UI_TEST_PORT=$PORT AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" \
        "$GP_BIN" >"$log" 2>&1 &
    pid=$!
    ready=0
    for _ in $(seq 1 40); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.25
    done
    if [ "$ready" -ne 1 ]; then
        printf "%-14s %6s %6s   %s\n" "gp_$gp" - - "APP DID NOT START (see $log)"
        SUITES_RED=$((SUITES_RED + 1))
        kill "$pid" 2>/dev/null; rm -rf "$fix"
        continue
    fi

    out="$(AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" UI_SPEC="grand_perspective/spec_${gp}" \
           tests/run_spec.sh 2>&1)"
    teardown "$pid"
    rm -rf "$fix" "$log"

    pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
    fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
    pass=${pass:-0}; fail=${fail:-0}
    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail))
    if [ "$fail" -gt 0 ] || { [ "$pass" -eq 0 ] && [ "$fail" -eq 0 ]; }; then
        SUITES_RED=$((SUITES_RED + 1))
        printf "%-14s %6s %6s   %s\n" "gp_$gp" "$pass" "$fail" "RED"
        printf '%s\n' "$out" | grep -E '✗|FAIL|not ok' | head -6 \
            | sed 's/^/                              | /'
    else
        printf "%-14s %6s %6s   %s\n" "gp_$gp" "$pass" "$fail" "green"
    fi
done

printf -- "------------------------------------------------------------\n"
printf "%-14s %6s %6s   %s\n" TOTAL "$TOTAL_PASS" "$TOTAL_FAIL" \
    "$([ "$SUITES_RED" -eq 0 ] && echo "all green" || echo "$SUITES_RED suite(s) red")"
[ "$SUITES_RED" -eq 0 ]
