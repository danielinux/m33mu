#!/usr/bin/env python3
# m33mu -- an ARMv8-M Emulator
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Generate imxrt700_svd_regs.h (peripheral map + non-zero register reset
# values) from the NXP MIMXRT798S CM33 SVDs (mcux-soc-svd repository).
#
#   python3 cpu/imxrt700/gen_svd_regs.py ../mcux-soc-svd/MIMXRT798S > cpu/imxrt700/imxrt700_svd_regs.h

import os
import re
import sys
import xml.etree.ElementTree as ET


def num(s, default=0):
    if s is None:
        return default
    s = s.strip().lower()
    return int(s, 0)


def expand_dim(node):
    """Yield (name, offset_delta) for a possibly dim'ed register/cluster."""
    dim = node.findtext('dim')
    name = node.findtext('name')
    if dim is None:
        yield name, 0
        return
    inc = num(node.findtext('dimIncrement'))
    for i in range(num(dim)):
        yield name.replace('%s', str(i)), i * inc


# Self-clearing handshake bits: firmware sets them and polls for zero
# (divider REQFLAG, cache GO, PMC/XSPI BUSY, XSPI ABRT_CLR).
SELF_CLEARING_FIELDS = ('REQFLAG', 'GO', 'BUSY', 'ABRT_CLR')


def field_mask(f):
    lsb = f.findtext('bitOffset')
    if lsb is not None:
        return ((1 << num(f.findtext('bitWidth'))) - 1) << num(lsb)
    rng = f.findtext('bitRange')
    if rng is not None:
        hi, lo = [int(x) for x in rng.strip('[]').split(':')]
        return ((1 << (hi - lo + 1)) - 1) << lo
    return ((1 << (num(f.findtext('msb')) - num(f.findtext('lsb')) + 1)) - 1) << num(f.findtext('lsb'))


def collect_regs(container, base_off, out, names):
    for r in container.findall('register'):
        off = num(r.findtext('addressOffset'))
        rst = r.findtext('resetValue')
        rst = num(rst) if rst is not None else 0
        rz = 0
        for f in r.findall('fields/field'):
            if f.findtext('name') in SELF_CLEARING_FIELDS:
                rz |= field_mask(f)
        for rname, d in expand_dim(r):
            out[base_off + off + d] = (rst, rz)
            names[rname] = base_off + off + d
    for c in container.findall('cluster'):
        coff = num(c.findtext('addressOffset'))
        for _, d in expand_dim(c):
            collect_regs(c, base_off + coff + d, out, names)


def load(path):
    root = ET.parse(path).getroot()
    periphs = {}
    order = []
    for p in root.find('peripherals').findall('peripheral'):
        periphs[p.findtext('name')] = p
        order.append(p.findtext('name'))
    devs = {}
    for name in order:
        p = periphs[name]
        base = num(p.findtext('baseAddress'))
        if base >= 0xE0000000:
            continue
        src = p
        derived = p.get('derivedFrom')
        if derived is not None:
            src = periphs[derived]
        regs = {}
        names = {}
        regs_node = src.find('registers')
        if regs_node is not None:
            collect_regs(regs_node, 0, regs, names)
        # X_SET / X_CLR / X_TOG write aliases of register X.
        ops = {}
        for rname, off in names.items():
            m = re.match(r'^(.*)_(SET|CLR|TOG)$', rname)
            if m and m.group(1) in names:
                ops[off] = ({'SET': 1, 'CLR': 2, 'TOG': 3}[m.group(2)], names[m.group(1)])
        ab = p.find('addressBlock')
        if ab is None:
            ab = src.find('addressBlock')
        size = num(ab.findtext('size')) if ab is not None else 0
        size = max(size, (max(regs.keys()) + 4) if regs else 4)
        # Keep each block inside the 4 KB peripheral slot it starts in.
        size = min((size + 3) & ~3, 0x1000 - (base & 0xFFF)) if size <= 0x1000 else (size + 0xFFF) & ~0xFFF
        lo = 0
        if (base & 0xFFF) != 0 and regs:
            # A block nested inside another block's slot (GLIKEY3/4 in
            # SYSCON0/3, GLIKEY0 in AHBSC0, FRO0/2 in CLKCTL0/3) only claims
            # its own registers: the SVD address blocks overlap the host's.
            size = min(size, (max(regs.keys()) + 4 + 0xF) & ~0xF)
            lo = min(regs.keys()) & ~0xF
        alias = None
        if '_ALIAS' in name:
            # GPIOn_ALIAS / AHBSCn_ALIASk mirror their own block, whatever
            # template the SVD derived their register list from.
            alias = re.sub(r'_ALIAS\d*$', '', name)
        irqs = [num(i.findtext('value')) for i in p.findall('interrupt')]
        devs[name] = dict(name=name, base=base & 0x4FFFFFFF if base >= 0x50000000 and base < 0x60000000 else base,
                          size=size, lo=lo, regs=regs, ops=ops, alias=alias, irqs=irqs)
    return devs


def main():
    d = sys.argv[1]
    c0 = load(os.path.join(d, 'MIMXRT798S_cm33_core0.xml'))
    c1 = load(os.path.join(d, 'MIMXRT798S_cm33_core1.xml'))
    merged = {}
    for dom, devs in ((1, c0), (2, c1)):
        for name, dv in devs.items():
            if name in merged:
                m = merged[name]
                if m['base'] != dv['base']:
                    raise SystemExit('base mismatch for %s' % name)
                m['domain'] |= dom
                m['irq%d' % (dom - 1)] = dv['irqs']
                m['regs'].update(dv['regs'])
                m['ops'].update(dv['ops'])
            else:
                dv = dict(dv)
                dv['domain'] = dom
                dv['irq0'] = dv['irqs'] if dom == 1 else []
                dv['irq1'] = dv['irqs'] if dom == 2 else []
                merged[name] = dv
    # Merge blocks that share a base address with a larger block (e.g. FRO1
    # lives inside the CLKCTL0 slot): one state, union of registers.
    by_base = {}
    for dv in sorted(merged.values(), key=lambda x: (x['base'], -x['size'], x['name'])):
        if dv['alias'] is not None:
            continue
        key = dv['base']
        if key in by_base:
            host = by_base[key]
            for off, v in dv['regs'].items():
                host['regs'].setdefault(off, v)
            for off, v in dv['ops'].items():
                host['ops'].setdefault(off, v)
            host['size'] = max(host['size'], dv['size'])
            host['merged'].append(dv['name'])
            dv['merged_into'] = host['name']
        else:
            dv['merged'] = []
            by_base[key] = dv
    devs = sorted(by_base.values(), key=lambda x: x['base'])
    # A block never extends into the next block's slot.
    for i, a in enumerate(devs):
        for b in devs[i + 1:]:
            if (b['base'] & 0xFFF) == 0:
                if a['base'] + a['size'] > b['base']:
                    a['size'] = b['base'] - a['base']
                break
    index = {dv['name']: i for i, dv in enumerate(devs)}
    for dv in merged.values():
        if dv.get('merged_into'):
            index[dv['name']] = index[dv['merged_into']]
    aliases = sorted([dv for dv in merged.values() if dv['alias'] is not None], key=lambda x: x['base'])

    print('/* Generated by cpu/imxrt700/gen_svd_regs.py from MIMXRT798S_cm33_core{0,1}.xml. Do not edit. */')
    print('#ifndef IMXRT700_SVD_REGS_H')
    print('#define IMXRT700_SVD_REGS_H')
    print()
    print('#include "m33mu/types.h"')
    print()
    print('#define IMXRT700_REG_PLAIN 0u')
    print('#define IMXRT700_REG_SET   1u  /* write-1-to-set alias of reg at .target */')
    print('#define IMXRT700_REG_CLR   2u  /* write-1-to-clear alias */')
    print('#define IMXRT700_REG_TOG   3u  /* write-1-to-toggle alias */')
    print()
    print('struct imxrt700_svd_reg {')
    print('    mm_u16 off;')
    print('    mm_u8 kind;')
    print('    mm_u16 target;      /* register a SET/CLR/TOG alias acts on */')
    print('    mm_u32 reset;')
    print('    mm_u32 rz;          /* self-clearing bits, read back as zero */')
    print('};')
    print()
    print('struct imxrt700_svd_dev {')
    print('    const char *name;')
    print('    mm_u32 base;        /* non-secure address */')
    print('    mm_u32 size;')
    print('    mm_u32 lo;          /* first claimed offset (blocks nested in another slot) */')
    print('    mm_u8 domain;       /* bit0: CPU0 (compute) SVD, bit1: CPU1 (sense) SVD */')
    print('    const struct imxrt700_svd_reg *regs; /* registers needing more than reset-to-zero */')
    print('    mm_u16 nregs;')
    print('};')
    print()
    print('struct imxrt700_svd_alias {')
    print('    const char *name;')
    print('    mm_u32 base;')
    print('    mm_u16 dev;         /* index into imxrt700_svd_devs */')
    print('};')
    print()
    for i, dv in enumerate(devs):
        nz = []
        for o, (v, rz) in sorted(dv['regs'].items()):
            kind, target = dv['ops'].get(o, (0, o))
            if o >= 0x10000 or (v == 0 and rz == 0 and kind == 0):
                continue
            nz.append((o, kind, target, v, rz))
        dv['nz'] = nz
        if nz:
            print('static const struct imxrt700_svd_reg imxrt700_svd_regs_%d[] = { /* %s */' % (i, dv['name']))
            for o, kind, target, v, rz in nz:
                print('    { 0x%03Xu, %du, 0x%03Xu, 0x%08Xu, 0x%08Xu },' % (o, kind, target, v, rz))
            print('};')
    print()
    print('#define IMXRT700_SVD_DEV_COUNT %du' % len(devs))
    print('static const struct imxrt700_svd_dev imxrt700_svd_devs[IMXRT700_SVD_DEV_COUNT] = {')
    for i, dv in enumerate(devs):
        regs = ('imxrt700_svd_regs_%d' % i) if dv['nz'] else '0'
        extra = (' /* + %s */' % ', '.join(dv['merged'])) if dv['merged'] else ''
        print('    { "%s", 0x%08Xu, 0x%Xu, 0x%Xu, %d, %s, %du },%s' % (dv['name'], dv['base'], dv['size'], dv['lo'], dv['domain'], regs, len(dv['nz']), extra))
    print('};')
    print()
    print('#define IMXRT700_SVD_ALIAS_COUNT %du' % len(aliases))
    print('static const struct imxrt700_svd_alias imxrt700_svd_aliases[IMXRT700_SVD_ALIAS_COUNT] = {')
    for a in aliases:
        print('    { "%s", 0x%08Xu, %du },' % (a['name'], a['base'], index[a['alias']]))
    print('};')
    print()
    print('#endif /* IMXRT700_SVD_REGS_H */')


if __name__ == '__main__':
    main()
