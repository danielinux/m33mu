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

/*
 * RP2350 PIO: three blocks of four programmable state machines.
 *
 * The model executes the PIO instruction set cycle by cycle, driven from the
 * SoC tick hook. Pin state is published back to IO_BANK0/SIO so that PIO
 * output is visible to the GPIO viewer and readable through SIO GPIO_IN.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rp2350/rp2350_pio.h"
#include "rp2350/rp2350_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

#define PIO_COUNT        3u
#define PIO_SM_COUNT     4u
#define PIO_FIFO_DEPTH   4u
#define PIO_IMEM_SIZE    32u
#define PIO_PIN_COUNT    48u

#define PIO0_BASE        0x50200000u
#define PIO_INSTANCE_STRIDE 0x00100000u
#define PIO_REG_SIZE     0x1000u
#define PIO_ALIAS_SIZE   0x4000u

/* Register offsets */
#define PIO_CTRL              0x000u
#define PIO_FSTAT             0x004u
#define PIO_FDEBUG            0x008u
#define PIO_FLEVEL            0x00cu
#define PIO_TXF0              0x010u
#define PIO_RXF0              0x020u
#define PIO_IRQ               0x030u
#define PIO_IRQ_FORCE         0x034u
#define PIO_INPUT_SYNC_BYPASS 0x038u
#define PIO_DBG_PADOUT        0x03cu
#define PIO_DBG_PADOE         0x040u
#define PIO_DBG_CFGINFO       0x044u
#define PIO_INSTR_MEM0        0x048u
#define PIO_SM0_CLKDIV        0x0c8u
#define PIO_SM_STRIDE         0x018u
#define PIO_RXF0_PUTGET0      0x128u
#define PIO_GPIOBASE          0x168u
#define PIO_INTR              0x16cu
#define PIO_IRQ0_INTE         0x170u
#define PIO_IRQ1_INTE         0x17cu

/* SMx register offsets within the per-SM block */
#define PIO_SM_CLKDIV         0x00u
#define PIO_SM_EXECCTRL       0x04u
#define PIO_SM_SHIFTCTRL      0x08u
#define PIO_SM_ADDR           0x0cu
#define PIO_SM_INSTR          0x10u
#define PIO_SM_PINCTRL        0x14u

/* EXECCTRL fields */
#define EXECCTRL_STATUS_N(v)     ((v) & 0x1fu)
#define EXECCTRL_STATUS_SEL(v)   (((v) >> 5) & 0x3u)
#define EXECCTRL_WRAP_BOTTOM(v)  (((v) >> 7) & 0x1fu)
#define EXECCTRL_WRAP_TOP(v)     (((v) >> 12) & 0x1fu)
#define EXECCTRL_OUT_STICKY(v)   (((v) >> 17) & 0x1u)
#define EXECCTRL_INLINE_OUT(v)   (((v) >> 18) & 0x1u)
#define EXECCTRL_OUT_EN_SEL(v)   (((v) >> 19) & 0x1fu)
#define EXECCTRL_JMP_PIN(v)      (((v) >> 24) & 0x1fu)
#define EXECCTRL_SIDE_PINDIR(v)  (((v) >> 29) & 0x1u)
#define EXECCTRL_SIDE_EN(v)      (((v) >> 30) & 0x1u)
#define EXECCTRL_EXEC_STALLED    (1u << 31)
#define EXECCTRL_WMASK           0x7fffffffu

/* SHIFTCTRL fields */
#define SHIFTCTRL_IN_COUNT(v)    ((v) & 0x1fu)
#define SHIFTCTRL_FJOIN_RX_GET(v) (((v) >> 14) & 0x1u)
#define SHIFTCTRL_FJOIN_RX_PUT(v) (((v) >> 15) & 0x1u)
#define SHIFTCTRL_AUTOPUSH(v)    (((v) >> 16) & 0x1u)
#define SHIFTCTRL_AUTOPULL(v)    (((v) >> 17) & 0x1u)
#define SHIFTCTRL_IN_SHIFTDIR(v) (((v) >> 18) & 0x1u)
#define SHIFTCTRL_OUT_SHIFTDIR(v) (((v) >> 19) & 0x1u)
#define SHIFTCTRL_PUSH_THRESH(v) (((v) >> 20) & 0x1fu)
#define SHIFTCTRL_PULL_THRESH(v) (((v) >> 25) & 0x1fu)
#define SHIFTCTRL_FJOIN_TX(v)    (((v) >> 30) & 0x1u)
#define SHIFTCTRL_FJOIN_RX(v)    (((v) >> 31) & 0x1u)
#define SHIFTCTRL_JOIN_MASK      0xc000c000u

/* PINCTRL fields */
#define PINCTRL_OUT_BASE(v)      ((v) & 0x1fu)
#define PINCTRL_SET_BASE(v)      (((v) >> 5) & 0x1fu)
#define PINCTRL_SIDESET_BASE(v)  (((v) >> 10) & 0x1fu)
#define PINCTRL_IN_BASE(v)       (((v) >> 15) & 0x1fu)
#define PINCTRL_OUT_COUNT(v)     (((v) >> 20) & 0x3fu)
#define PINCTRL_SET_COUNT(v)     (((v) >> 26) & 0x7u)
#define PINCTRL_SIDESET_COUNT(v) (((v) >> 29) & 0x7u)

/* FDEBUG lanes */
#define FDEBUG_RXSTALL_SHIFT  0u
#define FDEBUG_RXUNDER_SHIFT  8u
#define FDEBUG_TXOVER_SHIFT   16u
#define FDEBUG_TXSTALL_SHIFT  24u

/* IO_BANK0 function select values */
#define FUNCSEL_PIO0 6u

/* NVIC interrupt numbers: PIO0_IRQ_0 == 15, two lines per block. */
#define PIO_IRQ_BASE 15u

struct pio_fifo {
    mm_u32 buf[8];
    mm_u8 head;
    mm_u8 count;
};

struct pio_sm {
    mm_u32 clkdiv;
    mm_u32 execctrl;
    mm_u32 shiftctrl;
    mm_u32 pinctrl;
    mm_u32 x;
    mm_u32 y;
    mm_u32 isr;
    mm_u32 osr;
    mm_u32 clk_acc;
    mm_u32 sticky_base;
    mm_u32 sticky_count;
    mm_u32 sticky_val;
    mm_u32 sticky_dir_base;
    mm_u32 sticky_dir_count;
    mm_u32 sticky_dir_val;
    struct pio_fifo tx;
    struct pio_fifo rx;
    mm_u16 exec_instr;
    mm_u8 isr_count;
    mm_u8 osr_count;
    mm_u8 pc;
    mm_u8 delay;
    mm_u8 stalled;
    mm_u8 exec_valid;
    mm_u8 pending_push;
    mm_u8 pend_delay;
    mm_u8 pend_from_exec;
    mm_u8 irq_wait;
};

struct pio_block {
    struct pio_sm sm[PIO_SM_COUNT];
    mm_u16 imem[PIO_IMEM_SIZE];
    mm_u32 index;
    mm_u32 sm_enable;
    mm_u32 irq;
    mm_u32 fdebug;
    mm_u32 input_sync_bypass;
    mm_u32 gpiobase;
    mm_u32 inte[2];
    mm_u32 intf[2];
    mm_u64 pad_out;
    mm_u64 pad_oe;
    mm_u8 irq_line[2];
};

