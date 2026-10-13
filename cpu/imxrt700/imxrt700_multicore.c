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
#include <string.h>
#include "imxrt700/imxrt700_multicore.h"
#include "imxrt700/imxrt700_mmio.h"
#include "imxrt700/imxrt700_secure.h"
#include "imxrt700/imxrt700_timers.h"
#include "m33mu/cpu.h"
#include "m33mu/memmap.h"
#include "m33mu/nvic.h"

/* SYSCON3 */
#define SYSCON3_CPU_STATUS 0x08Cu
#define SYSCON3_CPU1_SVTOR 0x098u
#define SYSCON3_CPU1_NSVTOR 0x09Cu
#define SYSCON3_GRAY_CODE_LSB 0xB60u
#define SYSCON3_GRAY_CODE_MSB 0xB64u
#define SYSCON3_BINARY_CODE_LSB 0xB68u
#define SYSCON3_BINARY_CODE_MSB 0xB6Cu
#define CPU_STATUS_CPU_WAIT (1u << 0)
#define CPU_STATUS_LOCKUP (1u << 1)

/* GLIKEY4 index guarding the CPU1 vector table registers. */
#define GLIKEY4_IDX_CPU1_VTOR 1u

#define CLKCTL3_PSCCTL0_COMP 0x010u
#define CLKCTL3_CPU1_BIT 0u
#define RSTCTL3_PRSTCTL0 0x010u
#define RSTCTL3_CPU1_BIT 31u

struct mc_state {
    struct mm_cpu *cpu[2];
    struct mm_nvic *nvic[2];
    mm_u32 *active_core;
    struct mm_memmap *map;
    mm_bool running;
    mm_bool needs_boot;     /* next release starts from the vector table */
    mm_bool launch_pending;
    mm_u32 launch_vtor;
    mm_u32 launch_sp;
    mm_u32 launch_entry;
};

static struct mc_state mc;
static struct imxrt700_dev *syscon3;
static struct imxrt700_dev *clkctl3;
static struct imxrt700_dev *rstctl3;
static imxrt700_write_hook clkctl3_prev_write;

/* ------------------------------------------------------------------------ */
/* CPU1 run control                                                         */
/* ------------------------------------------------------------------------ */

static mm_bool core1_released(void)
{
    if (syscon3 == 0 || clkctl3 == 0 || rstctl3 == 0) {
        return MM_FALSE;
    }
    if ((mm_imxrt700_reg(clkctl3, CLKCTL3_PSCCTL0_COMP) & (1u << CLKCTL3_CPU1_BIT)) == 0u) {
        return MM_FALSE;
    }
    if ((mm_imxrt700_reg(rstctl3, RSTCTL3_PRSTCTL0) & (1u << RSTCTL3_CPU1_BIT)) != 0u) {
        return MM_FALSE;
    }
    return (mm_imxrt700_reg(syscon3, SYSCON3_CPU_STATUS) & CPU_STATUS_CPU_WAIT) == 0u ? MM_TRUE : MM_FALSE;
}

static void mc_update(void)
{
    mm_bool rel = core1_released();

    if (rstctl3 != 0 && (mm_imxrt700_reg(rstctl3, RSTCTL3_PRSTCTL0) & (1u << RSTCTL3_CPU1_BIT)) != 0u) {
        mc.needs_boot = MM_TRUE;
    }
    if (rel && !mc.running) {
        if (mc.needs_boot) {
            mm_u32 vtor = mm_imxrt700_reg(syscon3, SYSCON3_CPU1_SVTOR) << 7;
            mm_u32 sp = 0;
            mm_u32 entry = 0;
            if (mc.map == 0 ||
                !mm_memmap_read(mc.map, MM_SECURE, vtor, 4u, &sp) ||
                !mm_memmap_read(mc.map, MM_SECURE, vtor + 4u, 4u, &entry)) {
                fprintf(stderr, "[IMXRT700] CPU1 release: vector table at 0x%08lx unreadable\n",
                        (unsigned long)vtor);
                return;
            }
            printf("[IMXRT700] CPU1 start vtor=0x%08lx sp=0x%08lx entry=0x%08lx\n",
                   (unsigned long)vtor, (unsigned long)sp, (unsigned long)entry);
            mc.launch_vtor = vtor;
            mc.launch_sp = sp;
            mc.launch_entry = entry & ~1u;
            mc.launch_pending = MM_TRUE;
            mc.needs_boot = MM_FALSE;
        }
        mc.running = MM_TRUE;
    } else if (!rel && mc.running) {
        mc.running = MM_FALSE;
    }
}

