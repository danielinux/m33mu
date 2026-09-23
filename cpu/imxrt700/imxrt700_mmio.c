/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imxrt700/imxrt700_mmio.h"
#include "imxrt700/imxrt700_svd_regs.h"
#include "imxrt700/imxrt700_flexcomm.h"
#include "imxrt700/imxrt700_timers.h"
#include "imxrt700/imxrt700_multicore.h"
#include "imxrt700/imxrt700_secure.h"
#include "imxrt700/imxrt700_romapi.h"
#include "imxrt700/cpu_config.h"

#define PERIPH_WINDOW_SIZE 0x10000000u
#define PAGE_SHIFT 12u
#define PAGE_COUNT (PERIPH_WINDOW_SIZE >> PAGE_SHIFT)
#define PAGE_SLOTS 2u
#define NO_DEV 0xFFFFu

#define DEV_TOTAL (IMXRT700_SVD_DEV_COUNT + IMXRT700_SVD_ALIAS_COUNT)

/* devs[0 .. IMXRT700_SVD_DEV_COUNT-1] own storage; the alias entries after
 * them (GPIOn_ALIAS, AHBSCn_ALIASk, ...) share their parent's storage. */
static struct imxrt700_dev devs[DEV_TOTAL];
static mm_u16 dev_parent[DEV_TOTAL];
static mm_u16 page_map[PAGE_COUNT][PAGE_SLOTS];
static mm_bool devs_ready = MM_FALSE;
static mm_bool access_secure = MM_FALSE;
static struct mm_nvic *nvics[2];
static struct mm_memmap *bound_map;
static mm_u8 *bound_flash;
static mm_u32 bound_flash_size;
static mm_bool unmapped_warned;

static const mm_u8 window_ns = 0u;
static const mm_u8 window_s = 1u;

static mm_bool trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("M33MU_IMXRT700_TRACE") != 0 ? 1 : 0;
    }
    return cached != 0;
}

/* ------------------------------------------------------------------------ */
/* Device table                                                             */
/* ------------------------------------------------------------------------ */

static mm_u16 dev_index_by_name(const char *name)
{
    mm_u16 i;
    for (i = 0; i < DEV_TOTAL; ++i) {
        if (devs[i].name != 0 && strcmp(devs[i].name, name) == 0) {
            return i;
        }
    }
    return NO_DEV;
}

static void page_add(mm_u32 base, mm_u32 size, mm_u16 idx)
{
    mm_u32 first = (base - IMXRT700_PERIPH_BASE_NS) >> PAGE_SHIFT;
    mm_u32 last = (base + size - 1u - IMXRT700_PERIPH_BASE_NS) >> PAGE_SHIFT;
    mm_u32 p;
    for (p = first; p <= last && p < PAGE_COUNT; ++p) {
        mm_u32 s;
        for (s = 0; s < PAGE_SLOTS; ++s) {
            if (page_map[p][s] == NO_DEV) {
                page_map[p][s] = idx;
                break;
            }
        }
        if (s == PAGE_SLOTS) {
            fprintf(stderr, "[IMXRT700] page 0x%08lx over-subscribed (%s)\n",
                    (unsigned long)(IMXRT700_PERIPH_BASE_NS + (p << PAGE_SHIFT)), devs[idx].name);
        }
    }
}

