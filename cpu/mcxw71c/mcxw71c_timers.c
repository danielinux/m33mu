/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include "mcxw71c/mcxw71c_timers.h"
#include "mcxw71c/mcxw71c_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

#define LPIT0_BASE 0x4002F000u
#define LPIT_SIZE  0x1000u

#define LPIT_MCR    0x08u
#define LPIT_MSR    0x0Cu
#define LPIT_MIER   0x10u
#define LPIT_SETTEN 0x14u
#define LPIT_CLRTEN 0x18u

#define LPIT_CH_OFF   0x20u
#define LPIT_CH_STRIDE 0x10u
#define LPIT_TVAL     0x00u
#define LPIT_CVAL     0x04u
#define LPIT_TCTRL    0x08u

#define MCR_M_CEN (1u << 0)
#define MSR_TIF_MASK 0x0Fu
#define MIER_TIE_MASK 0x0Fu

#define TCTRL_T_EN  (1u << 0)
#define TCTRL_CHAIN (1u << 1)
#define TCTRL_MODE_MASK (0x3u << 2)

struct lpit_state {
    mm_u32 regs[LPIT_SIZE / 4];
    mm_u32 tval[4];
    mm_u32 cval[4];
    mm_u32 tctrl[4];
};

static struct lpit_state lpit0;
static struct mm_nvic *g_nvic = 0;

static mm_u32 lpit_get_ch_index(mm_u32 offset)
{
    if (offset < LPIT_CH_OFF) return 0xffffffffu;
    offset -= LPIT_CH_OFF;
    if (offset >= LPIT_CH_STRIDE * 4u) return 0xffffffffu;
    return offset / LPIT_CH_STRIDE;
}

static mm_bool lpit_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct lpit_state *l = (struct lpit_state *)opaque;
    if (l == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_LPIT0) ||
        !mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_LPIT0)) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > LPIT_SIZE) return MM_FALSE;
    if (size_bytes == 4) {
        mm_u32 ch = lpit_get_ch_index(offset);
        if (ch != 0xffffffffu) {
            mm_u32 coff = offset - (LPIT_CH_OFF + ch * LPIT_CH_STRIDE);
            if (coff == LPIT_TVAL) {
                *value_out = l->tval[ch];
                return MM_TRUE;
            }
            if (coff == LPIT_CVAL) {
                *value_out = l->cval[ch];
                return MM_TRUE;
            }
            if (coff == LPIT_TCTRL) {
                *value_out = l->tctrl[ch];
                return MM_TRUE;
            }
        }
    }
    memcpy(value_out, (mm_u8 *)l->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool lpit_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct lpit_state *l = (struct lpit_state *)opaque;
    if (l == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_LPIT0) ||
        !mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_LPIT0)) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > LPIT_SIZE) return MM_FALSE;
    if (size_bytes == 4) {
        mm_u32 ch = lpit_get_ch_index(offset);
        if (ch != 0xffffffffu) {
            mm_u32 coff = offset - (LPIT_CH_OFF + ch * LPIT_CH_STRIDE);
            if (coff == LPIT_TVAL) {
                l->tval[ch] = value;
                if ((l->tctrl[ch] & TCTRL_T_EN) == 0u) {
                    l->cval[ch] = value;
                }
                return MM_TRUE;
            }
            if (coff == LPIT_TCTRL) {
                l->tctrl[ch] = value;
                if ((value & TCTRL_T_EN) != 0u && l->cval[ch] == 0u) {
                    l->cval[ch] = l->tval[ch];
                }
                return MM_TRUE;
            }
        }
        if (offset == LPIT_MSR) {
            l->regs[LPIT_MSR / 4] &= ~(value & MSR_TIF_MASK);
            return MM_TRUE;
        }
        if (offset == LPIT_SETTEN) {
            mm_u32 set = value & 0x0Fu;
            int i;
            for (i = 0; i < 4; ++i) {
                if ((set >> i) & 1u) {
                    l->tctrl[i] |= TCTRL_T_EN;
                    if (l->cval[i] == 0u) l->cval[i] = l->tval[i];
                }
            }
            return MM_TRUE;
        }
        if (offset == LPIT_CLRTEN) {
            mm_u32 clr = value & 0x0Fu;
            int i;
            for (i = 0; i < 4; ++i) {
                if ((clr >> i) & 1u) {
                    l->tctrl[i] &= ~TCTRL_T_EN;
                }
            }
            return MM_TRUE;
        }
    }
    memcpy((mm_u8 *)l->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

void mm_mcxw71c_timers_tick(mm_u64 cycles)
{
    mm_u32 mcr = lpit0.regs[LPIT_MCR / 4];
    mm_u32 mcr_en = mcr & MCR_M_CEN;
    mm_u32 msr = lpit0.regs[LPIT_MSR / 4];
    mm_u32 mier = lpit0.regs[LPIT_MIER / 4] & MIER_TIE_MASK;
    mm_u32 step = (cycles > 0xffffffffu) ? 0xffffffffu : (mm_u32)cycles;
    int i;

    if (!mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_LPIT0) ||
        !mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_LPIT0)) {
        return;
    }
    if (mcr_en == 0u || step == 0u) return;

    for (i = 0; i < 4; ++i) {
        if ((lpit0.tctrl[i] & TCTRL_T_EN) == 0u) continue;
        if ((lpit0.tctrl[i] & TCTRL_CHAIN) != 0u) continue;
        if (lpit0.cval[i] > step) {
            lpit0.cval[i] -= step;
        } else {
            lpit0.cval[i] = lpit0.tval[i];
            msr |= (1u << i);
        }
    }

    lpit0.regs[LPIT_MSR / 4] = msr;
    if (g_nvic != 0 && (msr & mier) != 0u) {
        mm_nvic_set_pending(g_nvic, 36u, MM_TRUE);
    }
}

void mm_mcxw71c_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    struct mmio_region reg;
    g_nvic = nvic;
    mm_mcxw71c_gpio_set_nvic(nvic);
    memset(&lpit0, 0, sizeof(lpit0));
    reg.base = LPIT0_BASE;
    reg.size = LPIT_SIZE;
    reg.opaque = &lpit0;
    reg.read = lpit_read;
    reg.write = lpit_write;
    mmio_bus_register_region(bus, &reg);
    reg.base = LPIT0_BASE + 0x10000000u;
    mmio_bus_register_region(bus, &reg);
}

void mm_mcxw71c_timers_reset(void)
{
    memset(&lpit0, 0, sizeof(lpit0));
    g_nvic = 0;
}
