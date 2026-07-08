#!/bin/bash
# run_spec.sh — launcher glue for the Aeocha specs.
# The specs are Aether programs (spec_*.ae, helpers in gp_driver.ae); this
# wrapper only materializes aeocha.ae as a local sibling and runs ae from a
# stable cwd. Which spec: $GP_SPEC (ci.sh sets it per iteration) or $1.
# ci.sh's run_server_test passes the port as the first arg — the driver's
# port is fixed at 9222, so a numeric $1 is ignored.
#
# Why a SYMLINK instead of AETHER_INCLUDE_PATH: with `import aeocha` resolved
# via the include path, adding ANY std import (std.string, std.json, ...) to
# the same file makes aeocha's exports fail to merge (E0301 on aeocha.init).
# Same-directory resolution is immune. Candidate compiler issue, 2026-07-08.
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
ln -sf "$AEOCHA_DIR/aeocha.ae" aeocha.ae
exec ae run "spec_${SPEC}.ae"
