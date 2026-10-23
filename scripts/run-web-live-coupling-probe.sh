#!/bin/sh
# Reproduce instruction-boundary H563 WASM <-> Perun RC feedback outside the repo.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PERUN_DIR=${1:?usage: run-web-live-coupling-probe.sh /path/to/pinned-Perun-checkout}
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
PROBE_NAME="m33mu-live-coupling-$$.test.ts"
PROBE_PATH="$PERUN_DIR/tools/golden/src/$PROBE_NAME"
cleanup() {
    rm -f "$PROBE_PATH"
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT HUP INT TERM
make -C "$ROOT/tests/firmware/test-stm32h563-signal-analog-loop" clean app.elf
EM_CACHE="$TMP_DIR/emscripten-cache" emcmake cmake -S "$ROOT/cmake/wasm" -B "$TMP_DIR/build" -DCMAKE_BUILD_TYPE=Release
EM_CACHE="$TMP_DIR/emscripten-cache" cmake --build "$TMP_DIR/build" --target m33mu_web_api -j2
cp "$ROOT/scripts/live-m33mu-coupling-probe.test.ts" "$PROBE_PATH"
export M33MU_WASM_MODULE="$TMP_DIR/build/m33mu_web_api.js"
export M33MU_FIRMWARE="$ROOT/tests/firmware/test-stm32h563-signal-analog-loop/app.elf"
export M33MU_LIVE_RESULT="$TMP_DIR/live-coupling.json"
export XDG_DATA_HOME="$TMP_DIR/user-data"
cd "$PERUN_DIR"
"$VITEST" run "tools/golden/src/$PROBE_NAME"
cp "$TMP_DIR/live-coupling.json" "$ROOT/web/fixtures/solver/perun-live-coupling.json"
printf 'Wrote %s\n' "$ROOT/web/fixtures/solver/perun-live-coupling.json"