static void devs_build(void)
{
    mm_u32 i;
    mm_u32 total = 0;
    mm_u8 *pool;

    if (devs_ready) {
        return;
    }
    memset(page_map, 0xFF, sizeof(page_map));
    for (i = 0; i < IMXRT700_SVD_DEV_COUNT; ++i) {
        total += (imxrt700_svd_devs[i].size + 3u) & ~3u;
    }
    pool = (mm_u8 *)calloc(1u, total);
    if (pool == 0) {
        fprintf(stderr, "[IMXRT700] out of memory for register files\n");
        return;
    }
    for (i = 0; i < IMXRT700_SVD_DEV_COUNT; ++i) {
        const struct imxrt700_svd_dev *s = &imxrt700_svd_devs[i];
        devs[i].name = s->name;
        devs[i].base = s->base;
        devs[i].size = (s->size + 3u) & ~3u;
        devs[i].lo = s->lo;
        devs[i].domain = s->domain;
        devs[i].mem = pool;
        devs[i].svd = s;
        dev_parent[i] = (mm_u16)i;
        pool += devs[i].size;
        page_add(devs[i].base, devs[i].size, (mm_u16)i);
    }
    for (i = 0; i < IMXRT700_SVD_ALIAS_COUNT; ++i) {
        const struct imxrt700_svd_alias *a = &imxrt700_svd_aliases[i];
        mm_u32 idx = IMXRT700_SVD_DEV_COUNT + i;
        const struct imxrt700_dev *parent = &devs[a->dev];
        mm_u32 k;
        mm_bool dup = MM_FALSE;
        /* PUF_ALIASn and PUF_CTRL_ALIASn name the same slot. */
        for (k = IMXRT700_SVD_DEV_COUNT; k < idx; ++k) {
            if (devs[k].base == a->base) {
                dup = MM_TRUE;
            }
        }
        devs[idx].name = a->name;
        devs[idx].base = a->base;
        devs[idx].size = parent->size;
        devs[idx].lo = parent->lo;
        devs[idx].domain = parent->domain;
        devs[idx].mem = parent->mem;
        devs[idx].svd = parent->svd;
        dev_parent[idx] = a->dev;
        if (!dup) {
            page_add(devs[idx].base, devs[idx].size, (mm_u16)idx);
        }
    }
    devs_ready = MM_TRUE;
}

static struct imxrt700_dev *dev_lookup(mm_u32 ns_addr, mm_u32 *off_out, struct imxrt700_dev **alias_out)
{
    mm_u32 page = (ns_addr - IMXRT700_PERIPH_BASE_NS) >> PAGE_SHIFT;
    struct imxrt700_dev *best = 0;
    mm_u16 best_idx = NO_DEV;
    mm_u32 s;
    if (page >= PAGE_COUNT) {
        return 0;
    }
    for (s = 0; s < PAGE_SLOTS; ++s) {
        mm_u16 idx = page_map[page][s];
        struct imxrt700_dev *d;
        if (idx == NO_DEV) {
            continue;
        }
        d = &devs[idx];
        if (ns_addr < d->base + d->lo || ns_addr - d->base >= d->size) {
            continue;
        }
        if (best == 0 || d->size < best->size) {
            best = d;
            best_idx = idx;
        }
    }
    if (best == 0) {
        return 0;
    }
    *off_out = ns_addr - best->base;
    if (alias_out != 0) {
        *alias_out = best;
    }
    return &devs[dev_parent[best_idx]];
}

struct imxrt700_dev *mm_imxrt700_dev(const char *name)
{
    mm_u16 idx;
    devs_build();
    idx = dev_index_by_name(name);
    if (idx == NO_DEV) {
        return 0;
    }
    return &devs[dev_parent[idx]];
}

mm_bool mm_imxrt700_dev_hook(const char *name,
                             imxrt700_read_hook read,
                             imxrt700_write_hook write,
                             void *opaque)
{
    struct imxrt700_dev *d = mm_imxrt700_dev(name);
    if (d == 0) {
        fprintf(stderr, "[IMXRT700] no SVD block named %s\n", name);
        return MM_FALSE;
    }
    d->read = read;
    d->write = write;
    d->opaque = opaque;
    return MM_TRUE;
}

/* ------------------------------------------------------------------------ */
/* Register file                                                            */
/* ------------------------------------------------------------------------ */

static const struct imxrt700_svd_reg *svd_reg(const struct imxrt700_dev *dev, mm_u32 off)
{
    const struct imxrt700_svd_dev *s = (const struct imxrt700_svd_dev *)dev->svd;
    mm_u32 i;
    if (s == 0 || s->regs == 0) {
        return 0;
    }
    for (i = 0; i < s->nregs; ++i) {
        if (s->regs[i].off == off) {
            return &s->regs[i];
        }
        if (s->regs[i].off > off) {
            break;
        }
    }
    return 0;
}

mm_u32 mm_imxrt700_reg(const struct imxrt700_dev *dev, mm_u32 off)
{
    const mm_u8 *p;
    if (dev == 0 || dev->mem == 0 || off + 4u > dev->size) {
        return 0;
    }
    p = dev->mem + off;
    return (mm_u32)p[0] | ((mm_u32)p[1] << 8) | ((mm_u32)p[2] << 16) | ((mm_u32)p[3] << 24);
}