static struct pio_block g_pio[PIO_COUNT];
static struct mm_nvic *g_pio_nvic;
static mm_u64 g_pio_owner[PIO_COUNT];  /* pins whose FUNCSEL selects this block */
static mm_u64 g_sio_out;
static mm_u64 g_sio_oe;
static mm_u64 g_pad_level;
static mm_bool g_pads_dirty = MM_TRUE;
static mm_bool g_pio_trace;
static mm_bool g_trace_checked;

static void pio_pads_refresh(void);

static mm_u32 pio_reset_mask(mm_u32 index)
{
    return 1u << (11u + index);
}

static mm_bool pio_in_reset(const struct pio_block *b)
{
    return mm_rp2350_reset_asserted(pio_reset_mask(b->index));
}

/* ------------------------------------------------------------------ */
/* FIFOs                                                              */
/* ------------------------------------------------------------------ */

static mm_u32 sm_tx_depth(const struct pio_sm *s)
{
    if (SHIFTCTRL_FJOIN_RX_PUT(s->shiftctrl) || SHIFTCTRL_FJOIN_RX_GET(s->shiftctrl)) {
        return PIO_FIFO_DEPTH;
    }
    if (SHIFTCTRL_FJOIN_RX(s->shiftctrl)) return 0u;
    if (SHIFTCTRL_FJOIN_TX(s->shiftctrl)) return PIO_FIFO_DEPTH * 2u;
    return PIO_FIFO_DEPTH;
}

static mm_u32 sm_rx_depth(const struct pio_sm *s)
{
    if (SHIFTCTRL_FJOIN_RX_PUT(s->shiftctrl) || SHIFTCTRL_FJOIN_RX_GET(s->shiftctrl)) {
        return 0u;
    }
    if (SHIFTCTRL_FJOIN_TX(s->shiftctrl)) return 0u;
    if (SHIFTCTRL_FJOIN_RX(s->shiftctrl)) return PIO_FIFO_DEPTH * 2u;
    return PIO_FIFO_DEPTH;
}

static mm_bool fifo_push(struct pio_fifo *f, mm_u32 depth, mm_u32 value)
{
    mm_u32 slot;
    if (f->count >= depth) return MM_FALSE;
    slot = ((mm_u32)f->head + (mm_u32)f->count) & 7u;
    f->buf[slot] = value;
    f->count++;
    return MM_TRUE;
}

static mm_bool fifo_pop(struct pio_fifo *f, mm_u32 *value_out)
{
    if (f->count == 0u) return MM_FALSE;
    if (value_out != 0) *value_out = f->buf[f->head];
    f->head = (mm_u8)((f->head + 1u) & 7u);
    f->count--;
    return MM_TRUE;
}

static mm_u32 fifo_peek(const struct pio_fifo *f)
{
    if (f->count == 0u) return 0u;
    return f->buf[f->head];
}

static void fifo_clear(struct pio_fifo *f)
{
    f->head = 0u;
    f->count = 0u;
}

static void fdebug_set(struct pio_block *b, mm_u32 shift, mm_u32 smi)
{
    b->fdebug |= 1u << (shift + smi);
}

/* ------------------------------------------------------------------ */
/* Pads                                                               */
/* ------------------------------------------------------------------ */

/*
 * Snapshot which pins each PIO block is allowed to drive, plus the SIO pad
 * state. Function select can only change from a CPU write, which cannot
 * happen while a tick is in progress, so this is refreshed per tick call.
 */
static void pio_pads_snapshot(void)
{
    mm_u32 lo = 0u;
    mm_u32 hi = 0u;
    mm_u32 oe_lo = 0u;
    mm_u32 oe_hi = 0u;
    mm_u32 pin;
    mm_u32 i;

    for (i = 0; i < PIO_COUNT; ++i) g_pio_owner[i] = 0u;
    for (pin = 0; pin < PIO_PIN_COUNT; ++pin) {
        mm_u32 fsel = mm_rp2350_gpio_funcsel(pin);
        if (fsel >= FUNCSEL_PIO0 && fsel < FUNCSEL_PIO0 + PIO_COUNT) {
            g_pio_owner[fsel - FUNCSEL_PIO0] |= 1ull << pin;
        }
    }
    mm_rp2350_sio_pad_state(&lo, &hi, &oe_lo, &oe_hi);
    g_sio_out = (mm_u64)lo | ((mm_u64)hi << 32);
    g_sio_oe = (mm_u64)oe_lo | ((mm_u64)oe_hi << 32);
    g_pads_dirty = MM_TRUE;
}

/* Combined pad levels seen by PIO inputs: PIO drivers win over SIO. */
static void pio_pads_refresh(void)
{
    mm_u64 driven = 0u;
    mm_u64 level = 0u;
    mm_u32 i;

    for (i = 0; i < PIO_COUNT; ++i) {
        mm_u64 own = g_pio[i].pad_oe & g_pio_owner[i];
        level = (level & ~own) | (g_pio[i].pad_out & own);
        driven |= own;
    }
    level |= g_sio_out & g_sio_oe & ~driven;
    g_pad_level = level;
    g_pads_dirty = MM_FALSE;
}

static mm_u64 pio_pad_levels(void)
{
    if (g_pads_dirty) pio_pads_refresh();
    return g_pad_level;
}

void mm_rp2350_pio_pad_state(mm_u32 *out_lo, mm_u32 *out_hi, mm_u32 *oe_lo, mm_u32 *oe_hi)
{
    mm_u64 out = 0u;
    mm_u64 oe = 0u;
    mm_u32 i;

    /* Cheap exit for the common case of a system that never enables PIO. */
    if ((g_pio[0].pad_oe | g_pio[1].pad_oe | g_pio[2].pad_oe) == 0u) {
        if (out_lo != 0) *out_lo = 0u;
        if (out_hi != 0) *out_hi = 0u;
        if (oe_lo != 0) *oe_lo = 0u;
        if (oe_hi != 0) *oe_hi = 0u;
        return;
    }
    pio_pads_snapshot();
    for (i = 0; i < PIO_COUNT; ++i) {
        mm_u64 own = g_pio[i].pad_oe & g_pio_owner[i];
        out = (out & ~own) | (g_pio[i].pad_out & own);
        oe |= own;
    }
    if (out_lo != 0) *out_lo = (mm_u32)out;
    if (out_hi != 0) *out_hi = (mm_u32)(out >> 32);
    if (oe_lo != 0) *oe_lo = (mm_u32)oe;
    if (oe_hi != 0) *oe_hi = (mm_u32)(oe >> 32);
}

/* Write `count` pins starting at PIO-relative `base` (wrapping within the
 * block's 32 pin window) with the LSBs of `data`. */
