# helpers.sh — shared harness for the grand_perspective driver test scripts.
#
# Each test script is STANDALONE: it expects a freshly launched app scanning
# a fresh fixture (ci.sh recreates both per script), asserts over the
# AetherUIDriver HTTP API, and reports its own pass/fail tally.
#
#   fixture/                    (created by the caller; $GP_FIXTURE)
#     big.bin   400KB           (list row 0)
#     mid.bin   250KB           (list row 1)
#     sub/inner.bin  200KB      (row 2 — the only sub-directory)
#
# Usage in a script:
#   source "$(dirname "$0")/helpers.sh"
#   wait_for_app
#   ...tests...
#   results

PORT="${1:-9222}"
BASE="http://127.0.0.1:$PORT"
PASS=0
FAIL=0

assert_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if echo "$haystack" | grep -q "$needle"; then
        echo "  PASS: $desc"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected to contain '$needle', got '$haystack')"
        FAIL=$((FAIL + 1))
    fi
}

# The bottom status line: the text widget carrying scan/selection/hover text.
status_line() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
ws=json.load(sys.stdin)
best=''
for w in ws:
    t=w.get('text','')
    if w['type']=='text' and any(k in t for k in ('Scan','Folder','File:','Highlight','trash','Select','Copied','—')):
        best=t
print(best)"
}

# Visible breadcrumb labels, left to right (excludes the fixed toolbar).
crumbs() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
print(' / '.join(w['text'] for w in json.load(sys.stdin) if w['type']=='button' and w['visible'] and w['text'] not in ('Zoom In','Zoom Out','Rescan','Stop','Open','Reveal','Delete','Confirm delete?')))"
}

wid_of() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
print(next(w['id'] for w in json.load(sys.stdin) if w.get('text')==sys.argv[1]))" "$1"
}

enabled_of() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
print(next(str(w['enabled']).lower() for w in json.load(sys.stdin) if w.get('text')==sys.argv[1]))" "$1"
}

active_of() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
print(next(str(w['active']).lower() for w in json.load(sys.stdin) if w.get('text')==sys.argv[1]))" "$1"
}

canvas_size() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
c=next((w for w in json.load(sys.stdin) if w['type']=='canvas'),None)
print('%s %s' % (c['w'], c['h']) if c else '0 0')"
}

# The ~-abbreviated label the root breadcrumb carries (root_label of the
# fixture path).
root_crumb_label() {
    echo "~${GP_FIXTURE#"$HOME"}"
}

# Wait for: the HTTP server, the initial scan to finish, and the first-layout
# dance to settle (clicks unmap px→viewBox through the live mapping, so a
# click during a transient early allocation lands on the wrong row).
wait_for_app() {
    for _ in $(seq 1 40); do
        curl -sf -o /dev/null "$BASE/widgets" && break
        sleep 0.3
    done
    for _ in $(seq 1 40); do
        status_line | grep -q "Scan complete" && break
        sleep 0.3
    done
    local prev=""
    local cur
    for _ in $(seq 1 20); do
        cur=$(canvas_size)
        [ -n "$prev" ] && [ "$cur" = "$prev" ] && [ "$cur" != "0 0" ] && break
        prev="$cur"
        sleep 0.3
    done
}

results() {
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed ==="
    [ "$FAIL" -eq 0 ]
}