void mm_imxrt700_reg_put(struct imxrt700_dev *dev, mm_u32 off, mm_u32 value)
{
    mm_u8 *p;
    if (dev == 0 || dev->mem == 0 || off + 4u > dev->size) {
        return;
    }
    p = dev->mem + off;
    p[0] = (mm_u8)value;
    p[1] = (mm_u8)(value >> 8);
    p[2] = (mm_u8)(value >> 16);
    p[3] = (mm_u8)(value >> 24);
}

mm_bool mm_imxrt700_regfile_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    mm_u32 word_off = off & ~3u;
    mm_u32 shift = (off & 3u) * 8u;
    mm_u32 v;
    const struct imxrt700_svd_reg *r;
    if (dev == 0 || value_out == 0 || off + size > dev->size) {
        return MM_FALSE;
    }
    r = svd_reg(dev, word_off);
    if (r != 0 && r->kind != IMXRT700_REG_PLAIN) {
        v = 0u; /* SET/CLR/TOG aliases are write-only */
    } else {
        v = mm_imxrt700_reg(dev, word_off);
        if (r != 0) {
            v &= ~r->rz;
        }
    }
    v >>= shift;
    if (size == 1u) {
        v &= 0xFFu;
    } else if (size == 2u) {
        v &= 0xFFFFu;
    }
    *value_out = v;
    return MM_TRUE;
}

mm_bool mm_imxrt700_regfile_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    mm_u32 word_off = off & ~3u;
    mm_u32 shift = (off & 3u) * 8u;
    mm_u32 mask;
    mm_u32 cur;
    const struct imxrt700_svd_reg *r;
    if (dev == 0 || off + size > dev->size) {
        return MM_FALSE;
    }
    mask = (size == 4u) ? 0xFFFFFFFFu : (((1u << (size * 8u)) - 1u) << shift);
    value = (value << shift) & mask;
    r = svd_reg(dev, word_off);
    if (r != 0 && r->kind != IMXRT700_REG_PLAIN) {
        cur = mm_imxrt700_reg(dev, r->target);
        if (r->kind == IMXRT700_REG_SET) {
            cur |= value;
        } else if (r->kind == IMXRT700_REG_CLR) {
            cur &= ~value;
        } else {
            cur ^= value;
        }
        mm_imxrt700_reg_put(dev, r->target, cur);
        return MM_TRUE;
    }
    cur = mm_imxrt700_reg(dev, word_off);
    cur = (cur & ~mask) | value;
    if (r != 0) {
        cur &= ~r->rz;
    }
    mm_imxrt700_reg_put(dev, word_off, cur);
    return MM_TRUE;
}

