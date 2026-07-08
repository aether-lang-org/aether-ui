#!/bin/bash
# grand_perspective: map double-click drill + breadcrumb return + keyboard nav.
# Usage:  GP_FIXTURE=/path ./test_map_nav.sh [port]
set -e
source "$(dirname "$0")/helpers.sh"

echo "=== AetherUIDriver: grand_perspective map navigation ==="
echo "Target: $BASE   fixture: $GP_FIXTURE"
wait_for_app

echo ""
echo "--- double-click a map tile drills; crumb click returns ---"
# sub's tile sits in the lower-right of the map at the default 1356x600
# canvas (fixture: big.bin left half, mid.bin top right, sub bottom right).
curl -s -X POST "$BASE/canvas/1/click?x=860&y=450" > /dev/null; sleep 0.1
curl -s -X POST "$BASE/canvas/1/click?x=860&y=450" > /dev/null; sleep 0.4
assert_contains "double-click drilled into sub" "sub" "$(crumbs)"
ROOTB=$(wid_of "$(root_crumb_label)")
curl -s -X POST "$BASE/widget/$ROOTB/click" > /dev/null; sleep 0.4
C=$(crumbs)
if echo "$C" | grep -q "sub"; then
    echo "  FAIL: root crumb did not return (crumbs: $C)"; FAIL=$((FAIL+1))
else
    echo "  PASS: root crumb returned to root"; PASS=$((PASS+1))
fi

echo ""
echo "--- keyboard nav: Down selects, Right drills, Left returns ---"
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

results