static mm_bool syscon3_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    if (size != 4u) {
        return MM_FALSE;
    }
    switch (off) {
    case SYSCON3_CPU_STATUS: {
        mm_u32 cur = mm_imxrt700_reg(dev, off);
        mm_imxrt700_reg_put(dev, off, (cur & ~CPU_STATUS_CPU_WAIT) | (value & CPU_STATUS_CPU_WAIT));
        mc_update();
        return MM_TRUE;
    }
    case SYSCON3_CPU1_SVTOR:
    case SYSCON3_CPU1_NSVTOR:
        if (!mm_imxrt700_glikey_write_enabled(4u, GLIKEY4_IDX_CPU1_VTOR)) {
            return MM_TRUE; /* write-protected: ignored */
        }
        mm_imxrt700_reg_put(dev, off, value & 0x01FFFFFFu);
        return MM_TRUE;
    case SYSCON3_GRAY_CODE_LSB:
    case SYSCON3_GRAY_CODE_MSB: {
        mm_u64 g;
        mm_u64 b;
        mm_imxrt700_reg_put(dev, off, value);
        g = ((mm_u64)(mm_imxrt700_reg(dev, SYSCON3_GRAY_CODE_MSB) & 0x3FFu) << 32) |
            mm_imxrt700_reg(dev, SYSCON3_GRAY_CODE_LSB);
        b = mm_imxrt700_gray_decode(g);
        mm_imxrt700_reg_put(dev, SYSCON3_BINARY_CODE_LSB, (mm_u32)b);
        mm_imxrt700_reg_put(dev, SYSCON3_BINARY_CODE_MSB, (mm_u32)(b >> 32) & 0x3FFu);
        return MM_TRUE;
    }
    case SYSCON3_BINARY_CODE_LSB:
    case SYSCON3_BINARY_CODE_MSB:
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

static mm_bool syscon3_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    if (off == SYSCON3_CPU_STATUS && size == 4u) {
        /* LOCKUP is not modelled: a locked-up core is reported as a fault. */
        *value_out = mm_imxrt700_reg(dev, off) & ~CPU_STATUS_LOCKUP;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool rstctl3_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    if (!mm_imxrt700_regfile_write(dev, off, size, value)) {
        return MM_FALSE;
    }
    mc_update();
    return MM_TRUE;
}

static mm_bool clkctl3_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    mm_bool ok = MM_FALSE;
    if (clkctl3_prev_write != 0) {
        ok = clkctl3_prev_write(dev, off, size, value);
    }
    if (!ok && !mm_imxrt700_regfile_write(dev, off, size, value)) {
        return MM_FALSE;
    }
    mc_update();
    return MM_TRUE;
}

/* ------------------------------------------------------------------------ */
/* MU1: CPU0 side A <-> CPU1 side B                                          */
/* ------------------------------------------------------------------------ */

#define MU_VER 0x000u
#define MU_PAR 0x004u
#define MU_CR 0x008u
#define MU_SR 0x00Cu
#define MU_FCR 0x100u
#define MU_FSR 0x104u
#define MU_GIER 0x110u
#define MU_GCR 0x114u
#define MU_GSR 0x118u
#define MU_TCR 0x120u
#define MU_TSR 0x124u
#define MU_RCR 0x128u
#define MU_RSR 0x12Cu
#define MU_TR0 0x200u
#define MU_RR0 0x280u
#define MU_CHANNELS 4u

#define MU_SR_GIRP (1u << 4)
#define MU_SR_TEP (1u << 5)
#define MU_SR_RFP (1u << 6)

