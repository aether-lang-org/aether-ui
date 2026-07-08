#!/bin/bash
# grand_perspective: async scan totals + list-pane selection + Zoom In / ".."
# Usage:  GP_FIXTURE=/path ./test_scan_and_list.sh [port]
set -e
source "$(dirname "$0")/helpers.sh"

echo "=== AetherUIDriver: grand_perspective scan + list ==="
echo "Target: $BASE   fixture: $GP_FIXTURE"
wait_for_app

echo ""
echo "--- async scan completed with correct totals ---"
S=$(status_line)
assert_contains "scan completed" "Scan complete" "$S"
assert_contains "sees 3 files" "3 files" "$S"

echo ""
echo "--- list row click selects with %-of-parent ---"
curl -s -X POST "$BASE/canvas/1/click?x=100&y=90" > /dev/null; sleep 0.4
S=$(status_line)
assert_contains "row 0 selected (big.bin)" "big.bin" "$S"
assert_contains "shows % of parent" "% of parent" "$S"

echo ""
echo "--- Zoom In drills the selected dir; '..' returns ---"
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

results
