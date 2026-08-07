#!/bin/sh
# Flash TrustZone filter parity against silicon.
#
# Runs tests/firmware/test-stm32h563-flash-tz under m33mu with the same
# watermark the reference board carries in its option bytes, and diffs the
# transcript against the one captured from a NUCLEO-H563ZI over its VCP.
#
# The firmware probes marker words either side of the watermark through both
# flash aliases, with SAU off, with SAU on, and with SECBB claiming a
# non-secure sector back.  Any divergence means the emulated flash TZ filter
# no longer matches the part.
#
# Usage: flash-tz-hw-parity.sh <path-to-m33mu>

set -e

M33MU="${1:-./build/m33mu}"
HERE="$(dirname "$0")/.."
FWDIR="$HERE/tests/firmware/test-stm32h563-flash-tz"
APP="$FWDIR/app.bin"
EXPECTED="$FWDIR/nucleo-h563zi.expected.txt"

if [ ! -x "$M33MU" ]; then
    echo "flash-tz-hw-parity: emulator not found at $M33MU" >&2
    exit 1
fi
if [ ! -f "$APP" ]; then
    # The binary is gitignored, so build it when a cross compiler is around.
    if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        make -C "$FWDIR" app.bin >/dev/null 2>&1 || true
    fi
fi
if [ ! -f "$APP" ]; then
    echo "flash-tz-hw-parity: SKIP (no $APP, no arm-none-eabi-gcc)" >&2
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
IMG="$WORK/flashtz.bin"

# 2 MB of erased flash, the firmware at offset 0, and a marker word at the
# start of each probed sector.  Sectors are 8 KB; the marker is 0xa5a500NN
# little-endian where NN is the sector index.
head -c 2097152 /dev/zero | tr '\000' '\377' > "$IMG"
dd if="$APP" of="$IMG" bs=1 conv=notrunc status=none

for sector in 4 20 48 128 254 255; do
    off=$((sector * 8192))
    lo=$(printf '%d' $((sector & 0xff)))
    printf "$(printf '\\%03o\\000\\245\\245' "$lo")" \
        | dd of="$IMG" bs=1 seek="$off" conv=notrunc status=none
done

# SECWM1 = sectors 0..15, SECWM2 = sectors 126..127: the option bytes read
# back from the reference board with STM32_Programmer_CLI -ob displ.
"$M33MU" --cpu stm32h563 --uart-stdout --timeout 60 --expect-bkpt 0x7f \
    --secwm1=0:15 --secwm2=126:127 "$IMG" 2>&1 \
    | sed -n '/FLASHTZ/,/DONE/p' | tr -d '\r' > "$WORK/emu.txt"

# The board's OPTSR_CUR carries option bits (TZEN, boot locks) that the model
# does not claim to reproduce; everything below that line is the filter.
if diff -u \
    "$(grep -v '^OPTSR_CUR=' "$EXPECTED" > "$WORK/exp.f"; echo "$WORK/exp.f")" \
    "$(grep -v '^OPTSR_CUR=' "$WORK/emu.txt" > "$WORK/emu.f"; echo "$WORK/emu.f")"
then
    echo "flash-tz-hw-parity: transcript matches NUCLEO-H563ZI"
else
    echo "flash-tz-hw-parity: FAIL -- emulator diverges from silicon" >&2
    exit 1
fi
