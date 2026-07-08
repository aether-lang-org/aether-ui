#!/bin/bash
# run_spec.sh — launcher glue for the Aeocha specs.
# The specs are Aether programs (spec_*.ae, helpers in gp_driver.ae); this
# wrapper only points AETHER_LIB_DIR at the aeocha clone and runs ae from a
# stable cwd. Which spec: $GP_SPEC (ci.sh sets it per iteration) or $1.
# ci.sh's run_server_test passes the port as the first arg — the driver's
# port is fixed at 9222, so a numeric $1 is ignored.
#
# NB: the module-search env var is AETHER_LIB_DIR (aether #413).
#
#   AEOCHA_DIR   where aeocha.ae lives (default ~/scm/aeocha)
set -e
SPEC="${GP_SPEC:-$1}"
DIR="$(cd "$(dirname "$0")" && pwd)"
AEOCHA_DIR="${AEOCHA_DIR:-$HOME/scm/aeocha}"
if [ ! -f "$AEOCHA_DIR/aeocha.ae" ]; then
    echo "  FAIL: aeocha not found at $AEOCHA_DIR (set AEOCHA_DIR, or clone github.com/aether-lang-org/aeocha)"
    exit 1
fi
cd "$DIR"
exec env AETHER_LIB_DIR="$AEOCHA_DIR" ae run "spec_${SPEC}.ae"