struct mu_side {
    const char *name;
    int core;
    int irq;
    struct imxrt700_dev *dev;
    mm_u32 rx_full;          /* RSR.RFn */
    mm_u32 rr[MU_CHANNELS];  /* data the peer sent */
    mm_u32 gsr;              /* general-purpose interrupt pending */
};

static struct mu_side mu1[2] = {
    { "MU1_MUA", IMXRT700_CPU0, 30, 0, 0, { 0, 0, 0, 0 }, 0 },
    { "MU1_MUB", IMXRT700_CPU1, 26, 0, 0, { 0, 0, 0, 0 }, 0 },
};

static struct mu_side *mu_peer(struct mu_side *s)
{
    return (s == &mu1[0]) ? &mu1[1] : &mu1[0];
}

static mm_u32 mu_tsr(struct mu_side *s)
{
    /* A transmit register is empty once the peer has read it. */
    return (~mu_peer(s)->rx_full) & 0xFu;
}

static mm_u32 mu_pending(struct mu_side *s)
{
    mm_u32 tcr = mm_imxrt700_reg(s->dev, MU_TCR) & 0xFu;
    mm_u32 rcr = mm_imxrt700_reg(s->dev, MU_RCR) & 0xFu;
    mm_u32 gier = mm_imxrt700_reg(s->dev, MU_GIER) & 0xFu;
    return (mu_tsr(s) & tcr) | (s->rx_full & rcr) | (s->gsr & gier);
}

static void mu_irq_update(void)
{
    mm_u32 i;
    for (i = 0; i < 2u; ++i) {
        if (mu1[i].dev != 0 && mu_pending(&mu1[i]) != 0u) {
            mm_imxrt700_irq_set(mu1[i].core, mu1[i].irq, MM_TRUE);
        }
    }
}

static mm_bool mu_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    struct mu_side *s = (struct mu_side *)dev->opaque;
    struct mu_side *p = mu_peer(s);
    mm_u32 v;
    if (size != 4u) {
        return MM_FALSE;
    }
    if (off >= MU_RR0 && off < MU_RR0 + 4u * MU_CHANNELS) {
        mm_u32 ch = (off - MU_RR0) / 4u;
        *value_out = s->rr[ch];
        if (!mmio_peek_mode()) {
            s->rx_full &= ~(1u << ch);
            mu_irq_update();
        }
        return MM_TRUE;
    }
    switch (off) {
    case MU_TSR:
        *value_out = mu_tsr(s);
        return MM_TRUE;
    case MU_RSR:
        *value_out = s->rx_full;
        return MM_TRUE;
    case MU_GSR:
        *value_out = s->gsr;
        return MM_TRUE;
    case MU_GCR:
        /* A request stays set until the peer acknowledges it. */
        *value_out = p->gsr & mm_imxrt700_reg(dev, MU_GCR);
        return MM_TRUE;
    case MU_FSR:
        *value_out = mm_imxrt700_reg(p->dev, MU_FCR) & 0x7u;
        return MM_TRUE;
    case MU_SR:
        v = 0u;
        if (mu_tsr(s) & mm_imxrt700_reg(dev, MU_TCR)) v |= MU_SR_TEP;
        if (s->rx_full & mm_imxrt700_reg(dev, MU_RCR)) v |= MU_SR_RFP;
        if (p->gsr & mm_imxrt700_reg(dev, MU_GCR)) v |= MU_SR_GIRP;
        *value_out = v;
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

static mm_bool mu_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    struct mu_side *s = (struct mu_side *)dev->opaque;
    struct mu_side *p = mu_peer(s);
    if (size != 4u) {
        return MM_FALSE;
    }
    if (off >= MU_TR0 && off < MU_TR0 + 4u * MU_CHANNELS) {
        mm_u32 ch = (off - MU_TR0) / 4u;
        p->rr[ch] = value;
        p->rx_full |= 1u << ch;
        mu_irq_update();
        return MM_TRUE;
    }
    switch (off) {
    case MU_GCR:
        mm_imxrt700_reg_put(dev, MU_GCR, value & 0xFu);
        p->gsr |= value & 0xFu;
        mu_irq_update();
        return MM_TRUE;
    case MU_GSR:
        s->gsr &= ~value;
        mu_irq_update();
        return MM_TRUE;
    case MU_CR:
        if (value & 1u) {
            /* MUR: reset both sides of the unit. */
            s->rx_full = 0;
            p->rx_full = 0;
            s->gsr = 0;
            p->gsr = 0;
            return MM_TRUE;
        }
        return MM_FALSE;
    case MU_TSR:
    case MU_RSR:
    case MU_FSR:
    case MU_SR:
    case MU_VER:
    case MU_PAR:
        return MM_TRUE;
    case MU_TCR:
    case MU_RCR:
    case MU_GIER:
        mm_imxrt700_reg_put(dev, off, value);
        mu_irq_update();
        return MM_TRUE;
    default:
        if (off >= MU_RR0 && off < MU_RR0 + 4u * MU_CHANNELS) {
            return MM_TRUE;
        }
        return MM_FALSE;
    }
}

