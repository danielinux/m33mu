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

#include "m33mu/mem_prot.h"
#include "m33mu/mpu.h"
#include "m33mu/sau.h"
#include <stdlib.h>
#include <stdio.h>

#define SFSR_INVEP     (1u << 0)
#define SFSR_AUVIOL    (1u << 3)
#define SFSR_SFARVALID (1u << 6)

static mm_bool range_contains(const struct mm_prot_region *r, mm_u32 addr, mm_u32 size);

#define SAU_CTRL_ENABLE 0x1u
#define SAU_CTRL_ALLNS  0x2u
#define SAU_RLAR_ENABLE 0x1u
#define SAU_RLAR_NSC    0x2u

static mm_bool sau_region_matches_raw(mm_u32 rbar, mm_u32 rlar, mm_u32 addr)
{
    mm_u32 base;
    mm_u32 limit;

    if ((rlar & SAU_RLAR_ENABLE) == 0u) {
        return MM_FALSE;
    }
    base = rbar & ~0x1Fu;
    limit = (rlar & ~0x1Fu) | 0x1Fu;
    if (addr < base) {
        return MM_FALSE;
    }
    return addr <= limit;
}

static int sau_match_index(const struct mm_scs *scs, mm_u32 addr, mm_u32 *rbar_out, mm_u32 *rlar_out)
{
    int i;
    mm_bool enable;

    if (scs == 0) {
        return -1;
    }
    enable = (scs->sau_ctrl & SAU_CTRL_ENABLE) != 0u;
    if (!enable) {
        return -1;
    }

    for (i = 7; i >= 0; --i) {
        mm_u32 rbar = scs->sau_rbar[i];
        mm_u32 rlar = scs->sau_rlar[i];
        if (sau_region_matches_raw(rbar, rlar, addr)) {
            if (rbar_out) *rbar_out = rbar;
            if (rlar_out) *rlar_out = rlar;
            return i;
        }
    }

    return -1;
}

static int prot_trace_level(void)
{
    static mm_bool init = MM_FALSE;
    static int level = 0;
    const char *env;

    if (init) {
        return level;
    }
    env = getenv("M33MU_PROT_TRACE");
    if (env != 0 && env[0] != '\0') {
        /* Accept "1", "2", ...; any non-empty non-number enables level 1. */
        char *endp = 0;
        unsigned long v = strtoul(env, &endp, 0);
        if (endp != env) {
            level = (int)v;
        } else {
            level = 1;
        }
        if (level <= 0) {
            level = 1;
        }
    }
    init = MM_TRUE;
    return level;
}

static const char *sec_name(enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? "NS" : "S";
}

static const char *type_name(enum mm_access_type type)
{
    switch (type) {
    case MM_ACCESS_READ: return "READ";
    case MM_ACCESS_WRITE: return "WRITE";
    case MM_ACCESS_EXEC: return "EXEC";
    default: return "?";
    }
}

