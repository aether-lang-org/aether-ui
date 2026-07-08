#!/bin/bash
# grand_perspective: context-menu Copy path + Delete guard/arm/confirm-to-trash.
# The delete REALLY trashes $GP_FIXTURE/mid.bin (gio trash) — always run
# against a throwaway fixture.
# Usage:  GP_FIXTURE=/path ./test_fileops.sh [port]
set -e
source "$(dirname "$0")/helpers.sh"

echo "=== AetherUIDriver: grand_perspective file operations ==="
echo "Target: $BASE   fixture: $GP_FIXTURE"
wait_for_app

echo ""
echo "--- map context menu Copy path (canvas widget route) ---"
CW=$(curl -s "$BASE/widgets" | python3 -c "import json,sys; print(next(w['id'] for w in json.load(sys.stdin) if w['type']=='canvas'))")
curl -s -X POST "$BASE/canvas/1/click?x=100&y=90" > /dev/null; sleep 0.3    # select big.bin
R=$(curl -s -X POST "$BASE/widget/$CW/context_menu/3")                       # item 3 = Copy path
assert_contains "menu activated" '"activated":0' "$R"
sleep 0.3
assert_contains "status shows Copied" "Copied" "$(status_line)"

echo ""
echo "--- Delete guard, arm, confirm — file leaves the disk ---"
DEL=$(wid_of "Delete")
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
# The watch-triggered rescan after the trash cleared the selection; a Delete
# press now must hit the no-selection guard.
curl -s -X POST "$BASE/widget/$DEL/click" > /dev/null; sleep 0.3
assert_contains "guard after trash (no selection)" "Select" "$(status_line)"

results
