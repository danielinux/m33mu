#!/bin/sh
# Reproduce linear DC and RC acceptance against a pinned external Perun checkout.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PERUN_DIR=${1:?usage: run-web-perun-linear-acceptance-probe.sh /path/to/pinned-Perun-checkout}
EXPECTED_REV=067d1c9c59e27f72f6498ec224f3735d0da6364a
ACTUAL_REV=$(git -C "$PERUN_DIR" rev-parse HEAD)
if [ "$ACTUAL_REV" != "$EXPECTED_REV" ]; then
    echo "Perun checkout must be at $EXPECTED_REV (got $ACTUAL_REV)" >&2
    exit 1
fi
VITEST="$PERUN_DIR/node_modules/.bin/vitest"
if [ ! -x "$VITEST" ]; then
    echo "Perun checkout dependencies are missing; expected $VITEST" >&2
    exit 1
fi
TMP_DIR=$(mktemp -d)
PROBE_NAME="m33mu-linear-acceptance-$$.test.ts"
PROBE_PATH="$PERUN_DIR/tools/golden/src/$PROBE_NAME"
cleanup() {
    rm -f "$PROBE_PATH"
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT HUP INT TERM
cp "$ROOT/scripts/perun-linear-acceptance-probe.test.ts" "$PROBE_PATH"
export M33MU_PERUN_LINEAR_RESULT="$TMP_DIR/perun-linear-acceptance.json"
cd "$PERUN_DIR"
"$VITEST" run "tools/golden/src/$PROBE_NAME"
cat "$M33MU_PERUN_LINEAR_RESULT"