static void sm_pin_write(struct pio_block *b, mm_u32 base, mm_u32 count, mm_u32 data, mm_bool dirs)
{
    mm_u32 i;
    for (i = 0; i < count; ++i) {
        mm_u32 sys = b->gpiobase + ((base + i) & 0x1fu);
        mm_u64 bit;
        if (sys >= PIO_PIN_COUNT) continue;
        bit = 1ull << sys;
        if (dirs) {
            if ((data >> i) & 1u) b->pad_oe |= bit;
            else b->pad_oe &= ~bit;
        } else {
            if ((data >> i) & 1u) b->pad_out |= bit;
            else b->pad_out &= ~bit;
        }
    }
    g_pads_dirty = MM_TRUE;
}

/* 32-bit view of the pads inside this block's GPIO window. */
static mm_u32 pio_window_in(const struct pio_block *b)
{
    return (mm_u32)(pio_pad_levels() >> b->gpiobase);
}

static mm_u32 sm_in_pins(const struct pio_block *b, const struct pio_sm *s)
{
    mm_u32 window = pio_window_in(b);
    mm_u32 in_base = PINCTRL_IN_BASE(s->pinctrl);
    mm_u32 in_count = SHIFTCTRL_IN_COUNT(s->shiftctrl);
    mm_u32 v;

    if (in_base == 0u) v = window;
    else v = (window >> in_base) | (window << (32u - in_base));
    if (in_count != 0u) v &= (1u << in_count) - 1u;
    return v;
}

/* ------------------------------------------------------------------ */
/* Instruction execution                                              */
/* ------------------------------------------------------------------ */

static struct pio_block *pio_peer(struct pio_block *b, mm_bool next)
{
    mm_u32 idx;
    if (next) idx = (b->index + 1u) % PIO_COUNT;
    else idx = (b->index + PIO_COUNT - 1u) % PIO_COUNT;
    return &g_pio[idx];
}

/*
 * Resolve the target block and flag number of an IRQ index field. Bits [4:3]
 * select absolute (00), the lower-numbered block (01), SM-relative (10) or
 * the higher-numbered block (11).
 */
static struct pio_block *pio_irq_target(struct pio_block *b, mm_u32 smi, mm_u32 idx5, mm_u32 *flag_out)
{
    mm_u32 mode = idx5 & 0x18u;
    mm_u32 idx = idx5 & 0x7u;
    struct pio_block *tgt = b;

    if (mode == 0x10u) {
        idx = (idx & 0x4u) | ((idx + smi) & 0x3u);
    } else if (mode == 0x08u) {
        tgt = pio_peer(b, MM_FALSE);
    } else if (mode == 0x18u) {
        tgt = pio_peer(b, MM_TRUE);
    }
    if (flag_out != 0) *flag_out = idx;
    return tgt;
}

static mm_u32 sm_status_value(struct pio_block *b, struct pio_sm *s)
{
    mm_u32 sel = EXECCTRL_STATUS_SEL(s->execctrl);
    mm_u32 n = EXECCTRL_STATUS_N(s->execctrl);

    if (sel == 0u) return ((mm_u32)s->tx.count < n) ? 0xffffffffu : 0u;
    if (sel == 1u) return ((mm_u32)s->rx.count < n) ? 0xffffffffu : 0u;
    if (sel == 2u) {
        struct pio_block *tgt = b;
        if ((n & 0x18u) == 0x08u) tgt = pio_peer(b, MM_FALSE);
        else if ((n & 0x18u) == 0x10u) tgt = pio_peer(b, MM_TRUE);
        return ((tgt->irq >> (n & 0x7u)) & 1u) ? 0xffffffffu : 0u;
    }
    return 0u;
}

static mm_u32 bit_reverse32(mm_u32 v)
{
    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);
    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);
    v = ((v & 0x0f0f0f0fu) << 4) | ((v >> 4) & 0x0f0f0f0fu);
    v = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
    return (v << 16) | (v >> 16);
}

static mm_u32 sm_pull_thresh(const struct pio_sm *s)
{
    mm_u32 t = SHIFTCTRL_PULL_THRESH(s->shiftctrl);
    return (t == 0u) ? 32u : t;
}

static mm_u32 sm_push_thresh(const struct pio_sm *s)
{
    mm_u32 t = SHIFTCTRL_PUSH_THRESH(s->shiftctrl);
    return (t == 0u) ? 32u : t;
}

static void sm_out_pins(struct pio_block *b, struct pio_sm *s, mm_u32 data, mm_bool dirs)
{
    mm_u32 count = PINCTRL_OUT_COUNT(s->pinctrl);
    mm_u32 base = PINCTRL_OUT_BASE(s->pinctrl);
    if (count > 32u) count = 32u;
    if (count == 0u) return;
    sm_pin_write(b, base, count, data, dirs);
    if (dirs) {
        s->sticky_dir_base = base;
        s->sticky_dir_count = count;
        s->sticky_dir_val = data;
    } else {
        s->sticky_base = base;
        s->sticky_count = count;
        s->sticky_val = data;
    }
}

/* Re-assert the most recent OUT/SET pin write (EXECCTRL.OUT_STICKY). */
static void sm_sticky_reassert(struct pio_block *b, struct pio_sm *s)
{
    if (s->sticky_count != 0u) {
        sm_pin_write(b, s->sticky_base, s->sticky_count, s->sticky_val, MM_FALSE);
    }
    if (s->sticky_dir_count != 0u) {
        sm_pin_write(b, s->sticky_dir_base, s->sticky_dir_count, s->sticky_dir_val, MM_TRUE);
    }
}

static mm_u32 sm_mov_source(struct pio_block *b, struct pio_sm *s, mm_u32 src, mm_bool *valid)
{
    *valid = MM_TRUE;
    switch (src) {
    case 0u: return sm_in_pins(b, s);
    case 1u: return s->x;
    case 2u: return s->y;
    case 3u: return 0u;
    case 5u: return sm_status_value(b, s);
    case 6u: return s->isr;
    case 7u: return s->osr;
    default: break;
    }
    *valid = MM_FALSE;
    return 0u;
}

/*
 * Execute one instruction. Returns MM_FALSE when the state machine stalled,
 * in which case no architectural state was changed (except for the FDEBUG
 * stall flags and, for a stalled autopush, the ISR contents).
 */
