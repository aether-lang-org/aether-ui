#!/bin/bash
# grand_perspective: hover drives the status line; clicks still land after a
# window resize (px→viewBox unmapping through the live meet mapping).
# Usage:  GP_FIXTURE=/path ./test_hover_and_resize.sh [port]
set -e
source "$(dirname "$0")/helpers.sh"

echo "=== AetherUIDriver: grand_perspective hover + resize ==="
echo "Target: $BASE   fixture: $GP_FIXTURE"
wait_for_app

echo ""
echo "--- hover (canvas move) drives the status line ---"
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
echo "--- clicks still land after a window resize ---"
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

results