static mm_bool mpcbb_attr_for_addr(const struct mm_prot_ctx *ctx,
                                   mm_u32 addr,
                                   enum mm_sau_attr *attr_out,
                                   enum mm_sec_state *sec_out)
{
    const struct mm_target_cfg *cfg;
    mm_u32 i;
    if (ctx == 0 || attr_out == 0 || sec_out == 0) {
        return MM_FALSE;
    }
    cfg = ctx->cfg;
    if (cfg == 0 || cfg->mpcbb_block_secure == 0 || cfg->ram_regions == 0 ||
        cfg->ram_region_count == 0u || cfg->mpcbb_block_size == 0u) {
        return MM_FALSE;
    }
    for (i = 0; i < cfg->ram_region_count; ++i) {
        const struct mm_ram_region *r = &cfg->ram_regions[i];
        if (addr >= r->base_s && (addr - r->base_s) < r->size) {
            mm_u32 block = (addr - r->base_s) / cfg->mpcbb_block_size;
            mm_bool sec = cfg->mpcbb_block_secure(r->mpcbb_index, block);
            *sec_out = sec ? MM_SECURE : MM_NONSECURE;
            *attr_out = sec ? MM_SAU_SECURE : MM_SAU_NONSECURE;
            return MM_TRUE;
        }
        if (addr >= r->base_ns && (addr - r->base_ns) < r->size) {
            mm_u32 block = (addr - r->base_ns) / cfg->mpcbb_block_size;
            mm_bool sec = cfg->mpcbb_block_secure(r->mpcbb_index, block);
            *sec_out = sec ? MM_SECURE : MM_NONSECURE;
            *attr_out = sec ? MM_SAU_SECURE : MM_SAU_NONSECURE;
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static const char *attr_name(enum mm_sau_attr attr)
{
    switch (attr) {
    case MM_SAU_SECURE: return "S";
    case MM_SAU_NONSECURE: return "NS";
    case MM_SAU_NSC: return "NSC";
    default: return "?";
    }
}

static void prot_trace_req(const struct mm_prot_ctx *ctx,
                           enum mm_access_type type,
                           enum mm_sec_state sec,
                           mm_u32 addr,
                           mm_u32 size_bytes,
                           enum mm_sau_attr attr,
                           enum mm_sec_state addr_sec,
                           mm_bool ignore_addr_sec)
{
    int lvl;
    int sau_idx = -1;
    mm_u32 rbar = 0;
    mm_u32 rlar = 0;
    if (ctx == 0 || ctx->scs == 0) {
        return;
    }
    lvl = prot_trace_level();
    if (lvl < 2) {
        return;
    }
    sau_idx = sau_match_index(ctx->scs, addr, &rbar, &rlar);
    printf("[PROT_REQ] sec=%s type=%s addr=0x%08lx size=%lu sau_ctrl=0x%08lx attr=%s addr_sec=%s ignore_addr_sec=%d\n",
           sec_name(sec),
           type_name(type),
           (unsigned long)addr,
           (unsigned long)size_bytes,
           (unsigned long)ctx->scs->sau_ctrl,
           attr_name(attr),
           sec_name(addr_sec),
           (int)ignore_addr_sec);
    printf("[PROT_REQ]  sau_match=%d rbar=0x%08lx rlar=0x%08lx (EN=%d NSC=%d ALLNS=%d)\n",
           sau_idx,
           (unsigned long)rbar,
           (unsigned long)rlar,
           (int)((rlar & SAU_RLAR_ENABLE) != 0u),
           (int)((rlar & SAU_RLAR_NSC) != 0u),
           (int)((ctx->scs->sau_ctrl & SAU_CTRL_ALLNS) != 0u));
}

static void prot_trace_region_scan(const struct mm_prot_ctx *ctx,
                                   enum mm_sec_state sec,
                                   mm_u32 addr,
                                   mm_u32 size_bytes,
                                   enum mm_sec_state addr_sec,
                                   mm_bool ignore_addr_sec,
                                   mm_u32 needed)
{
    int lvl;
    size_t i;
    if (ctx == 0) {
        return;
    }
    lvl = prot_trace_level();
    if (lvl < 3) {
        return;
    }
    printf("[PROT_SCAN] need=0x%lx sec=%s addr=0x%08lx size=%lu addr_sec=%s ignore_addr_sec=%d regions=%lu\n",
           (unsigned long)needed,
           sec_name(sec),
           (unsigned long)addr,
           (unsigned long)size_bytes,
           sec_name(addr_sec),
           (int)ignore_addr_sec,
           (unsigned long)ctx->count);
    for (i = 0; i < ctx->count; ++i) {
        const struct mm_prot_region *r = &ctx->regions[i];
        mm_bool contains = range_contains(r, addr, size_bytes);
        mm_bool sec_ok = ignore_addr_sec ? MM_TRUE : (r->sec == addr_sec);
        mm_bool perm_ok = ((r->perms & needed) != 0u) ? MM_TRUE : MM_FALSE;
        printf("[PROT_SCAN]  #%lu base=0x%08lx size=0x%08lx perms=0x%02x rsec=%s contains=%d sec_ok=%d perm_ok=%d\n",
               (unsigned long)i,
               (unsigned long)r->base,
               (unsigned long)r->size,
               (unsigned)r->perms,
               sec_name(r->sec),
               (int)contains,
               (int)sec_ok,
               (int)perm_ok);
    }
}

static mm_bool range_contains(const struct mm_prot_region *r, mm_u32 addr, mm_u32 size)
{
    if (r == 0) {
        return MM_FALSE;
    }
    if (size == 0u) {
        return MM_FALSE;
    }
    if (addr < r->base) {
        return MM_FALSE;
    }
    return (addr - r->base) + size <= r->size;
}

static void record_securefault(struct mm_prot_ctx *ctx, enum mm_access_type type, mm_u32 addr)
{
    mm_u32 bits;
    enum mm_sau_attr attr;

    if (ctx == 0 || ctx->scs == 0) {
        return;
    }

    attr = mm_sau_attr_for_addr(ctx->scs, addr);
    bits = SFSR_SFARVALID;
    if (type == MM_ACCESS_EXEC) {
        bits |= SFSR_INVEP;
    } else {
        bits |= SFSR_AUVIOL;
    }
    ctx->scs->sau_sfsr |= bits;
    ctx->scs->sau_sfar = addr;
    ctx->scs->securefault_pending = MM_TRUE;
    if (prot_trace_level() >= 1) {
        printf("[PROT_DENY] sec=NS type=%d addr=0x%08lx attr=%d sau_ctrl=0x%08lx -> SecureFault sfsr=0x%08lx sfar=0x%08lx\n",
               (int)type,
               (unsigned long)addr,
               (int)attr,
               (unsigned long)ctx->scs->sau_ctrl,
               (unsigned long)ctx->scs->sau_sfsr,
               (unsigned long)ctx->scs->sau_sfar);
    }
}

static void record_memfault(struct mm_prot_ctx *ctx, enum mm_sec_state sec, enum mm_access_type type, mm_u32 addr)
{
    mm_u32 bits;
    enum mm_sau_attr attr;

    if (ctx == 0 || ctx->scs == 0) {
        return;
    }

    attr = mm_sau_attr_for_addr(ctx->scs, addr);
    bits = (type == MM_ACCESS_EXEC) ? 0x1u : 0x2u; /* IACCVIOL or DACCVIOL */
    bits |= (1u << 7); /* MMARVALID */
    ctx->scs->cfsr &= ~0x00010000u; /* clear UNDEFINSTR if set */
    ctx->scs->cfsr |= bits;
    ctx->scs->mmfar = addr;
    if (sec == MM_NONSECURE) {
        ctx->scs->shcsr_ns |= 0x1u; /* MEMFAULTACT */
    } else {
        ctx->scs->shcsr_s |= 0x1u;
    }
    if (prot_trace_level() >= 1) {
        printf("[PROT_DENY] sec=%c type=%d addr=0x%08lx attr=%d sau_ctrl=0x%08lx -> MemFault cfsr=0x%08lx mmfar=0x%08lx\n",
               (sec == MM_NONSECURE) ? 'N' : 'S',
               (int)type,
               (unsigned long)addr,
               (int)attr,
               (unsigned long)ctx->scs->sau_ctrl,
               (unsigned long)ctx->scs->cfsr,
               (unsigned long)ctx->scs->mmfar);
    }
}

void mm_prot_init(struct mm_prot_ctx *ctx, struct mm_scs *scs, const struct mm_target_cfg *cfg)
{
    if (ctx == 0) {
        return;
    }
    ctx->count = 0;
    ctx->scs = scs;
    ctx->cfg = cfg;
}

mm_bool mm_prot_add_region(struct mm_prot_ctx *ctx, mm_u32 base, mm_u32 size, mm_u8 perms, enum mm_sec_state sec)
{
    if (ctx == 0) {
        return MM_FALSE;
    }
    if (ctx->count >= (sizeof(ctx->regions) / sizeof(ctx->regions[0]))) {
        return MM_FALSE;
    }
    if (size == 0u) {
        return MM_FALSE;
    }
    ctx->regions[ctx->count].base = base;
    ctx->regions[ctx->count].size = size;
    ctx->regions[ctx->count].perms = perms;
    ctx->regions[ctx->count].sec = sec;
    ctx->count++;
    return MM_TRUE;
}

mm_bool mm_prot_interceptor(void *opaque, enum mm_access_type type, enum mm_sec_state sec, mm_u32 addr, mm_u32 size_bytes)
{
    struct mm_prot_ctx *ctx = (struct mm_prot_ctx *)opaque;
    enum mm_sau_attr attr;
    enum mm_sec_state addr_sec;
    mm_bool ignore_addr_sec;
    mm_u32 needed = 0;
    size_t i;

    if (ctx == 0) {
        return MM_TRUE;
    }

    /* Always allow System Control Space (SCS/SCB/NVIC/SysTick); tolerate alias forms.
     * This is outside the SAU/MPU-controlled memory map in this emulator model.
     */
    if ((addr >= 0xE000E000u && addr < 0xE0010000u) ||
        (addr >= 0xE002E000u && addr < 0xE0030000u) ||
        (addr >= 0x00E00000u && addr < 0x00E10000u)) {
        if (ctx->scs != 0) {
            ctx->scs->last_access_sec = sec;
        }
        if (prot_trace_level() >= 2) {
            printf("[PROT_ALLOW] sec=%s type=%s addr=0x%08lx size=%lu reason=SCS\n",
                   sec_name(sec),
                   type_name(type),
                   (unsigned long)addr,
                   (unsigned long)size_bytes);
        }
        return MM_TRUE;
    }

    /* Secure state can perform data accesses to both Secure and Non-secure
     * attributed memory. We still enforce attribution for instruction fetches.
     */
    ignore_addr_sec = (sec == MM_SECURE && type != MM_ACCESS_EXEC) ? MM_TRUE : MM_FALSE;

    addr_sec = MM_SECURE;
    attr = MM_SAU_SECURE;
    {
        mm_bool mpcbb_hit = mpcbb_attr_for_addr(ctx, addr, &attr, &addr_sec);
        if (!mpcbb_hit && ctx->scs != 0) {
            attr = mm_sau_attr_for_addr(ctx->scs, addr);
            addr_sec = (attr == MM_SAU_NONSECURE) ? MM_NONSECURE : MM_SECURE;
        }
        if (sec == MM_NONSECURE) {
            if (attr == MM_SAU_SECURE) {
                record_securefault(ctx, type, addr);
                /* Also raise a non-secure fault so NS firmware can handle it
                 * (HardFault/MemManage depending on SHCSR settings). */
                record_memfault(ctx, sec, type, addr);
                return MM_FALSE;
            }
            if (attr == MM_SAU_NSC) {
                if (type != MM_ACCESS_EXEC) {
                    record_securefault(ctx, type, addr);
                    record_memfault(ctx, sec, type, addr);
                    return MM_FALSE;
                }
            }
        }
    }

    if (type == MM_ACCESS_EXEC && ctx->scs != 0) {
        if (mm_mpu_is_xn_exec(ctx->scs, sec, addr)) {
            record_memfault(ctx, sec, type, addr);
            return MM_FALSE;
        }
    }

    switch (type) {
    case MM_ACCESS_READ: needed = MM_PROT_PERM_READ; break;
    case MM_ACCESS_WRITE: needed = MM_PROT_PERM_WRITE; break;
    case MM_ACCESS_EXEC: needed = MM_PROT_PERM_EXEC; break;
    default: needed = 0; break;
    }

    prot_trace_req(ctx, type, sec, addr, size_bytes, attr, addr_sec, ignore_addr_sec);
    prot_trace_region_scan(ctx, sec, addr, size_bytes, addr_sec, ignore_addr_sec, needed);

    for (i = 0; i < ctx->count; ++i) {
        struct mm_prot_region *r = &ctx->regions[i];
        if (!range_contains(r, addr, size_bytes)) {
            continue;
        }
        if (!ignore_addr_sec && r->sec != addr_sec) {
            continue;
        }
        if ((r->perms & needed) != 0u) {
            if (prot_trace_level() >= 2) {
                printf("[PROT_ALLOW] sec=%s type=%s addr=0x%08lx size=%lu reason=region#%lu perms=0x%02x\n",
                       sec_name(sec),
                       type_name(type),
                       (unsigned long)addr,
                       (unsigned long)size_bytes,
                       (unsigned long)i,
                       (unsigned)r->perms);
            }
            return MM_TRUE;
        }
        record_memfault(ctx, sec, type, addr);
        return MM_FALSE;
    }

    /* No matching region: fault. */
    record_memfault(ctx, sec, type, addr);
    return MM_FALSE;
}