static mm_bool sm_exec(struct pio_block *b, mm_u32 smi, mm_u16 instr, mm_bool *jumped)
{
    struct pio_sm *s = &b->sm[smi];
    mm_u32 major = ((mm_u32)instr >> 13) & 0x7u;
    mm_u32 arg1 = ((mm_u32)instr >> 5) & 0x7u;
    mm_u32 arg2 = (mm_u32)instr & 0x1fu;

    switch (major) {
    case 0u: { /* JMP */
        mm_bool take = MM_FALSE;
        switch (arg1) {
        case 0u: take = MM_TRUE; break;
        case 1u: take = (s->x == 0u) ? MM_TRUE : MM_FALSE; break;
        case 2u: take = (s->x != 0u) ? MM_TRUE : MM_FALSE; s->x--; break;
        case 3u: take = (s->y == 0u) ? MM_TRUE : MM_FALSE; break;
        case 4u: take = (s->y != 0u) ? MM_TRUE : MM_FALSE; s->y--; break;
        case 5u: take = (s->x != s->y) ? MM_TRUE : MM_FALSE; break;
        case 6u: {
            mm_u32 window = pio_window_in(b);
            take = ((window >> EXECCTRL_JMP_PIN(s->execctrl)) & 1u) ? MM_TRUE : MM_FALSE;
            break;
        }
        case 7u: take = ((mm_u32)s->osr_count < sm_pull_thresh(s)) ? MM_TRUE : MM_FALSE; break;
        default: break;
        }
        if (take) {
            s->pc = (mm_u8)arg2;
            *jumped = MM_TRUE;
        }
        return MM_TRUE;
    }
    case 1u: { /* WAIT */
        mm_u32 pol = (arg1 >> 2) & 1u;
        mm_u32 src = arg1 & 0x3u;
        mm_u32 window;
        mm_u32 level;
        if (src == 2u) {
            mm_u32 flag = 0u;
            struct pio_block *tgt = pio_irq_target(b, smi, arg2, &flag);
            level = (tgt->irq >> flag) & 1u;
            if (level != pol) return MM_FALSE;
            if (pol != 0u) tgt->irq &= ~(1u << flag);
            return MM_TRUE;
        }
        window = pio_window_in(b);
        if (src == 0u) {
            level = (window >> (arg2 & 0x1fu)) & 1u;
        } else if (src == 1u) {
            mm_u32 pin = (PINCTRL_IN_BASE(s->pinctrl) + arg2) & 0x1fu;
            level = (window >> pin) & 1u;
        } else {
            mm_u32 pin = (EXECCTRL_JMP_PIN(s->execctrl) + (arg2 & 0x3u)) & 0x1fu;
            level = (window >> pin) & 1u;
        }
        return (level == pol) ? MM_TRUE : MM_FALSE;
    }
    case 2u: { /* IN */
        mm_u32 n = (arg2 == 0u) ? 32u : arg2;
        mm_u32 mask = (n >= 32u) ? 0xffffffffu : ((1u << n) - 1u);
        mm_bool valid = MM_TRUE;
        mm_u32 data;
        mm_u32 thresh;
        switch (arg1) {
        case 0u: data = sm_in_pins(b, s); break;
        case 1u: data = s->x; break;
        case 2u: data = s->y; break;
        case 3u: data = 0u; break;
        case 5u: data = sm_status_value(b, s); break;
        case 6u: data = s->isr; break;
        case 7u: data = s->osr; break;
        default: data = 0u; valid = MM_FALSE; break;
        }
        if (!valid) return MM_TRUE; /* reserved encoding: treat as NOP */
        data &= mask;
        if (SHIFTCTRL_IN_SHIFTDIR(s->shiftctrl)) {
            s->isr = (n >= 32u) ? data : ((s->isr >> n) | (data << (32u - n)));
        } else {
            s->isr = (n >= 32u) ? data : ((s->isr << n) | data);
        }
        s->isr_count = (mm_u8)((s->isr_count + n > 32u) ? 32u : (s->isr_count + n));
        thresh = sm_push_thresh(s);
        if (SHIFTCTRL_AUTOPUSH(s->shiftctrl) && (mm_u32)s->isr_count >= thresh) {
            if (!fifo_push(&s->rx, sm_rx_depth(s), s->isr)) {
                fdebug_set(b, FDEBUG_RXSTALL_SHIFT, smi);
                s->pending_push = 1u;
                return MM_FALSE;
            }
            s->isr = 0u;
            s->isr_count = 0u;
        }
        return MM_TRUE;
    }
    case 3u: { /* OUT */
        mm_u32 n = (arg2 == 0u) ? 32u : arg2;
        mm_u32 mask = (n >= 32u) ? 0xffffffffu : ((1u << n) - 1u);
        mm_u32 thresh = sm_pull_thresh(s);
        mm_u32 data;
        mm_u32 v;

        if (SHIFTCTRL_AUTOPULL(s->shiftctrl) && (mm_u32)s->osr_count >= thresh) {
            if (!fifo_pop(&s->tx, &v)) {
                fdebug_set(b, FDEBUG_TXSTALL_SHIFT, smi);
                return MM_FALSE;
            }
            s->osr = v;
            s->osr_count = 0u;
        }
        if (SHIFTCTRL_OUT_SHIFTDIR(s->shiftctrl)) {
            data = s->osr & mask;
            s->osr = (n >= 32u) ? 0u : (s->osr >> n);
        } else {
            data = (n >= 32u) ? s->osr : (s->osr >> (32u - n));
            s->osr = (n >= 32u) ? 0u : (s->osr << n);
        }
        s->osr_count = (mm_u8)((s->osr_count + n > 32u) ? 32u : (s->osr_count + n));

        switch (arg1) {
        case 0u: /* PINS */
        case 4u: /* PINDIRS */
        {
            mm_bool enabled = MM_TRUE;
            if (EXECCTRL_INLINE_OUT(s->execctrl)) {
                enabled = ((data >> EXECCTRL_OUT_EN_SEL(s->execctrl)) & 1u) ? MM_TRUE : MM_FALSE;
            }
            if (enabled) {
                sm_out_pins(b, s, data, (arg1 == 4u) ? MM_TRUE : MM_FALSE);
            } else if (EXECCTRL_OUT_STICKY(s->execctrl)) {
                if (arg1 == 4u) s->sticky_dir_count = 0u;
                else s->sticky_count = 0u;
            }
            break;
        }
        case 1u: s->x = data; break;
        case 2u: s->y = data; break;
        case 3u: break; /* NULL */
        case 5u: /* PC */
            s->pc = (mm_u8)(data & 0x1fu);
            *jumped = MM_TRUE;
            break;
        case 6u: /* ISR */
            s->isr = data;
            s->isr_count = (mm_u8)n;
            break;
        case 7u: /* EXEC */
            s->exec_instr = (mm_u16)(data & 0xffffu);
            s->exec_valid = 1u;
            break;
        default: break;
        }
        /* Autopull refills as soon as the OSR is drained and data is ready. */
        if (SHIFTCTRL_AUTOPULL(s->shiftctrl) && (mm_u32)s->osr_count >= thresh) {
            if (fifo_pop(&s->tx, &v)) {
                s->osr = v;
                s->osr_count = 0u;
            }
        }
        return MM_TRUE;
    }
    case 4u: { /* PUSH / PULL / MOV RXFIFO */
        if (arg2 != 0u) {
            /* RP2350 random access to the RX FIFO registers */
            mm_u32 index;
            if ((arg1 & 0x3u) != 0u || (arg2 & 0x10u) == 0u) return MM_TRUE;
            index = (arg2 & 0x8u) ? (arg2 & 0x3u) : (s->y & 0x3u);
            if (arg1 & 0x4u) { /* MOV OSR, RXFIFO[index] */
                if (!SHIFTCTRL_FJOIN_RX_GET(s->shiftctrl)) return MM_TRUE;
                s->osr = s->rx.buf[index];
                s->osr_count = 0u;
            } else { /* MOV RXFIFO[index], ISR */
                if (!SHIFTCTRL_FJOIN_RX_PUT(s->shiftctrl)) return MM_TRUE;
                s->rx.buf[index] = s->isr;
                s->isr = 0u;
                s->isr_count = 0u;
            }
            return MM_TRUE;
        }
        if (arg1 & 0x4u) { /* PULL */
            mm_bool ifempty = (arg1 & 0x2u) ? MM_TRUE : MM_FALSE;
            mm_bool block = (arg1 & 0x1u) ? MM_TRUE : MM_FALSE;
            mm_u32 v;
            if ((ifempty || SHIFTCTRL_AUTOPULL(s->shiftctrl)) &&
                (mm_u32)s->osr_count < sm_pull_thresh(s)) {
                return MM_TRUE;
            }
            if (!fifo_pop(&s->tx, &v)) {
                fdebug_set(b, FDEBUG_TXSTALL_SHIFT, smi);
                if (block) return MM_FALSE;
                s->osr = s->x;
                s->osr_count = 0u;
                return MM_TRUE;
            }
            s->osr = v;
            s->osr_count = 0u;
            return MM_TRUE;
        } else { /* PUSH */
            mm_bool iffull = (arg1 & 0x2u) ? MM_TRUE : MM_FALSE;
            mm_bool block = (arg1 & 0x1u) ? MM_TRUE : MM_FALSE;
            if ((iffull || SHIFTCTRL_AUTOPUSH(s->shiftctrl)) &&
                (mm_u32)s->isr_count < sm_push_thresh(s)) {
                return MM_TRUE;
            }
            if (!fifo_push(&s->rx, sm_rx_depth(s), s->isr)) {
                fdebug_set(b, FDEBUG_RXSTALL_SHIFT, smi);
                if (block) return MM_FALSE;
            }
            s->isr = 0u;
            s->isr_count = 0u;
            return MM_TRUE;
        }
    }
    case 5u: { /* MOV */
        mm_u32 op = (arg2 >> 3) & 0x3u;
        mm_bool valid = MM_TRUE;
        mm_u32 data = sm_mov_source(b, s, arg2 & 0x7u, &valid);
        if (!valid || op == 3u) return MM_TRUE; /* reserved: NOP */
        if (op == 1u) data = ~data;
        else if (op == 2u) data = bit_reverse32(data);
        switch (arg1) {
        case 0u: sm_out_pins(b, s, data, MM_FALSE); break;
        case 1u: s->x = data; break;
        case 2u: s->y = data; break;
        case 3u: sm_out_pins(b, s, data, MM_TRUE); break;
        case 4u:
            s->exec_instr = (mm_u16)(data & 0xffffu);
            s->exec_valid = 1u;
            break;
        case 5u:
            s->pc = (mm_u8)(data & 0x1fu);
            *jumped = MM_TRUE;
            break;
        case 6u:
            s->isr = data;
            s->isr_count = 0u;
            break;
        case 7u:
            s->osr = data;
            s->osr_count = 0u;
            break;
        default: break;
        }
        return MM_TRUE;
    }
    case 6u: { /* IRQ */
        mm_u32 flag = 0u;
        struct pio_block *tgt;
        if (arg1 & 0x4u) return MM_TRUE; /* reserved */
        tgt = pio_irq_target(b, smi, arg2, &flag);
        if (arg1 & 0x2u) { /* clear */
            tgt->irq &= ~(1u << flag);
            s->irq_wait = 0u;
            return MM_TRUE;
        }
        if (!s->irq_wait) {
            tgt->irq |= 1u << flag;
            if (arg1 & 0x1u) {
                s->irq_wait = 1u;
            } else {
                return MM_TRUE;
            }
        }
        if ((tgt->irq >> flag) & 1u) return MM_FALSE;
        s->irq_wait = 0u;
        return MM_TRUE;
    }
    case 7u: { /* SET */
        switch (arg1) {
        case 0u:
            sm_pin_write(b, PINCTRL_SET_BASE(s->pinctrl), PINCTRL_SET_COUNT(s->pinctrl), arg2, MM_FALSE);
            s->sticky_base = PINCTRL_SET_BASE(s->pinctrl);
            s->sticky_count = PINCTRL_SET_COUNT(s->pinctrl);
            s->sticky_val = arg2;
            break;
        case 1u: s->x = arg2; break;
        case 2u: s->y = arg2; break;
        case 4u:
            sm_pin_write(b, PINCTRL_SET_BASE(s->pinctrl), PINCTRL_SET_COUNT(s->pinctrl), arg2, MM_TRUE);
            s->sticky_dir_base = PINCTRL_SET_BASE(s->pinctrl);
            s->sticky_dir_count = PINCTRL_SET_COUNT(s->pinctrl);
            s->sticky_dir_val = arg2;
            break;
        default: break;
        }
        return MM_TRUE;
    }
    default: break;
    }
    return MM_TRUE;
}