static void devs_reset(void)
{
    mm_u32 i;
    devs_build();
    for (i = 0; i < IMXRT700_SVD_DEV_COUNT; ++i) {
        const struct imxrt700_svd_dev *s = &imxrt700_svd_devs[i];
        mm_u32 k;
        if (devs[i].mem == 0) {
            continue;
        }
        memset(devs[i].mem, 0, devs[i].size);
        for (k = 0; k < s->nregs; ++k) {
            if (s->regs[k].kind == IMXRT700_REG_PLAIN) {
                mm_imxrt700_reg_put(&devs[i], s->regs[k].off, s->regs[k].reset);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Clock / reset / power: status bits firmware polls                        */
/* ------------------------------------------------------------------------ */

struct force_bits {
    const char *dev;
    mm_u32 off;
    mm_u32 set;
};

/* Oscillators and PLLs are modelled as always settled. */
static const struct force_bits ready_bits[] = {
    { "CLKCTL0", 0x118u, 0x00000003u },  /* FRO01CLKSTATUS: FRO0/FRO1 CLK_OK */
    { "CLKCTL0", 0x200u, 0x03000000u },  /* FRO1 CSR: TRIM_LOCK | TUNEONCE_DONE */
    { "FRO0", 0x200u, 0x03000000u },
    { "FRO2", 0x200u, 0x03000000u },
    { "CLKCTL3", 0x290u, 0x00000001u },  /* FRO2CLKSTATUS: CLK_OK */
    { "CLKCTL2", 0x218u, 0x40404040u },  /* MAINPLL0PFD: PFDn_CLKRDY */
    { "CLKCTL2", 0x418u, 0x40404040u },  /* AUDIOPLL0PFD: PFDn_CLKRDY */
    { "OSC32KNP", 0x008u, 0x00000003u }, /* STAT: TCXO/SCXO_STABLE */
};

static mm_bool status_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    mm_u32 i;
    if (!mm_imxrt700_regfile_read(dev, off, size, value_out)) {
        return MM_FALSE;
    }
    for (i = 0; i < sizeof(ready_bits) / sizeof(ready_bits[0]); ++i) {
        if (strcmp(ready_bits[i].dev, dev->name) == 0 && ready_bits[i].off == (off & ~3u)) {
            *value_out |= ready_bits[i].set >> ((off & 3u) * 8u);
        }
    }
    return MM_TRUE;
}

mm_bool mm_imxrt700_clock_on(const char *clkctl, mm_u32 pscctl_off, mm_u32 bit)
{
    struct imxrt700_dev *d = mm_imxrt700_dev(clkctl);
    return (d != 0 && (mm_imxrt700_reg(d, pscctl_off) & (1u << bit)) != 0u) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_imxrt700_reset_released(const char *rstctl, mm_u32 prstctl_off, mm_u32 bit)
{
    struct imxrt700_dev *d = mm_imxrt700_dev(rstctl);
    return (d != 0 && (mm_imxrt700_reg(d, prstctl_off) & (1u << bit)) == 0u) ? MM_TRUE : MM_FALSE;
}

/* ------------------------------------------------------------------------ */
/* RGPIO                                                                    */
/* ------------------------------------------------------------------------ */

#define RGPIO_PDOR 0x40u
#define RGPIO_PSOR 0x44u
#define RGPIO_PCOR 0x48u
#define RGPIO_PTOR 0x4Cu
#define RGPIO_PDIR 0x50u
#define RGPIO_PDDR 0x54u

static mm_bool gpio_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    if (off == RGPIO_PDIR && size == 4u) {
        /* No external stimulus: outputs read back what they drive. */
        *value_out = mm_imxrt700_reg(dev, RGPIO_PDOR) & mm_imxrt700_reg(dev, RGPIO_PDDR);
        return MM_TRUE;
    }
    if ((off == RGPIO_PSOR || off == RGPIO_PCOR || off == RGPIO_PTOR) && size == 4u) {
        *value_out = 0u;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool gpio_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    mm_u32 pdor = mm_imxrt700_reg(dev, RGPIO_PDOR);
    if (size != 4u) {
        return MM_FALSE;
    }
    switch (off) {
    case RGPIO_PSOR:
        pdor |= value;
        break;
    case RGPIO_PCOR:
        pdor &= ~value;
        break;
    case RGPIO_PTOR:
        pdor ^= value;
        break;
    case RGPIO_PDIR:
        return MM_TRUE; /* read-only */
    default:
        return MM_FALSE;
    }
    mm_imxrt700_reg_put(dev, RGPIO_PDOR, pdor);
    return MM_TRUE;
}

/* ------------------------------------------------------------------------ */
/* CRC (DATA / GPOLY / CTRL engine)                                         */
/* ------------------------------------------------------------------------ */

#define CRC_DATA 0x0u
#define CRC_GPOLY 0x4u
#define CRC_CTRL 0x8u
#define CRC_CTRL_TCRC (1u << 24)
#define CRC_CTRL_WAS (1u << 25)
#define CRC_CTRL_FXOR (1u << 26)

static mm_u32 crc_state;

static mm_u32 bitrev(mm_u32 v, mm_u32 bits)
{
    mm_u32 r = 0;
    mm_u32 i;
    for (i = 0; i < bits; ++i) {
        r = (r << 1) | ((v >> i) & 1u);
    }
    return r;
}

static mm_u32 crc_transpose(mm_u32 v, mm_u32 bytes, mm_u32 type)
{
    mm_u32 out = 0;
    mm_u32 i;
    switch (type & 3u) {
    case 1u: /* bits within bytes */
        for (i = 0; i < bytes; ++i) {
            out |= bitrev((v >> (i * 8u)) & 0xFFu, 8u) << (i * 8u);
        }
        return out;
    case 2u: /* bits and bytes */
        return bitrev(v, bytes * 8u);
    case 3u: /* bytes only */
        for (i = 0; i < bytes; ++i) {
            out |= ((v >> (i * 8u)) & 0xFFu) << ((bytes - 1u - i) * 8u);
        }
        return out;
    default:
        return v;
    }
}

static mm_bool crc_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    mm_u32 ctrl = mm_imxrt700_reg(dev, CRC_CTRL);
    mm_u32 v;
    if (off >= 4u) {
        return MM_FALSE;
    }
    v = crc_transpose(crc_state, 4u, ctrl >> 28);
    if (ctrl & CRC_CTRL_FXOR) {
        v ^= (ctrl & CRC_CTRL_TCRC) ? 0xFFFFFFFFu : 0xFFFFu;
    }
    v >>= off * 8u;
    if (size == 1u) v &= 0xFFu;
    if (size == 2u) v &= 0xFFFFu;
    *value_out = v;
    return MM_TRUE;
}

static mm_bool crc_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    mm_u32 ctrl = mm_imxrt700_reg(dev, CRC_CTRL);
    mm_bool wide = (ctrl & CRC_CTRL_TCRC) ? MM_TRUE : MM_FALSE;
    mm_u32 poly = mm_imxrt700_reg(dev, CRC_GPOLY);
    mm_u32 width = wide ? 32u : 16u;
    mm_u32 topbit = 1u << (width - 1u);
    mm_u32 wmask = wide ? 0xFFFFFFFFu : 0xFFFFu;
    mm_u32 data;
    mm_u32 bits;
    mm_u32 i;
    if (off >= 4u) {
        return MM_FALSE;
    }
    if (ctrl & CRC_CTRL_WAS) {
        mm_u32 mask = (size == 4u) ? 0xFFFFFFFFu : (((1u << (size * 8u)) - 1u) << (off * 8u));
        crc_state = (crc_state & ~mask) | ((value << (off * 8u)) & mask);
        return MM_TRUE;
    }
    data = crc_transpose(value, size, (ctrl >> 30) & 3u);
    bits = size * 8u;
    poly &= wmask;
    for (i = 0; i < bits; ++i) {
        mm_u32 in = (data >> (bits - 1u - i)) & 1u;
        mm_u32 top = ((crc_state & topbit) ? 1u : 0u) ^ in;
        crc_state = (crc_state << 1) & wmask;
        if (top) {
            crc_state ^= poly;
        }
    }
    return MM_TRUE;
}

/* ------------------------------------------------------------------------ */
/* Dispatcher                                                               */
/* ------------------------------------------------------------------------ */

mm_bool mm_imxrt700_access_secure(void)
{
    return access_secure;
}

static mm_bool window_read(void *opaque, mm_u32 offset, mm_u32 size, mm_u32 *value_out)
{
    const mm_u8 *win = (const mm_u8 *)opaque;
    mm_u32 ns_addr = IMXRT700_PERIPH_BASE_NS + offset;
    mm_u32 off = 0;
    struct imxrt700_dev *alias = 0;
    struct imxrt700_dev *dev;
    mm_bool ok;

    if (value_out == 0 || size == 0u || size > 4u) {
        return MM_FALSE;
    }
    dev = dev_lookup(ns_addr, &off, &alias);
    access_secure = (*win == window_s) ? MM_TRUE : MM_FALSE;
    if (dev == 0) {
        if (!unmapped_warned || trace_enabled()) {
            fprintf(stderr, "[IMXRT700] read from unmapped peripheral address 0x%08lx (RAZ)\n",
                    (unsigned long)(ns_addr | (access_secure ? 0x10000000u : 0u)));
            unmapped_warned = MM_TRUE;
        }
        *value_out = 0u;
        return MM_TRUE;
    }
    (void)alias;
    ok = MM_FALSE;
    if (dev->read != 0) {
        ok = dev->read(dev, off, size, value_out);
    }
    if (!ok) {
        ok = mm_imxrt700_regfile_read(dev, off, size, value_out);
    }
    if (trace_enabled()) {
        fprintf(stderr, "[IMXRT700] rd %s+0x%03lx = 0x%08lx\n", dev->name, (unsigned long)off,
                (unsigned long)*value_out);
    }
    return ok;
}

static mm_bool window_write(void *opaque, mm_u32 offset, mm_u32 size, mm_u32 value)
{
    const mm_u8 *win = (const mm_u8 *)opaque;
    mm_u32 ns_addr = IMXRT700_PERIPH_BASE_NS + offset;
    mm_u32 off = 0;
    struct imxrt700_dev *dev;
    mm_bool ok;

    if (size == 0u || size > 4u) {
        return MM_FALSE;
    }
    dev = dev_lookup(ns_addr, &off, 0);
    access_secure = (*win == window_s) ? MM_TRUE : MM_FALSE;
    if (dev == 0) {
        if (!unmapped_warned || trace_enabled()) {
            fprintf(stderr, "[IMXRT700] write to unmapped peripheral address 0x%08lx (WI)\n",
                    (unsigned long)(ns_addr | (access_secure ? 0x10000000u : 0u)));
            unmapped_warned = MM_TRUE;
        }
        return MM_TRUE;
    }
    if (trace_enabled()) {
        fprintf(stderr, "[IMXRT700] wr %s+0x%03lx <= 0x%08lx\n", dev->name, (unsigned long)off,
                (unsigned long)value);
    }
    ok = MM_FALSE;
    if (dev->write != 0) {
        ok = dev->write(dev, off, size, value);
    }
    if (!ok) {
        ok = mm_imxrt700_regfile_write(dev, off, size, value);
    }
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                               */
/* ------------------------------------------------------------------------ */

void mm_imxrt700_bind_nvic(int core, struct mm_nvic *nvic)
{
    if (core == IMXRT700_CPU0 || core == IMXRT700_CPU1) {
        nvics[core] = nvic;
    }
}

struct mm_nvic *mm_imxrt700_nvic(int core)
{
    if (core == IMXRT700_CPU0 || core == IMXRT700_CPU1) {
        return nvics[core];
    }
    return 0;
}

void mm_imxrt700_irq_set(int core, int irq, mm_bool level)
{
    struct mm_nvic *n = mm_imxrt700_nvic(core);
    if (n == 0 || irq < 0) {
        return;
    }
    if (level) {
        mm_nvic_set_pending(n, (mm_u32)irq, MM_TRUE);
    }
}

/* ------------------------------------------------------------------------ */
/* Target hooks                                                             */
/* ------------------------------------------------------------------------ */

void mm_imxrt700_mmio_reset(void)
{
    devs_reset();
    crc_state = 0u;
    access_secure = MM_FALSE;
    unmapped_warned = MM_FALSE;
    mm_imxrt700_mc_reset();
    mm_imxrt700_secure_reset();
    mm_imxrt700_romapi_reset();
}

static void attach_status_hooks(void)
{
    static const char *const status_devs[] = {
        "CLKCTL0", "CLKCTL2", "CLKCTL3", "FRO0", "FRO2", "OSC32KNP"
    };
    mm_u32 i;
    for (i = 0; i < sizeof(status_devs) / sizeof(status_devs[0]); ++i) {
        (void)mm_imxrt700_dev_hook(status_devs[i], status_read, 0, 0);
    }
}

mm_bool mm_imxrt700_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    char name[8];
    mm_u32 i;

    devs_build();
    if (!devs_ready) {
        return MM_FALSE;
    }
    attach_status_hooks();
    for (i = 0; i <= 10u; ++i) {
        snprintf(name, sizeof(name), "GPIO%lu", (unsigned long)i);
        (void)mm_imxrt700_dev_hook(name, gpio_read, gpio_write, 0);
    }
    (void)mm_imxrt700_dev_hook("CRC", crc_read, crc_write, 0);
    mm_imxrt700_flexcomm_attach();
    mm_imxrt700_timers_attach();
    mm_imxrt700_mc_attach();
    mm_imxrt700_secure_attach();

    memset(&reg, 0, sizeof(reg));
    reg.base = IMXRT700_PERIPH_BASE_NS;
    reg.size = PERIPH_WINDOW_SIZE;
    reg.opaque = (void *)&window_ns;
    reg.read = window_read;
    reg.write = window_write;
    if (!mmio_bus_register_region(bus, &reg)) {
        return MM_FALSE;
    }
    reg.base = IMXRT700_PERIPH_BASE_S;
    reg.opaque = (void *)&window_s;
    if (!mmio_bus_register_region(bus, &reg)) {
        return MM_FALSE;
    }
    return mm_imxrt700_romapi_register_mmio(bus);
}

void mm_imxrt700_flash_bind(struct mm_memmap *map,
                            mm_u8 *flash,
                            mm_u32 flash_size,
                            const struct mm_flash_persist *persist,
                            mm_u32 flags)
{
    (void)persist;
    (void)flags;
    bound_map = map;
    bound_flash = flash;
    bound_flash_size = flash_size;
    mm_imxrt700_secure_flash_bind(flash, flash_size);
}

struct mm_memmap *mm_imxrt700_memmap(void)
{
    return bound_map;
}

mm_u8 *mm_imxrt700_flash(mm_u32 *size_out)
{
    if (size_out != 0) {
        *size_out = bound_flash_size;
    }
    return bound_flash;
}

mm_u64 mm_imxrt700_cpu_hz(void)
{
    /* CPU0 main clock out of the boot ROM (SystemCoreClock default). */
    return 192000000ull;
}

void mm_imxrt700_periph_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    (void)bus;
    mm_imxrt700_bind_nvic(IMXRT700_CPU0, nvic);
}

void mm_imxrt700_periph_reset(void)
{
    mm_imxrt700_flexcomm_reset();
    mm_imxrt700_timers_reset();
}

void mm_imxrt700_periph_poll(void)
{
    mm_imxrt700_flexcomm_poll();
}

void mm_imxrt700_tick(mm_u64 cycles)
{
    mm_imxrt700_timers_tick(cycles);
    mm_imxrt700_mc_poll();
}

/* ------------------------------------------------------------------------ */
/* Boot ROM model                                                           */
/* ------------------------------------------------------------------------ */

#define FCB_TAG 0x42464346u /* "FCFB" */
#define IVT_OFFSET_XSPI 0x4000u
#define IMG_LENGTH_OFF 0x20u
#define IMG_EXEC_ADDR_OFF 0x34u
#define RAM_LOAD_MIN_OFF 0x15000u /* ROM owns the first 84 KB of SRAM during boot */

static mm_u32 rd32(const mm_u8 *p)
{
    return (mm_u32)p[0] | ((mm_u32)p[1] << 8) | ((mm_u32)p[2] << 16) | ((mm_u32)p[3] << 24);
}

static mm_bool ram_secure_offset(mm_u32 addr, mm_u32 *off_out)
{
    /* Any SRAM alias: code bus 0x0/0x10000000, system bus 0x20000000/0x30000000. */
    mm_u32 base = addr & 0xF0000000u;
    mm_u32 off = addr & 0x0FFFFFFFu;
    if ((base == 0x00000000u || base == 0x10000000u || base == 0x20000000u || base == 0x30000000u) &&
        off < IMXRT700_RAM_SIZE) {
        *off_out = off;
        return MM_TRUE;
    }
    return MM_FALSE;
}

mm_bool mm_imxrt700_boot_resolve(struct mm_memmap *map,
                                 const mm_u8 *flash,
                                 mm_u32 flash_size,
                                 mm_u32 *vtor_out)
{
    mm_u32 ivt;
    mm_u32 exec_addr;
    mm_u32 length;
    mm_u32 ram_off;
    mm_u32 i;

    if (flash == 0 || vtor_out == 0 || flash_size < IVT_OFFSET_XSPI + 0x40u) {
        return MM_FALSE;
    }
    if (rd32(flash) != FCB_TAG) {
        return MM_FALSE; /* raw image: vector table at offset 0 */
    }
    ivt = IVT_OFFSET_XSPI;
    exec_addr = rd32(flash + ivt + IMG_EXEC_ADDR_OFF);
    length = rd32(flash + ivt + IMG_LENGTH_OFF);
    if (exec_addr == 0u || exec_addr == 0xFFFFFFFFu) {
        *vtor_out = IMXRT700_FLASH_BASE_S + ivt;
        printf("[IMXRT700] boot: XIP from XSPI0, vector table 0x%08lx\n", (unsigned long)*vtor_out);
        return MM_TRUE;
    }
    if (!ram_secure_offset(exec_addr, &ram_off) || ram_off < RAM_LOAD_MIN_OFF ||
        length == 0u || length > flash_size - ivt || ram_off + length > IMXRT700_RAM_SIZE) {
        fprintf(stderr, "[IMXRT700] boot: invalid load-to-RAM image (exec=0x%08lx len=0x%lx)\n",
                (unsigned long)exec_addr, (unsigned long)length);
        return MM_FALSE;
    }
    for (i = 0; i < length; ++i) {
        (void)mm_memmap_write_ram_raw(map, IMXRT700_RAM_BASE_S + ram_off + i, 1u, flash[ivt + i]);
    }
    *vtor_out = exec_addr;
    printf("[IMXRT700] boot: loaded 0x%lx bytes to RAM, vector table 0x%08lx\n",
           (unsigned long)length, (unsigned long)*vtor_out);
    return MM_TRUE;
}
