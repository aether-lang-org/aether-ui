#!/bin/bash
# AetherUIDriver script for grand_perspective — exercises the three-pane
# disk-usage app end to end over HTTP. The harness (ci.sh) exports
# AEVG_DIR pointing at a fixture this script also knows via $GP_FIXTURE
# (both set by the caller); the app scans it on launch.
#
#   fixture/
#     big.bin   400KB   (list row 0)
#     mid.bin   250KB   (list row 1)
#     sub/      200KB   (one file inside)
#
# Usage:  GP_FIXTURE=/path ./test_grand_perspective.sh [port]

set -e

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

crumbs() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
print(' / '.join(w['text'] for w in json.load(sys.stdin) if w['type']=='button' and w['visible'] and w['text'] not in ('Zoom In','Zoom Out','Rescan','Stop','Open','Reveal','Delete','Confirm delete?','by Size','by Depth','by Type')))"
}

wid_of() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
print(next(w['id'] for w in json.load(sys.stdin) if w.get('text')==sys.argv[1]))" "$1"
}

echo "=== AetherUIDriver: grand_perspective ==="
echo "Target: $BASE   fixture: $GP_FIXTURE"

echo "Waiting for test server + scan..."
for _ in $(seq 1 40); do
    curl -sf -o /dev/null "$BASE/widgets" && break
    sleep 0.3
done
for _ in $(seq 1 40); do
    status_line | grep -q "Scan complete" && break
    sleep 0.3
done
# Wait for the first-layout dance to settle: clicks unmap px→viewBox through
# the live mapping, so a click during a transient early allocation lands on
# the wrong row. Two identical canvas sizes 0.3s apart = settled.
canvas_size() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
c=next((w for w in json.load(sys.stdin) if w['type']=='canvas'),None)
print('%s %s' % (c['w'], c['h']) if c else '0 0')"
}
PREV=""
for _ in $(seq 1 20); do
    CUR=$(canvas_size)
    [ -n "$PREV" ] && [ "$CUR" = "$PREV" ] && [ "$CUR" != "0 0" ] && break
    PREV="$CUR"
    sleep 0.3
done

echo ""
echo "--- Test 1: async scan completed with correct totals ---"
S=$(status_line)
assert_contains "scan completed" "Scan complete" "$S"
assert_contains "sees 3 files" "3 files" "$S"

echo ""
echo "--- Test 2: list row click selects with %-of-parent ---"
curl -s -X POST "$BASE/canvas/1/click?x=100&y=90" > /dev/null; sleep 0.4
S=$(status_line)
assert_contains "row 0 selected (big.bin)" "big.bin" "$S"
assert_contains "shows % of parent" "% of parent" "$S"

echo ""
echo "--- Test 3: Zoom In drills the selected dir; '..' returns ---"
# select the sub dir row (row 2: 400k, 250k, 200k-dir sorted desc)
curl -s -X POST "$BASE/canvas/1/click?x=100&y=140" > /dev/null; sleep 0.3
ZI=$(wid_of "Zoom In")
curl -s -X POST "$BASE/widget/$ZI/click" > /dev/null; sleep 0.4
assert_contains "crumbs show root / sub" "sub" "$(crumbs)"
curl -s -X POST "$BASE/canvas/1/click?x=100&y=68" > /dev/null; sleep 0.4   # ".." row
C=$(crumbs)
if echo "$C" | grep -q "sub"; then
    echo "  FAIL: '..' did not return (crumbs: $C)"; FAIL=$((FAIL+1))
else
    echo "  PASS: '..' returned to root"; PASS=$((PASS+1))
fi

echo ""
echo "--- Test 4: legend row click toggles a type highlight ---"
curl -s -X POST "$BASE/canvas/1/click?x=1100&y=65" > /dev/null; sleep 0.4
assert_contains "highlight on" "Highlighting" "$(status_line)"
curl -s -X POST "$BASE/canvas/1/click?x=1100&y=65" > /dev/null; sleep 0.4
assert_contains "highlight off" "Highlight off" "$(status_line)"