/* ------------------------------------------------------------------ */
/* Scheduling                                                         */
/* ------------------------------------------------------------------ */

static void sm_advance_pc(struct pio_sm *s)
{
    mm_u32 top = EXECCTRL_WRAP_TOP(s->execctrl);
    mm_u32 bottom = EXECCTRL_WRAP_BOTTOM(s->execctrl);
    if ((mm_u32)s->pc == top) s->pc = (mm_u8)bottom;
    else s->pc = (mm_u8)((s->pc + 1u) & 0x1fu);
}

static void sm_complete(struct pio_sm *s, mm_u32 delay, mm_bool jumped, mm_bool from_exec)
{
    if (from_exec) {
        s->exec_valid = 0u;
        return;
    }
    if (!jumped) sm_advance_pc(s);
    s->delay = (mm_u8)delay;
}

/* Free-running clock divider: returns MM_TRUE on an execution cycle. */
static mm_bool sm_clock_tick(struct pio_sm *s)
{
    mm_u32 int_part = (s->clkdiv >> 16) & 0xffffu;
    mm_u32 frac = (s->clkdiv >> 8) & 0xffu;
    mm_u32 div256 = (int_part == 0u) ? (65536u << 8) : ((int_part << 8) | frac);

    s->clk_acc += 256u;
    if (s->clk_acc >= div256) {
        s->clk_acc -= div256;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static void sm_cycle(struct pio_block *b, mm_u32 smi)
{
    struct pio_sm *s = &b->sm[smi];
    mm_bool jumped = MM_FALSE;
    mm_bool from_exec;
    mm_u16 instr;
    mm_u32 delay_field;
    mm_u32 side_count;
    mm_u32 side_en;

    if (EXECCTRL_OUT_STICKY(s->execctrl)) sm_sticky_reassert(b, s);

    /* An autopush that found the RX FIFO full retries before anything else. */
    if (s->pending_push) {
        if (!fifo_push(&s->rx, sm_rx_depth(s), s->isr)) {
            s->stalled = 1u;
            return;
        }
        s->isr = 0u;
        s->isr_count = 0u;
        s->pending_push = 0u;
        s->stalled = 0u;
        sm_complete(s, s->pend_delay, MM_FALSE, s->pend_from_exec ? MM_TRUE : MM_FALSE);
        return;
    }

    if (s->delay > 0u) {
        s->delay--;
        return;
    }

    if (s->exec_valid) {
        instr = s->exec_instr;
        from_exec = MM_TRUE;
    } else {
        instr = b->imem[s->pc];
        from_exec = MM_FALSE;
    }

    delay_field = ((mm_u32)instr >> 8) & 0x1fu;
    side_count = PINCTRL_SIDESET_COUNT(s->pinctrl);
    if (side_count > 5u) side_count = 5u;  /* 6 and 7 are reserved encodings */
    side_en = EXECCTRL_SIDE_EN(s->execctrl);
    if (side_count != 0u) {
        mm_u32 data_bits = side_count - side_en;
        mm_bool apply = MM_TRUE;
        if (side_en != 0u) apply = (delay_field & 0x10u) ? MM_TRUE : MM_FALSE;
        if (apply && data_bits != 0u) {
            mm_u32 sv = (delay_field & (side_en ? 0x0fu : 0x1fu)) >> (5u - side_count);
            sm_pin_write(b, PINCTRL_SIDESET_BASE(s->pinctrl), data_bits, sv,
                         EXECCTRL_SIDE_PINDIR(s->execctrl) ? MM_TRUE : MM_FALSE);
        }
        delay_field &= (1u << (5u - side_count)) - 1u;
    }

    if (!sm_exec(b, smi, instr, &jumped)) {
        s->stalled = 1u;
        if (s->pending_push) {
            /* The IN already shifted; remember how to retire it. */
            s->pend_delay = (mm_u8)delay_field;
            s->pend_from_exec = from_exec ? 1u : 0u;
        }
        return;
    }
    s->stalled = 0u;
    sm_complete(s, delay_field, jumped, from_exec);
}

static void sm_restart(struct pio_sm *s)
{
    s->isr = 0u;
    s->isr_count = 0u;
    /*
     * Both shift registers become empty: the ISR has nothing to push, and the
     * OSR needs a (auto)pull before the next OUT produces data.
     */
    s->osr_count = 32u;
    s->delay = 0u;
    s->stalled = 0u;
    s->exec_valid = 0u;
    s->pending_push = 0u;
    s->irq_wait = 0u;
    s->sticky_base = 0u;
    s->sticky_count = 0u;
    s->sticky_val = 0u;
    s->sticky_dir_base = 0u;
    s->sticky_dir_count = 0u;
    s->sticky_dir_val = 0u;
}

/* ------------------------------------------------------------------ */
/* Register interface                                                 */
/* ------------------------------------------------------------------ */

static mm_u32 read_slice(mm_u32 reg, mm_u32 offset_in_reg, mm_u32 size_bytes)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes >= 4u) ? 0xffffffffu : ((1u << (size_bytes * 8u)) - 1u);
    return (reg >> shift) & mask;
}

