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

#include <string.h>
#include "imxrt700/imxrt700_timers.h"
#include "imxrt700/imxrt700_mmio.h"

/* ------------------------------------------------------------------------ */
/* CTIMER                                                                   */
/* ------------------------------------------------------------------------ */

#define CT_IR 0x00u
#define CT_TCR 0x04u
#define CT_TC 0x08u
#define CT_PR 0x0Cu
#define CT_PC 0x10u
#define CT_MCR 0x14u
#define CT_MR0 0x18u

#define TCR_CEN (1u << 0)
#define TCR_CRST (1u << 1)

/* The timer clock is modelled as the CPU clock. */
static const struct {
    const char *name;
    int core;
    int irq;
} ct_table[] = {
    { "CTIMER0", IMXRT700_CPU0, 3 },  { "CTIMER1", IMXRT700_CPU0, 4 },
    { "CTIMER2", IMXRT700_CPU0, 32 }, { "CTIMER3", IMXRT700_CPU0, 6 },
    { "CTIMER4", IMXRT700_CPU0, 33 }, { "CTIMER5", IMXRT700_CPU1, 7 },
    { "CTIMER6", IMXRT700_CPU1, 8 },  { "CTIMER7", IMXRT700_CPU1, 9 },
};

#define CT_COUNT (sizeof(ct_table) / sizeof(ct_table[0]))

static struct imxrt700_dev *ct_devs[CT_COUNT];

static mm_bool ct_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    if (size != 4u) {
        return MM_FALSE;
    }
    if (off == CT_IR) {
        /* Interrupt flags are write-1-to-clear. */
        mm_imxrt700_reg_put(dev, CT_IR, mm_imxrt700_reg(dev, CT_IR) & ~value);
        return MM_TRUE;
    }
    if (off == CT_TCR) {
        mm_imxrt700_reg_put(dev, CT_TCR, value);
        if (value & TCR_CRST) {
            mm_imxrt700_reg_put(dev, CT_TC, 0u);
            mm_imxrt700_reg_put(dev, CT_PC, 0u);
        }
        return MM_TRUE;
    }
    return MM_FALSE;
}

