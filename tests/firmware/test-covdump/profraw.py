#!/usr/bin/env python3
# m33mu -- an ARMv8-M Emulator
#
# Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
#
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Assemble an llvm .profraw from an ELF plus the regions --covdump wrote.

The emulator deliberately writes no coverage container: .profraw is a versioned
LLVM format, so it is built here, next to the llvm-profdata that reads it.

  ./profraw.py --elf app.elf --prefix ./cov -o cov.profraw
"""
import argparse, struct, subprocess, sys

HDR_FIELDS = 16
DATA_ENTRY = 48          # sizeof(__llvm_profile_data), 32-bit target
CTR = 8
MAGIC, VERSION, VKL = 0xFF6C70726F665281, 10, 2


def pad8(n):
    return (8 - (n % 8)) % 8


def elf_info(elf):
    secs, syms = {}, {}
    out = subprocess.run(['readelf', '-SW', elf], capture_output=True,
                         text=True, check=True).stdout
    for line in out.splitlines():
        if '__llvm_prf_' in line:
            p = line.split()
            i = next(k for k, t in enumerate(p) if t.startswith('__llvm_prf_'))
            secs[p[i]] = (int(p[i + 2], 16), int(p[i + 3], 16), int(p[i + 4], 16))
    out = subprocess.run(['readelf', '-sW', elf], capture_output=True,
                         text=True, check=True).stdout
    for line in out.splitlines():
        if '__start___llvm_prf_' in line or '__stop___llvm_prf_' in line:
            p = line.split()
            syms[p[-1]] = int(p[1], 16)
    return secs, syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--prefix', required=True)
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()

    secs, syms = elf_info(a.elf)
    d_addr, d_off, d_size = secs['__llvm_prf_data']
    n_addr, n_off, n_size = secs['__llvm_prf_names']
    with open(a.elf, 'rb') as f:
        f.seek(d_off); data = f.read(d_size)
        f.seek(n_off); names = f.read(n_size)
    counters = open(a.prefix + '.cnts.bin', 'rb').read()
    try:
        bitmap = open(a.prefix + '.bits.bin', 'rb').read()
    except OSError:
        bitmap = b''

    hdr = struct.pack('<%dQ' % HDR_FIELDS, MAGIC, VERSION, 0,
                      d_size // DATA_ENTRY, pad8(d_size), len(counters) // CTR,
                      pad8(len(counters)), len(bitmap), pad8(len(bitmap)),
                      n_size,
                      syms['__start___llvm_prf_cnts'] - d_addr,
                      syms['__start___llvm_prf_bits'] - d_addr,
                      n_addr, 0, 0, VKL)
    blob = (hdr + data + b'\0' * pad8(d_size)
            + counters + b'\0' * pad8(len(counters))
            + bitmap + b'\0' * pad8(len(bitmap))
            + names + b'\0' * pad8(n_size))
    open(a.out, 'wb').write(blob)
    print(f'{a.out}: {len(blob)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
