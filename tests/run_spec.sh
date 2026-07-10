#!/bin/bash
# tests/run_spec.sh — launcher glue for ALL the Aeocha driver specs.
# The specs are Aether programs (tests/<app>/spec_*.ae) built on the shared
# tests/lib/uidriver.ae client; this wrapper only wires the module search
# path (aeocha + tests/lib) and runs ae from the spec's directory (same-dir
# modules like gp_driver.ae resolve, and the ae module cache is cwd-keyed).
#
# Which spec: $UI_SPEC as "<app-dir>/<spec-name>" (ci.sh sets it per
# iteration), e.g. UI_SPEC=calculator/spec_calculator — or $1. ci.sh's
# run_server_test passes the port as the first arg; the driver's port is
# fixed at 9222, so a numeric $1 is ignored.
#
# NB: the module-search env var is AETHER_LIB_DIR (aether #413),
# multi-entry with the platform path separator.
#
#   AEOCHA_DIR   where aeocha.ae lives (default ~/scm/aeocha)
set -e
SPEC="${UI_SPEC:-$1}"
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
AEOCHA_DIR="${AEOCHA_DIR:-$HOME/scm/aeocha}"
if [ ! -f "$AEOCHA_DIR/aeocha.ae" ]; then
    echo "  FAIL: aeocha not found at $AEOCHA_DIR (set AEOCHA_DIR, or clone github.com/aether-lang-org/aeocha)"
    exit 1
fi
cd "$TESTS_DIR/$(dirname "$SPEC")"
exec env AETHER_LIB_DIR="$AEOCHA_DIR:$TESTS_DIR/lib" ae run "$(basename "$SPEC").ae"