echo ""
echo "--- Test 5: map context menu Copy path (canvas widget route) ---"
CW=$(curl -s "$BASE/widgets" | python3 -c "import json,sys; print(next(w['id'] for w in json.load(sys.stdin) if w['type']=='canvas'))")
curl -s -X POST "$BASE/canvas/1/click?x=100&y=90" > /dev/null; sleep 0.3    # select big.bin
R=$(curl -s -X POST "$BASE/widget/$CW/context_menu/3")                       # item 3 = Copy path
assert_contains "menu activated" '"activated":0' "$R"
sleep 0.3
assert_contains "status shows Copied" "Copied" "$(status_line)"

echo ""
echo "--- Test 6: Delete guard, arm, confirm — file leaves the disk ---"
DEL=$(wid_of "Delete")
# clear selection via Escape-equivalent: select then delete flow needs selection;
# first prove the guard with no selection (fresh select cleared by nav above? select then clear via legend off leaves selection... use guard by deleting AFTER trash removes selection)
curl -s -X POST "$BASE/canvas/1/click?x=100&y=115" > /dev/null; sleep 0.3   # select mid.bin (row 1)
curl -s -X POST "$BASE/widget/$DEL/click" > /dev/null; sleep 0.3
BTN=$(curl -s "$BASE/widgets" | python3 -c "import json,sys; print(next(w['text'] for w in json.load(sys.stdin) if w['id']==$DEL))")
assert_contains "armed: button relabelled" "Confirm" "$BTN"
curl -s -X POST "$BASE/widget/$DEL/click" > /dev/null; sleep 1.5
if [ -f "$GP_FIXTURE/mid.bin" ]; then
    echo "  FAIL: mid.bin still on disk"; FAIL=$((FAIL+1))
else
    echo "  PASS: mid.bin moved to trash"; PASS=$((PASS+1))
fi
curl -s -X POST "$BASE/widget/$DEL/click" > /dev/null; sleep 0.3
assert_contains "guard after trash (no selection)" "Select" "$(status_line)"

echo ""
echo "--- Test 7: double-click a map tile drills; crumb click returns ---"
# sub's tile sits in the lower-right of the map at the default 1356x600
# canvas (fixture: big.bin left half, mid.bin top right, sub bottom right).
curl -s -X POST "$BASE/canvas/1/click?x=860&y=450" > /dev/null; sleep 0.1
curl -s -X POST "$BASE/canvas/1/click?x=860&y=450" > /dev/null; sleep 0.4
assert_contains "double-click drilled into sub" "sub" "$(crumbs)"
# The root crumb is labelled with the ~-abbreviated scan path, not "root".
ROOTLBL="~${GP_FIXTURE#"$HOME"}"
ROOTB=$(wid_of "$ROOTLBL")
curl -s -X POST "$BASE/widget/$ROOTB/click" > /dev/null; sleep 0.4
C=$(crumbs)
if echo "$C" | grep -q "sub"; then
    echo "  FAIL: root crumb did not return (crumbs: $C)"; FAIL=$((FAIL+1))
else
    echo "  PASS: root crumb returned to root"; PASS=$((PASS+1))
fi

echo ""
echo "--- Test 8: keyboard nav — Down selects, Right drills, Left returns ---"
curl -s -X POST "$BASE/canvas/1/key?name=Escape" > /dev/null; sleep 0.2
curl -s -X POST "$BASE/canvas/1/key?name=Down" > /dev/null; sleep 0.3
assert_contains "Down selects row 0" "big.bin" "$(status_line)"
curl -s -X POST "$BASE/canvas/1/key?name=Down" > /dev/null; sleep 0.2
curl -s -X POST "$BASE/canvas/1/key?name=Down" > /dev/null; sleep 0.3
assert_contains "Down x3 reaches the dir row" "sub" "$(status_line)"
curl -s -X POST "$BASE/canvas/1/key?name=Right" > /dev/null; sleep 0.4
assert_contains "Right drills the selected dir" "sub" "$(crumbs)"
curl -s -X POST "$BASE/canvas/1/key?name=Left" > /dev/null; sleep 0.4
C=$(crumbs)
if echo "$C" | grep -q "sub"; then
    echo "  FAIL: Left did not go up (crumbs: $C)"; FAIL=$((FAIL+1))
