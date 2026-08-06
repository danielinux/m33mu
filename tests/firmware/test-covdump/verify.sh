#!/bin/sh
# Check a --covdump result against what the firmware is known to do.
#
# Two levels: the structural check needs nothing but readelf, and always runs.
# The coverage check additionally needs llvm-profdata/llvm-cov plus the
# host-side profraw assembler, and is skipped (not failed) when absent.
set -e
prefix="${1:-./cov}"
elf="${2:-app.elf}"

test -s "${prefix}.cnts.bin" || { echo "FAIL: no ${prefix}.cnts.bin"; exit 1; }
test -f "${prefix}.json"     || { echo "FAIL: no ${prefix}.json"; exit 1; }

# strtonum() is a gawk extension, so do the hex arithmetic in the shell.
sym_addr() {
    readelf -sW "$elf" | awk -v want="$1" '$NF == want { print $2; exit }'
}
sym_size() {
    start=$(sym_addr "__start___llvm_prf_$1")
    stop=$(sym_addr "__stop___llvm_prf_$1")
    if [ -z "$start" ] || [ -z "$stop" ]; then echo 0; return; fi
    echo $(( 0x$stop - 0x$start ))
}

for r in cnts bits; do
    want=$(sym_size "$r")
    got=$(wc -c < "${prefix}.${r}.bin")
    if [ "$want" != "$got" ]; then
        echo "FAIL: ${r} region is $got bytes, ELF says $want"
        exit 1
    fi
    echo "ok: ${r} region $got bytes matches the ELF"
done

# The counters must not be all zero: a dump taken before the firmware ran, or
# from the wrong address, would still be the right size.
if od -An -tx1 "${prefix}.cnts.bin" | tr -d ' \n' | grep -qE '^0*$'; then
    echo "FAIL: every counter is zero; the dump did not capture execution"
    exit 1
fi
echo "ok: counters are non-zero"
echo "PASS: --covdump structural check"

# --- optional: full coverage check -------------------------------------------
# Needs llvm-profdata/llvm-cov. Absent tools are a skip, not a failure: the
# structural check above is what proves --covdump itself works.
if ! command -v llvm-profdata >/dev/null 2>&1 || \
   ! command -v llvm-cov >/dev/null 2>&1; then
    echo "skip: llvm-profdata/llvm-cov not found; coverage check not run"
    exit 0
fi

here=$(dirname "$0")
python3 "${here}/profraw.py" --elf "$elf" --prefix "$prefix" \
        -o "${prefix}.profraw" >/dev/null
llvm-profdata merge -sparse "${prefix}.profraw" -o "${prefix}.profdata"
llvm-cov export "$elf" --instr-profile="${prefix}.profdata" \
        > "${prefix}-export.json"

# main.c's single decision has two conditions, and the three calls in main()
# form a complete MC/DC set for it, so this is 2/2 by construction.
mcdc=$(python3 - "$prefix" <<'PY'
import json, sys
d = json.load(open(sys.argv[1] + '-export.json'))
for f in d['data'][0]['files']:
    if f['filename'].endswith('main.c'):
        s = f['summary']['mcdc']
        print(f"{s['covered']}/{s['count']}")
        break
PY
)
if [ "$mcdc" != "2/2" ]; then
    echo "FAIL: main.c MC/DC is ${mcdc}, expected 2/2"
    exit 1
fi
echo "ok: main.c MC/DC ${mcdc}"
echo "PASS: --covdump coverage check"
