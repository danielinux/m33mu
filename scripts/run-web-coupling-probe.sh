#!/bin/sh
# Reproduce the staged firmware GPIO -> Perun RC -> H563 EXTI evidence.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PERUN_DIR=${1:?usage: run-web-coupling-probe.sh /path/to/Perun-checkout}
EXPECTED_REV=067d1c9c59e27f72f6498ec224f3735d0da6364a
PNPM=${PNPM:-pnpm}
ACTUAL_REV=$(git -C "$PERUN_DIR" rev-parse HEAD)
if [ "$ACTUAL_REV" != "$EXPECTED_REV" ]; then
    echo "Perun checkout must be at $EXPECTED_REV (got $ACTUAL_REV)" >&2
    exit 1
fi
TMP_DIR=$(mktemp -d)
PROBE_NAME="m33mu-coupling-probe-codex-$$.test.ts"
PROBE_PATH="$PERUN_DIR/tools/golden/src/$PROBE_NAME"
cleanup() {
    rm -f "$PROBE_PATH"
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT HUP INT TERM

cmake --build "$ROOT/build" -j2 --target m33mu
make -C "$ROOT/tests/firmware/test-stm32h5f4-signal-output-timing" app.elf
make -C "$ROOT/tests/firmware/test-stm32h563-signal-exti-both" app.elf
"$ROOT/build/m33mu" --cpu stm32h5f4 --no-realtime \
    --signal-sync=signal_trigger_marker \
    --signal-record="$TMP_DIR/pa5.csv" --signal-watch:target=PA5 \
    --expect-bkpt 0x7f \
    "$ROOT/tests/firmware/test-stm32h5f4-signal-output-timing/app.elf"
cp "$ROOT/scripts/perun-coupling-probe.test.ts" "$PROBE_PATH"
export M33MU_GPIO_RECORD="$TMP_DIR/pa5.csv"
export M33MU_COSIM_JSON="$TMP_DIR/perun.json"
if [ "${PNPM##*.}" = cjs ]; then
        node "$PNPM" --dir "$PERUN_DIR" exec vitest run "tools/golden/src/$PROBE_NAME"
    else
        "$PNPM" --dir "$PERUN_DIR" exec vitest run "tools/golden/src/$PROBE_NAME"
    fi
python3 "$ROOT/scripts/archive-web-coupling-probe.py" \
    "$TMP_DIR/perun.json" \
    --fixture "$ROOT/web/fixtures/solver/perun-firmware-coupling.json" \
    --sigc "$TMP_DIR/perun-replay.sigc"
python3 "$ROOT/scripts/validate-web-coupling-trace.py"
"$ROOT/build/m33mu" --cpu stm32h563 --no-realtime --vdd-mv=3300 \
    --gpio-vil-mv=1525 --gpio-vih-mv=1775 \
    --signal-file="$TMP_DIR/perun-replay.sigc" \
    --signal-bind:trace=vin:target=PA5 \
    --signal-sync=signal_trigger_marker --expect-bkpt 0x7f \
    "$ROOT/tests/firmware/test-stm32h563-signal-exti-both/app.elf"