/* ------------------------------------------------------------------------ */
/* Target multicore ops                                                     */
/* ------------------------------------------------------------------------ */

static void mc_bind(struct mm_cpu *core0,
                    struct mm_cpu *core1,
                    struct mm_nvic *nvic0,
                    struct mm_nvic *nvic1,
                    mm_u32 *active_core,
                    struct mm_memmap *map)
{
    mc.cpu[0] = core0;
    mc.cpu[1] = core1;
    mc.nvic[0] = nvic0;
    mc.nvic[1] = nvic1;
    mc.active_core = active_core;
    mc.map = map;
    mm_imxrt700_bind_nvic(IMXRT700_CPU0, nvic0);
    mm_imxrt700_bind_nvic(IMXRT700_CPU1, nvic1);
}

static void mc_set_active_core(mm_u32 core_id)
{
    mm_imxrt700_secure_set_active_core(core_id);
}

static mm_bool mc_core1_running(void)
{
    return mc.running;
}

static mm_bool mc_core1_can_reset(void)
{
    return MM_TRUE;
}

static mm_bool mc_core1_take_launch(mm_u32 *vtor_out, mm_u32 *sp_out, mm_u32 *entry_out)
{
    if (!mc.launch_pending) {
        return MM_FALSE;
    }
    mc.launch_pending = MM_FALSE;
    if (vtor_out) *vtor_out = mc.launch_vtor;
    if (sp_out) *sp_out = mc.launch_sp;
    if (entry_out) *entry_out = mc.launch_entry;
    return MM_TRUE;
}

const struct mm_target_mc_ops mm_imxrt700_mc_ops = {
    mc_bind,
    mc_set_active_core,
    mc_core1_running,
    mc_core1_can_reset,
    mc_core1_take_launch
};

void mm_imxrt700_mc_attach(void)
{
    mm_u32 i;
    syscon3 = mm_imxrt700_dev("SYSCON3");
    rstctl3 = mm_imxrt700_dev("RSTCTL3");
    clkctl3 = mm_imxrt700_dev("CLKCTL3");
    (void)mm_imxrt700_dev_hook("SYSCON3", syscon3_read, syscon3_write, 0);
    (void)mm_imxrt700_dev_hook("RSTCTL3", 0, rstctl3_write, 0);
    if (clkctl3 != 0 && clkctl3->write != clkctl3_write) {
        clkctl3_prev_write = clkctl3->write;
        clkctl3->write = clkctl3_write;
    }
    for (i = 0; i < 2u; ++i) {
        mu1[i].dev = mm_imxrt700_dev(mu1[i].name);
        (void)mm_imxrt700_dev_hook(mu1[i].name, mu_read, mu_write, &mu1[i]);
    }
}

void mm_imxrt700_mc_reset(void)
{
    mm_u32 i;
    mc.running = MM_FALSE;
    mc.needs_boot = MM_TRUE;
    mc.launch_pending = MM_FALSE;
    for (i = 0; i < 2u; ++i) {
        mu1[i].rx_full = 0;
        mu1[i].gsr = 0;
        memset(mu1[i].rr, 0, sizeof(mu1[i].rr));
    }
}

void mm_imxrt700_mc_poll(void)
{
    mu_irq_update();
}