static mm_u32 pio_fstat(struct pio_block *b)
{
    mm_u32 v = 0u;
    mm_u32 i;
    for (i = 0; i < PIO_SM_COUNT; ++i) {
        struct pio_sm *s = &b->sm[i];
        mm_u32 txd = sm_tx_depth(s);
        mm_u32 rxd = sm_rx_depth(s);
        if ((mm_u32)s->rx.count >= rxd) v |= 1u << i;           /* RXFULL */
        if ((mm_u32)s->rx.count == 0u) v |= 1u << (8u + i);     /* RXEMPTY */
        if ((mm_u32)s->tx.count >= txd) v |= 1u << (16u + i);   /* TXFULL */
        if ((mm_u32)s->tx.count == 0u) v |= 1u << (24u + i);    /* TXEMPTY */
    }
    return v;
}

static mm_u32 pio_flevel(struct pio_block *b)
{
    mm_u32 v = 0u;
    mm_u32 i;
    for (i = 0; i < PIO_SM_COUNT; ++i) {
        v |= ((mm_u32)b->sm[i].tx.count & 0xfu) << (i * 8u);
        v |= ((mm_u32)b->sm[i].rx.count & 0xfu) << (i * 8u + 4u);
    }
    return v;
}

static mm_u32 pio_intr(struct pio_block *b)
{
    mm_u32 v = 0u;
    mm_u32 i;
    for (i = 0; i < PIO_SM_COUNT; ++i) {
        struct pio_sm *s = &b->sm[i];
        if (s->rx.count != 0u) v |= 1u << i;
        if ((mm_u32)s->tx.count < sm_tx_depth(s)) v |= 1u << (4u + i);
    }
    v |= (b->irq & 0xffu) << 8;
    return v;
}

static void pio_update_irqs(struct pio_block *b)
{
    mm_u32 intr = pio_intr(b);
    mm_u32 k;
    for (k = 0; k < 2u; ++k) {
        mm_u32 ints = (intr | b->intf[k]) & b->inte[k];
        mm_u8 level = (ints != 0u) ? 1u : 0u;
        if (level == b->irq_line[k]) continue;
        b->irq_line[k] = level;
        if (g_pio_nvic != 0) {
            mm_nvic_set_pending(g_pio_nvic, PIO_IRQ_BASE + b->index * 2u + k,
                                level ? MM_TRUE : MM_FALSE);
        }
        if (g_pio_trace && level) {
            printf("[PIO%u_IRQ%u] ints=0x%08lx\n", (unsigned)b->index, (unsigned)k,
                   (unsigned long)ints);
        }
    }
}

static mm_u32 pio_read32(struct pio_block *b, mm_u32 off, mm_bool side_effects)
{
    if (off < PIO_TXF0) {
        if (off == PIO_CTRL) return b->sm_enable & 0xfu;
        if (off == PIO_FSTAT) return pio_fstat(b);
        if (off == PIO_FDEBUG) return b->fdebug;
        if (off == PIO_FLEVEL) return pio_flevel(b);
        return 0u;
    }
    if (off < PIO_RXF0) return 0u; /* TXF is write-only */
    if (off < PIO_IRQ) {
        mm_u32 smi = (off - PIO_RXF0) / 4u;
        struct pio_sm *s = &b->sm[smi];
        mm_u32 v = fifo_peek(&s->rx);
        if (s->rx.count == 0u) {
            if (side_effects) b->fdebug |= 1u << (FDEBUG_RXUNDER_SHIFT + smi);
            return v;
        }
        if (side_effects) (void)fifo_pop(&s->rx, &v);
        return v;
    }
    if (off == PIO_IRQ) return b->irq & 0xffu;
    if (off == PIO_IRQ_FORCE) return 0u;
    if (off == PIO_INPUT_SYNC_BYPASS) return b->input_sync_bypass;
    if (off == PIO_DBG_PADOUT) return (mm_u32)(b->pad_out >> b->gpiobase);
    if (off == PIO_DBG_PADOE) return (mm_u32)(b->pad_oe >> b->gpiobase);
    if (off == PIO_DBG_CFGINFO) {
        return (1u << 28) | (PIO_IMEM_SIZE << 16) | (PIO_SM_COUNT << 8) | PIO_FIFO_DEPTH;
    }
    if (off >= PIO_INSTR_MEM0 && off < PIO_SM0_CLKDIV) return 0u; /* write-only */
    if (off >= PIO_SM0_CLKDIV && off < PIO_RXF0_PUTGET0) {
        mm_u32 smi = (off - PIO_SM0_CLKDIV) / PIO_SM_STRIDE;
        mm_u32 reg = (off - PIO_SM0_CLKDIV) % PIO_SM_STRIDE;
        struct pio_sm *s = &b->sm[smi];
        switch (reg) {
        case PIO_SM_CLKDIV: return s->clkdiv;
        case PIO_SM_EXECCTRL:
            return (s->execctrl & EXECCTRL_WMASK) | (s->stalled ? EXECCTRL_EXEC_STALLED : 0u);
        case PIO_SM_SHIFTCTRL: return s->shiftctrl;
        case PIO_SM_ADDR: return s->pc;
        case PIO_SM_INSTR: return b->imem[s->pc];
        case PIO_SM_PINCTRL: return s->pinctrl;
        default: return 0u;
        }
    }
    if (off >= PIO_RXF0_PUTGET0 && off < PIO_GPIOBASE) {
        mm_u32 idx = (off - PIO_RXF0_PUTGET0) / 4u;
        return b->sm[idx / 4u].rx.buf[idx & 3u];
    }
    if (off == PIO_GPIOBASE) return b->gpiobase;
    if (off == PIO_INTR) return pio_intr(b);
    if (off >= PIO_IRQ0_INTE && off <= PIO_IRQ1_INTE + 8u) {
        mm_u32 k = (off >= PIO_IRQ1_INTE) ? 1u : 0u;
        mm_u32 reg = off - (k ? PIO_IRQ1_INTE : PIO_IRQ0_INTE);
        if (reg == 0u) return b->inte[k];
        if (reg == 4u) return b->intf[k];
        return (pio_intr(b) | b->intf[k]) & b->inte[k];
    }
    return 0u;
}