static void ct_advance(mm_u32 idx, mm_u64 cycles)
{
    struct imxrt700_dev *dev = ct_devs[idx];
    mm_u32 tcr;
    mm_u64 pr;
    mm_u64 pc;
    mm_u64 steps;
    mm_u32 guard;

    if (dev == 0) {
        return;
    }
    tcr = mm_imxrt700_reg(dev, CT_TCR);
    if ((tcr & TCR_CEN) == 0u || (tcr & TCR_CRST) != 0u) {
        return;
    }
    pr = (mm_u64)mm_imxrt700_reg(dev, CT_PR) + 1u;
    pc = (mm_u64)mm_imxrt700_reg(dev, CT_PC) + cycles;
    steps = pc / pr;
    mm_imxrt700_reg_put(dev, CT_PC, (mm_u32)(pc % pr));

    /* Walk the TC forward, stopping at every match that acts. */
    for (guard = 0; steps > 0u && guard < 64u; ++guard) {
        mm_u32 tc = mm_imxrt700_reg(dev, CT_TC);
        mm_u32 mcr = mm_imxrt700_reg(dev, CT_MCR);
        mm_u64 nearest = steps + 1u;
        mm_u32 hit_mask = 0;
        mm_u32 n;
        for (n = 0; n < 4u; ++n) {
            mm_u32 act = (mcr >> (3u * n)) & 7u;
            mm_u32 mr = mm_imxrt700_reg(dev, CT_MR0 + 4u * n);
            mm_u64 dist;
            if (act == 0u) {
                continue;
            }
            dist = (mm_u64)(mm_u32)(mr - tc);
            if (dist == 0u || dist > steps) {
                continue;
            }
            if (dist < nearest) {
                nearest = dist;
                hit_mask = 1u << n;
            } else if (dist == nearest) {
                hit_mask |= 1u << n;
            }
        }
        if (hit_mask == 0u) {
            mm_imxrt700_reg_put(dev, CT_TC, tc + (mm_u32)steps);
            break;
        }
        tc += (mm_u32)nearest;
        steps -= nearest;
        mm_imxrt700_reg_put(dev, CT_TC, tc);
        for (n = 0; n < 4u; ++n) {
            mm_u32 act = (mcr >> (3u * n)) & 7u;
            if ((hit_mask & (1u << n)) == 0u) {
                continue;
            }
            if (act & 1u) {
                mm_imxrt700_reg_put(dev, CT_IR, mm_imxrt700_reg(dev, CT_IR) | (1u << n));
                mm_imxrt700_irq_set(ct_table[idx].core, ct_table[idx].irq, MM_TRUE);
            }
            if (act & 2u) {
                mm_imxrt700_reg_put(dev, CT_TC, 0u);
            }
            if (act & 4u) {
                mm_imxrt700_reg_put(dev, CT_TCR, mm_imxrt700_reg(dev, CT_TCR) & ~TCR_CEN);
                steps = 0u;
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* OS event timer                                                           */
/* ------------------------------------------------------------------------ */

#define OS_EVTIMERL 0x00u
#define OS_EVTIMERH 0x04u
#define OS_CAPTURE_L 0x08u
#define OS_CAPTURE_H 0x0Cu
#define OS_MATCH_L 0x10u
#define OS_MATCH_H 0x14u
#define OS_CTRL 0x1Cu

#define OS_CTRL_INTRFLAG (1u << 0)
#define OS_CTRL_INTENA (1u << 1)
#define OS_CTRL_MATCH_WR_RDY (1u << 2)

#define OS_COUNTER_MASK ((1ull << 42) - 1u)
#define OS_TICK_HZ 1000000ull /* 1 MHz OSTIMER function clock */

static const struct {
    const char *name;
    int core;
    int irq;
} os_table[] = {
    { "OSTIMER_CPU0", IMXRT700_CPU0, 34 },
    { "OSTIMER_CPU1", IMXRT700_CPU1, 30 },
};

static struct imxrt700_dev *os_devs[2];
static mm_u64 os_counter;
static mm_u64 os_cycle_acc;

mm_u64 mm_imxrt700_gray_encode(mm_u64 v)
{
    return v ^ (v >> 1);
}

mm_u64 mm_imxrt700_gray_decode(mm_u64 g)
{
    mm_u64 v = g;
    mm_u32 shift;
    for (shift = 1; shift < 64u; shift <<= 1) {
        v ^= v >> shift;
    }
    return v;
}

static mm_bool os_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    mm_u64 g = mm_imxrt700_gray_encode(os_counter);
    if (size != 4u) {
        return MM_FALSE;
    }
    switch (off) {
    case OS_EVTIMERL:
        *value_out = (mm_u32)g;
        return MM_TRUE;
    case OS_EVTIMERH:
        *value_out = (mm_u32)(g >> 32) & 0x3FFu;
        return MM_TRUE;
    case OS_CTRL:
        *value_out = mm_imxrt700_reg(dev, OS_CTRL) & ~OS_CTRL_MATCH_WR_RDY;
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

static mm_bool os_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    if (size != 4u) {
        return MM_FALSE;
    }
    switch (off) {
    case OS_EVTIMERL:
    case OS_EVTIMERH:
    case OS_CAPTURE_L:
    case OS_CAPTURE_H:
        return MM_TRUE; /* read-only */
    case OS_CTRL: {
        mm_u32 cur = mm_imxrt700_reg(dev, OS_CTRL);
        if (value & OS_CTRL_INTRFLAG) {
            cur &= ~OS_CTRL_INTRFLAG;
        }
        cur = (cur & OS_CTRL_INTRFLAG) | (value & ~(OS_CTRL_INTRFLAG | OS_CTRL_MATCH_WR_RDY));
        mm_imxrt700_reg_put(dev, OS_CTRL, cur);
        return MM_TRUE;
    }
    default:
        return MM_FALSE;
    }
}

static void os_advance(mm_u64 cycles)
{
    mm_u64 hz = mm_imxrt700_cpu_hz();
    mm_u64 ticks;
    mm_u64 prev = os_counter;
    mm_u32 i;

    os_cycle_acc += cycles * OS_TICK_HZ;
    ticks = os_cycle_acc / hz;
    os_cycle_acc %= hz;
    if (ticks == 0u) {
        return;
    }
    os_counter = (os_counter + ticks) & OS_COUNTER_MASK;
    for (i = 0; i < 2u; ++i) {
        struct imxrt700_dev *dev = os_devs[i];
        mm_u64 match;
        if (dev == 0) {
            continue;
        }
        match = mm_imxrt700_gray_decode(((mm_u64)(mm_imxrt700_reg(dev, OS_MATCH_H) & 0x3FFu) << 32) |
                                        mm_imxrt700_reg(dev, OS_MATCH_L));
        if (((match - prev - 1u) & OS_COUNTER_MASK) < ticks) {
            mm_u32 ctrl = mm_imxrt700_reg(dev, OS_CTRL) | OS_CTRL_INTRFLAG;
            mm_imxrt700_reg_put(dev, OS_CTRL, ctrl);
            if (ctrl & OS_CTRL_INTENA) {
                mm_imxrt700_irq_set(os_table[i].core, os_table[i].irq, MM_TRUE);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */

void mm_imxrt700_timers_attach(void)
{
    mm_u32 i;
    for (i = 0; i < CT_COUNT; ++i) {
        ct_devs[i] = mm_imxrt700_dev(ct_table[i].name);
        (void)mm_imxrt700_dev_hook(ct_table[i].name, 0, ct_write, 0);
    }
    for (i = 0; i < 2u; ++i) {
        os_devs[i] = mm_imxrt700_dev(os_table[i].name);
        (void)mm_imxrt700_dev_hook(os_table[i].name, os_read, os_write, 0);
    }
}

void mm_imxrt700_timers_reset(void)
{
    os_counter = 0u;
    os_cycle_acc = 0u;
}

void mm_imxrt700_timers_tick(mm_u64 cycles)
{
    mm_u32 i;
    for (i = 0; i < CT_COUNT; ++i) {
        ct_advance(i, cycles);
    }
    os_advance(cycles);
}
