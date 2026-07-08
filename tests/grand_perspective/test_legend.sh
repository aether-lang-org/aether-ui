#!/bin/bash
# grand_perspective: type-legend highlight toggle + colour-scheme radio group
# + scan-lifecycle button ghosting.
# Usage:  GP_FIXTURE=/path ./test_legend.sh [port]
set -e
source "$(dirname "$0")/helpers.sh"

echo "=== AetherUIDriver: grand_perspective legend + schemes ==="
echo "Target: $BASE   fixture: $GP_FIXTURE"
wait_for_app

echo ""
echo "--- legend row click toggles a type highlight ---"
curl -s -X POST "$BASE/canvas/1/click?x=1100&y=65" > /dev/null; sleep 0.4
assert_contains "highlight on" "Highlighting" "$(status_line)"
curl -s -X POST "$BASE/canvas/1/click?x=1100&y=65" > /dev/null; sleep 0.4
assert_contains "highlight off" "Highlight off" "$(status_line)"

echo ""
echo "--- colour-scheme radio group (grouped toggles) ---"
assert_contains "by Type active initially (the default scheme)" "true" "$(active_of 'by Type')"
BD=$(wid_of "by Depth")
curl -s -X POST "$BASE/widget/$BD/toggle" > /dev/null; sleep 0.3
assert_contains "by Depth active after toggle" "true" "$(active_of 'by Depth')"
assert_contains "by Type deactivated by the group" "false" "$(active_of 'by Type')"
BT=$(wid_of "by Type")
curl -s -X POST "$BASE/widget/$BT/toggle" > /dev/null; sleep 0.3   # restore default
assert_contains "by Type active again" "true" "$(active_of 'by Type')"

echo ""
echo "--- scan-lifecycle ghosting ---"
assert_contains "Stop ghosted when idle" "false" "$(enabled_of 'Stop')"
assert_contains "Rescan live when idle" "true" "$(enabled_of 'Rescan')"

results