static void pio_write_ctrl(struct pio_block *b, mm_u32 value)
{
    mm_u32 restart = (value >> 4) & 0xfu;
    mm_u32 clk_restart = (value >> 8) & 0xfu;
    mm_u32 prev_mask = (value >> 16) & 0xfu;
    mm_u32 next_mask = (value >> 20) & 0xfu;
    mm_u32 i;

    b->sm_enable = value & 0xfu;
    for (i = 0; i < PIO_SM_COUNT; ++i) {
        if (restart & (1u << i)) sm_restart(&b->sm[i]);
        if (clk_restart & (1u << i)) b->sm[i].clk_acc = 0u;
    }
    if ((value & ((1u << 24) | (1u << 25) | (1u << 26))) == 0u) return;
    for (i = 0; i < 2u; ++i) {
        struct pio_block *peer = pio_peer(b, (i == 1u) ? MM_TRUE : MM_FALSE);
        mm_u32 mask = (i == 1u) ? next_mask : prev_mask;
        mm_u32 smi;
        if (mask == 0u) continue;
        if (value & (1u << 26)) {
            for (smi = 0; smi < PIO_SM_COUNT; ++smi) {
                if (mask & (1u << smi)) peer->sm[smi].clk_acc = 0u;
            }
        }
        if (value & (1u << 24)) peer->sm_enable |= mask;
        if (value & (1u << 25)) peer->sm_enable &= ~mask;
    }
}

static void sm_write_shiftctrl(struct pio_sm *s, mm_u32 value)
{
    if ((value & ((1u << 14) | (1u << 15))) != 0u) {
        value &= ~((1u << 30) | (1u << 31));
    }
    if (((s->shiftctrl ^ value) & SHIFTCTRL_JOIN_MASK) != 0u) {
        fifo_clear(&s->tx);
        fifo_clear(&s->rx);
    }
    s->shiftctrl = value;
}

static void sm_write_instr(struct pio_block *b, mm_u32 smi, mm_u32 value)
{
    struct pio_sm *s = &b->sm[smi];
    s->exec_instr = (mm_u16)(value & 0xffffu);
    s->exec_valid = 1u;
    s->delay = 0u;
    if ((b->sm_enable & (1u << smi)) == 0u) {
        /* Not clocked: the write takes effect immediately, as in hardware. */
        mm_bool jumped = MM_FALSE;
        pio_pads_snapshot();
        if (sm_exec(b, smi, s->exec_instr, &jumped)) {
            s->exec_valid = 0u;
            s->stalled = 0u;
        } else {
            s->stalled = 1u;
        }
    }
}

static void pio_write32(struct pio_block *b, mm_u32 off, mm_u32 value)
{
    if (off == PIO_CTRL) {
        pio_write_ctrl(b, value);
        return;
    }
    if (off == PIO_FDEBUG) {
        b->fdebug &= ~value;
        return;
    }
    if (off >= PIO_TXF0 && off < PIO_RXF0) {
        mm_u32 smi = (off - PIO_TXF0) / 4u;
        struct pio_sm *s = &b->sm[smi];
        if (!fifo_push(&s->tx, sm_tx_depth(s), value)) {
            b->fdebug |= 1u << (FDEBUG_TXOVER_SHIFT + smi);
        }
        return;
    }
    if (off == PIO_IRQ) {
        b->irq &= ~(value & 0xffu);
        return;
    }
    if (off == PIO_IRQ_FORCE) {
        b->irq |= value & 0xffu;
        return;
    }
    if (off == PIO_INPUT_SYNC_BYPASS) {
        b->input_sync_bypass = value;
        return;
    }
    if (off >= PIO_INSTR_MEM0 && off < PIO_SM0_CLKDIV) {
        b->imem[(off - PIO_INSTR_MEM0) / 4u] = (mm_u16)(value & 0xffffu);
        return;
    }
    if (off >= PIO_SM0_CLKDIV && off < PIO_RXF0_PUTGET0) {
        mm_u32 smi = (off - PIO_SM0_CLKDIV) / PIO_SM_STRIDE;
        mm_u32 reg = (off - PIO_SM0_CLKDIV) % PIO_SM_STRIDE;
        struct pio_sm *s = &b->sm[smi];
        switch (reg) {
        case PIO_SM_CLKDIV: s->clkdiv = value & 0xffffff00u; break;
        case PIO_SM_EXECCTRL: s->execctrl = value & EXECCTRL_WMASK; break;
        case PIO_SM_SHIFTCTRL: sm_write_shiftctrl(s, value); break;
        case PIO_SM_INSTR: sm_write_instr(b, smi, value); break;
        case PIO_SM_PINCTRL: s->pinctrl = value; break;
        default: break;
        }
        return;
    }
    if (off >= PIO_RXF0_PUTGET0 && off < PIO_GPIOBASE) {
        mm_u32 idx = (off - PIO_RXF0_PUTGET0) / 4u;
        b->sm[idx / 4u].rx.buf[idx & 3u] = value;
        return;
    }
    if (off == PIO_GPIOBASE) {
        b->gpiobase = (value & 0x10u) ? 16u : 0u;
        g_pads_dirty = MM_TRUE;
        return;
    }
    if (off >= PIO_IRQ0_INTE && off <= PIO_IRQ1_INTE + 8u) {
        mm_u32 k = (off >= PIO_IRQ1_INTE) ? 1u : 0u;
        mm_u32 reg = off - (k ? PIO_IRQ1_INTE : PIO_IRQ0_INTE);
        if (reg == 0u) b->inte[k] = value & 0xffffu;
        else if (reg == 4u) b->intf[k] = value & 0xffffu;
        return;
    }
}

static mm_bool pio_mmio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct pio_block *b = (struct pio_block *)opaque;
    mm_u32 base_off;
    mm_u32 reg;
    mm_u32 lane;

    if (b == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > PIO_ALIAS_SIZE) return MM_FALSE;
    base_off = offset & 0xfffu;
    reg = base_off & ~3u;
    lane = base_off & 3u;
    if ((lane + size_bytes) > 4u) return MM_FALSE;
    *value_out = read_slice(pio_read32(b, reg, MM_TRUE), lane, size_bytes);
    pio_update_irqs(b);
    return MM_TRUE;
}