else
    echo "  PASS: Left went back up"; PASS=$((PASS+1))
fi
curl -s -X POST "$BASE/canvas/1/key?name=Escape" > /dev/null; sleep 0.2

echo ""
echo "--- Test 9: hover (canvas move) drives the status line ---"
curl -s -X POST "$BASE/canvas/1/move?x=400&y=300" > /dev/null; sleep 0.3
assert_contains "hover over big.bin names it" "big.bin" "$(status_line)"
curl -s -X POST "$BASE/canvas/1/move?x=1300&y=550" > /dev/null; sleep 0.3
S=$(status_line)
if [ "$S" = "—" ]; then
    echo "  PASS: hover off-map clears to em-dash"; PASS=$((PASS+1))
else
    echo "  FAIL: hover off-map (expected '—', got '$S')"; FAIL=$((FAIL+1))
fi

echo ""
echo "--- Test 10: scheme buttons act as a radio group (active = ghosted) ---"
enabled_of() {
    curl -s "$BASE/widgets" | python3 -c "
import json,sys
print(next(str(w['enabled']).lower() for w in json.load(sys.stdin) if w.get('text')==sys.argv[1]))" "$1"
}
assert_contains "by Type ghosted initially (the default scheme)" "false" "$(enabled_of 'by Type')"
BD=$(wid_of "by Depth")
curl -s -X POST "$BASE/widget/$BD/click" > /dev/null; sleep 0.3
assert_contains "by Depth ghosted after click" "false" "$(enabled_of 'by Depth')"
assert_contains "by Type re-enabled" "true" "$(enabled_of 'by Type')"
BT=$(wid_of "by Type")
curl -s -X POST "$BASE/widget/$BT/click" > /dev/null; sleep 0.3   # restore default
assert_contains "Stop ghosted when idle" "false" "$(enabled_of 'Stop')"

echo ""
echo "--- Test 11: clicks still land after a window resize ---"
# Grow the window, then double-click sub's tile at its NEW pixel position,
# computed through the same xMidYMid-meet mapping the scene uses. Before the
# px→viewBox unmapping fix this hit the wrong pane and nothing drilled.
curl -s -X POST "$BASE/window/resize?w=1716&h=830" > /dev/null; sleep 1.0
XY=$(curl -s "$BASE/widgets" | python3 -c "
import json,sys
ws=json.load(sys.stdin)
c=next(w for w in ws if w['type']=='canvas')
cw,ch=c['w'],c['h']
s=min(cw/1356.0, ch/600.0)
ox,oy=(cw-1356.0*s)/2.0,(ch-600.0*s)/2.0
print('%.0f %.0f %d %d' % (860*s+ox, 450*s+oy, cw, ch))")
PX=$(echo "$XY" | cut -d' ' -f1); PY=$(echo "$XY" | cut -d' ' -f2)
echo "  (canvas now $(echo "$XY" | cut -d' ' -f3)x$(echo "$XY" | cut -d' ' -f4); sub tile at px $PX,$PY)"
curl -s -X POST "$BASE/canvas/1/click?x=$PX&y=$PY" > /dev/null; sleep 0.1
curl -s -X POST "$BASE/canvas/1/click?x=$PX&y=$PY" > /dev/null; sleep 0.4
assert_contains "double-click drills at the new scale" "sub" "$(crumbs)"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