static mm_bool pio_mmio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct pio_block *b = (struct pio_block *)opaque;
    mm_u32 base_off;
    mm_u32 alias;
    mm_u32 reg;
    mm_u32 lane;
    mm_u32 mask;
    mm_u32 wdata;

    if (b == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > PIO_ALIAS_SIZE) return MM_FALSE;
    base_off = offset & 0xfffu;
    alias = (offset >> 12) & 0x3u;
    reg = base_off & ~3u;
    lane = base_off & 3u;
    if ((lane + size_bytes) > 4u) return MM_FALSE;

    mask = (size_bytes >= 4u) ? 0xffffffffu : (((1u << (size_bytes * 8u)) - 1u) << (lane * 8u));
    wdata = (size_bytes >= 4u) ? value : ((value << (lane * 8u)) & mask);

    if (alias == 0u) {
        if (mask != 0xffffffffu && !(reg >= PIO_TXF0 && reg < PIO_RXF0)) {
            wdata |= pio_read32(b, reg, MM_FALSE) & ~mask;
        }
    } else {
        mm_u32 cur = pio_read32(b, reg, MM_FALSE);
        if (alias == 1u) wdata = cur ^ wdata;
        else if (alias == 2u) wdata = cur | wdata;
        else wdata = cur & ~wdata;
    }
    pio_write32(b, reg, wdata);
    pio_update_irqs(b);
    return MM_TRUE;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

void mm_rp2350_pio_reset(void)
{
    mm_u32 i;
    mm_u32 smi;

    if (!g_trace_checked) {
        g_pio_trace = (getenv("M33MU_PIO_TRACE") != 0) ? MM_TRUE : MM_FALSE;
        g_trace_checked = MM_TRUE;
    }
    memset(g_pio, 0, sizeof(g_pio));
    for (i = 0; i < PIO_COUNT; ++i) {
        g_pio[i].index = i;
        for (smi = 0; smi < PIO_SM_COUNT; ++smi) {
            struct pio_sm *s = &g_pio[i].sm[smi];
            s->clkdiv = 0x00010000u;
            s->execctrl = 0x0001f000u;
            s->shiftctrl = 0x000c0000u;
            s->pinctrl = 0x14000000u;
            s->osr_count = 32u;
        }
    }
    for (i = 0; i < PIO_COUNT; ++i) g_pio_owner[i] = 0u;
    g_sio_out = 0u;
    g_sio_oe = 0u;
    g_pad_level = 0u;
    g_pads_dirty = MM_TRUE;
}

void mm_rp2350_pio_bind_nvic(struct mm_nvic *nvic)
{
    g_pio_nvic = nvic;
}

mm_bool mm_rp2350_pio_register(struct mmio_bus *bus)
{
    struct mmio_region reg;
    mm_u32 i;

    if (bus == 0) return MM_FALSE;
    mm_rp2350_pio_reset();
    for (i = 0; i < PIO_COUNT; ++i) {
        memset(&reg, 0, sizeof(reg));
        reg.size = PIO_ALIAS_SIZE;
        reg.base = PIO0_BASE + i * PIO_INSTANCE_STRIDE;
        reg.opaque = &g_pio[i];
        reg.read = pio_mmio_read;
        reg.write = pio_mmio_write;
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    }
    return MM_TRUE;
}

/* Bound so a long idle skip cannot turn into an unbounded loop. */
#define PIO_MAX_TICK_CYCLES  (1u << 20)
#define PIO_DREQ_MAX_CYCLES  (1u << 20)

static mm_u32 pio_active_mask(void)
{
    mm_u32 mask = 0u;
    mm_u32 i;
    for (i = 0; i < PIO_COUNT; ++i) {
        if (g_pio[i].sm_enable != 0u && !pio_in_reset(&g_pio[i])) mask |= 1u << i;
    }
    return mask;
}

static void pio_run_cycles(mm_u64 cycles, mm_u32 active)
{
    mm_u64 n;
    mm_u32 i;
    mm_u32 smi;

    for (n = 0; n < cycles; ++n) {
        for (i = 0; i < PIO_COUNT; ++i) {
            struct pio_block *b;
            if ((active & (1u << i)) == 0u) continue;
            b = &g_pio[i];
            for (smi = 0; smi < PIO_SM_COUNT; ++smi) {
                if (!sm_clock_tick(&b->sm[smi])) continue;
                if ((b->sm_enable & (1u << smi)) == 0u) continue;
                sm_cycle(b, smi);
            }
        }
    }
}

void mm_rp2350_pio_tick(mm_u64 cycles)
{
    mm_u32 active;
    mm_u32 i;

    if (!mm_rp2350_active() || cycles == 0u) return;
    active = pio_active_mask();
    if (active == 0u) return;
    if (cycles > PIO_MAX_TICK_CYCLES) cycles = PIO_MAX_TICK_CYCLES;
    pio_pads_snapshot();
    pio_run_cycles(cycles, active);
    for (i = 0; i < PIO_COUNT; ++i) pio_update_irqs(&g_pio[i]);
}

mm_bool mm_rp2350_pio_dreq_wait(mm_u32 addr, mm_bool is_write)
{
    struct pio_block *b = 0;
    struct pio_sm *s;
    mm_u32 off = 0u;
    mm_u32 smi;
    mm_u32 active;
    mm_u32 guard;

    for (smi = 0; smi < PIO_COUNT; ++smi) {
        mm_u32 base = PIO0_BASE + smi * PIO_INSTANCE_STRIDE;
        if (addr >= base && addr < base + PIO_REG_SIZE) {
            b = &g_pio[smi];
            off = addr - base;
            break;
        }
    }
    if (b == 0) return MM_TRUE;
    if (is_write) {
        if (off < PIO_TXF0 || off >= PIO_RXF0) return MM_TRUE;
        smi = (off - PIO_TXF0) / 4u;
    } else {
        if (off < PIO_RXF0 || off >= PIO_IRQ) return MM_TRUE;
        smi = (off - PIO_RXF0) / 4u;
    }
    s = &b->sm[smi];
    active = pio_active_mask();
    if ((active & (1u << b->index)) == 0u || (b->sm_enable & (1u << smi)) == 0u) {
        /* Nothing is draining the FIFO; behave as an unpaced transfer. */
        return MM_TRUE;
    }
    pio_pads_snapshot();
    for (guard = 0; guard < PIO_DREQ_MAX_CYCLES; ++guard) {
        if (is_write) {
            if ((mm_u32)s->tx.count < sm_tx_depth(s)) break;
        } else {
            if (s->rx.count != 0u) break;
        }
        pio_run_cycles(1u, active);
    }
    for (smi = 0; smi < PIO_COUNT; ++smi) pio_update_irqs(&g_pio[smi]);
    if (is_write) return ((mm_u32)s->tx.count < sm_tx_depth(s)) ? MM_TRUE : MM_FALSE;
    return (s->rx.count != 0u) ? MM_TRUE : MM_FALSE;
}
