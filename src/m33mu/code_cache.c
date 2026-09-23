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
#include "m33mu/code_cache.h"
#include "m33mu/scs.h"
#include "m33mu/execute.h"
#include "m33mu/exec_helpers.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static mm_u32 code_page_id(mm_u32 addr, mm_u32 base)
{
    return (addr - base) >> M33MU_CODE_PAGE_SHIFT;
}

static mm_u32 code_page_base(mm_u32 addr)
{
    return addr & ~(M33MU_CODE_PAGE_SIZE - 1u);
}

static mm_bool is_control_flow(enum mm_op_kind kind)
{
    switch (kind) {
    case MM_OP_B_COND:
    case MM_OP_B_COND_WIDE:
    case MM_OP_B_UNCOND:
    case MM_OP_B_UNCOND_WIDE:
    case MM_OP_BL:
    case MM_OP_BLX:
    case MM_OP_BX:
    case MM_OP_CBZ:
    case MM_OP_CBNZ:
    case MM_OP_BKPT:
    case MM_OP_SVC:
    case MM_OP_WFI:
    case MM_OP_WFE:
    case MM_OP_SEV:
    case MM_OP_SEV_W:
    case MM_OP_UDF:
    case MM_OP_TBB:
    case MM_OP_TBH:
    case MM_OP_MRS:
    case MM_OP_MSR:
    case MM_OP_CPS:     /* PRIMASK/FAULTMASK change: let pending IRQs in */
    case MM_OP_MCR_MRC:
    case MM_OP_MCRR_MRRC:
    case MM_OP_DSB:
    case MM_OP_DMB:
    case MM_OP_ISB:
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

static mm_bool ldm_pop_writes_pc(enum mm_op_kind kind, mm_u32 imm)
{
    if (kind == MM_OP_POP) {
        return (imm & 0x0100u) != 0u;
    }
    if (kind == MM_OP_LDM) {
        return (imm & 0x8000u) != 0u;
    }
    return MM_FALSE;
}

static mm_u32 tb_index(mm_u32 pc)
{
    return (pc >> 1u) & (M33MU_TB_ENTRIES - 1u);
}

static mm_u32 page_bit_words(mm_u32 page_count)
{
    return (page_count + 63u) >> 6;
}

static mm_bool page_has_tb_test(const struct mm_code_cache *cc, mm_u32 page_id)
{
    mm_u32 idx;
    mm_u32 bit;
    if (cc == 0 || cc->page_has_tb == 0) {
        return MM_FALSE;
    }
    idx = page_id >> 6;
    bit = page_id & 63u;
    return (cc->page_has_tb[idx] & (1ull << bit)) != 0ull;
}

static void page_has_tb_set(struct mm_code_cache *cc, mm_u32 page_id)
{
    mm_u32 idx;
    mm_u32 bit;
    if (cc == 0 || cc->page_has_tb == 0) {
        return;
    }
    idx = page_id >> 6;
    bit = page_id & 63u;
    cc->page_has_tb[idx] |= (1ull << bit);
}

static mm_bool tb_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = (getenv("M33MU_TB_TRACE") != 0) ? 1 : 0;
    }
    return cached != 0;
}

void mm_code_cache_init(struct mm_code_cache *cc, struct mm_memmap *map)
{
    mm_u32 min_base = 0xffffffffu;
    mm_u32 max_end = 0u;

    if (cc == 0 || map == 0) {
        return;
    }
    memset(cc, 0, sizeof(*cc));
    cc->fpu_ok = MM_TRUE;

    if (map->flash.buffer != 0) {
        mm_u32 base = map->flash_base_s;
        mm_u32 size = map->flash_size_s;
        if (size == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size = (mm_u32)map->flash.length;
        }
        if (size != 0u) {
            if (base < min_base) min_base = base;
            if (base + size > max_end) max_end = base + size;
        }
        base = map->flash_base_ns;
        size = map->flash_size_ns;
        if (size == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size = (mm_u32)map->flash.length;
        }
        if (size != 0u) {
            if (base < min_base) min_base = base;
            if (base + size > max_end) max_end = base + size;
        }
    }
    if (map->ram.buffer != 0) {
        if (map->ram_region_count > 0u) {
            mm_u32 i;
            for (i = 0; i < map->ram_region_count; ++i) {
                const struct mm_ram_region *r = &map->ram_regions[i];
                mm_u32 size = r->size;
                if (size != 0u) {
                    if (r->base_s < min_base) min_base = r->base_s;
                    if (r->base_s + size > max_end) max_end = r->base_s + size;
                    if (r->base_ns < min_base) min_base = r->base_ns;
                    if (r->base_ns + size > max_end) max_end = r->base_ns + size;
                }
            }
        } else {
            mm_u32 base = map->ram_base_s;
            mm_u32 size = map->ram_size_s;
            if (size != 0u) {
                if (base < min_base) min_base = base;
                if (base + size > max_end) max_end = base + size;
            }
            base = map->ram_base_ns;
            size = map->ram_size_ns;
            if (size != 0u) {
                if (base < min_base) min_base = base;
                if (base + size > max_end) max_end = base + size;
            }
        }
    }

    if (min_base == 0xffffffffu || max_end <= min_base) {
        return;
    }

    cc->page_base = min_base & ~(M33MU_CODE_PAGE_SIZE - 1u);
    cc->page_size = M33MU_CODE_PAGE_SIZE;
    cc->page_count = (max_end - cc->page_base + (M33MU_CODE_PAGE_SIZE - 1u)) >> M33MU_CODE_PAGE_SHIFT;
    cc->page_gen = (mm_u32 *)calloc(cc->page_count, sizeof(mm_u32));
    cc->page_has_tb = (mm_u64 *)calloc(page_bit_words(cc->page_count), sizeof(mm_u64));
}

void mm_code_cache_reset(struct mm_code_cache *cc)
{
    mm_u32 i;
    if (cc == 0) {
        return;
    }
    memset(cc->icache, 0, sizeof(cc->icache));
    cc->fpu_ok = MM_TRUE;
    for (i = 0; i < M33MU_TB_ENTRIES; ++i) {
        if (cc->tb_cache[i] != 0) {
            free(cc->tb_cache[i]);
            cc->tb_cache[i] = 0;
        }
        cc->tb_cache_gen[i] = 0u;
    }
    if (cc->page_gen != 0 && cc->page_count != 0u) {
        memset(cc->page_gen, 0, sizeof(mm_u32) * cc->page_count);
    }
    if (cc->page_has_tb != 0 && cc->page_count != 0u) {
        memset(cc->page_has_tb, 0, sizeof(mm_u64) * page_bit_words(cc->page_count));
    }
}

void mm_code_cache_release(struct mm_code_cache *cc)
{
    mm_u32 i;
    if (cc == 0) {
        return;
    }
    for (i = 0; i < M33MU_TB_ENTRIES; ++i) {
        if (cc->tb_cache[i] != 0) {
            free(cc->tb_cache[i]);
            cc->tb_cache[i] = 0;
        }
        cc->tb_cache_gen[i] = 0u;
    }
    cc->fpu_ok = MM_TRUE;
    if (cc->page_gen != 0) {
        free(cc->page_gen);
        cc->page_gen = 0;
    }
    if (cc->page_has_tb != 0) {
        free(cc->page_has_tb);
        cc->page_has_tb = 0;
    }
    cc->page_count = 0u;
}

void mm_code_cache_note_write(struct mm_code_cache *cc, mm_u32 addr, mm_u32 size)
{
    mm_u32 start;
    mm_u32 end;
    if (cc == 0 || cc->page_gen == 0 || cc->page_count == 0u || size == 0u) {
        return;
    }
    if (addr < cc->page_base) {
        return;
    }
    start = (addr - cc->page_base) >> M33MU_CODE_PAGE_SHIFT;
    end = ((addr + size - 1u) - cc->page_base) >> M33MU_CODE_PAGE_SHIFT;
    if (start >= cc->page_count) {
        return;
    }
    if (end >= cc->page_count) {
        end = cc->page_count - 1u;
    }
    for (mm_u32 i = start; i <= end; ++i) {
        if (cc->page_has_tb != 0 && !page_has_tb_test(cc, i)) {
            continue;
        }
        cc->page_gen[i]++;
    }
}

mm_bool mm_icache_lookup(struct mm_code_cache *cc,
                         const struct mm_fetch_result *fetch,
                         enum mm_sec_state sec,
                         mm_bool fpu_ok,
                         struct mm_decoded *out)
{
    mm_u32 idx;
    mm_u32 page_id;
    mm_u32 page_id2;
    struct mm_icache_entry *e;
    if (cc == 0 || fetch == 0 || out == 0) {
        return MM_FALSE;
    }
    idx = (fetch->pc_fetch >> 1u) & (M33MU_ICACHE_ENTRIES - 1u);
    e = &cc->icache[idx];
    if (e->pc != fetch->pc_fetch || e->sec != sec || e->raw != fetch->insn || e->len != fetch->len || e->fpu_ok != fpu_ok) {
        return MM_FALSE;
    }
    if (cc->page_gen != 0) {
        if (fetch->pc_fetch < cc->page_base) {
            return MM_FALSE;
        }
        page_id = code_page_id(fetch->pc_fetch, cc->page_base);
        if (page_id >= cc->page_count) {
            return MM_FALSE;
        }
        if (e->page_gen != cc->page_gen[page_id]) {
            return MM_FALSE;
        }
        if (fetch->len == 4u) {
            page_id2 = code_page_id(fetch->pc_fetch + 2u, cc->page_base);
            if (page_id2 != page_id) {
                if (page_id2 >= cc->page_count) {
                    return MM_FALSE;
                }
                if (e->page_gen2 != cc->page_gen[page_id2]) {
                    return MM_FALSE;
                }
            }
        }
    }
    *out = e->dec;
    return MM_TRUE;
}

void mm_icache_store(struct mm_code_cache *cc,
                     const struct mm_fetch_result *fetch,
                     enum mm_sec_state sec,
                     mm_bool fpu_ok,
                     const struct mm_decoded *dec)
{
    mm_u32 idx;
    mm_u32 page_id;
    mm_u32 page_id2;
    struct mm_icache_entry *e;
    if (cc == 0 || fetch == 0 || dec == 0) {
        return;
    }
    idx = (fetch->pc_fetch >> 1u) & (M33MU_ICACHE_ENTRIES - 1u);
    e = &cc->icache[idx];
    e->pc = fetch->pc_fetch;
    e->sec = sec;
    e->raw = fetch->insn;
    e->len = fetch->len;
    e->fpu_ok = fpu_ok;
    e->page_gen = 0u;
    e->page_gen2 = 0u;
    if (cc->page_gen != 0 && fetch->pc_fetch >= cc->page_base) {
        page_id = code_page_id(fetch->pc_fetch, cc->page_base);
        if (page_id < cc->page_count) {
            e->page_gen = cc->page_gen[page_id];
            page_has_tb_set(cc, page_id);
        }
        if (fetch->len == 4u) {
            page_id2 = code_page_id(fetch->pc_fetch + 2u, cc->page_base);
            if (page_id2 < cc->page_count) {
                e->page_gen2 = cc->page_gen[page_id2];
                if (page_id2 != page_id) {
                    page_has_tb_set(cc, page_id2);
                }
            }
        }
    }
    e->dec = *dec;
}

struct mm_tb *mm_tb_lookup(struct mm_code_cache *cc, mm_u32 pc, enum mm_sec_state sec)
{
    mm_u32 idx;
    struct mm_tb *tb;
    if (cc == 0) {
        return 0;
    }
    idx = tb_index(pc);
    tb = cc->tb_cache[idx];
    if (tb == 0) {
        return 0;
    }
    if (tb->start_pc != pc || tb->sec != sec) {
        return 0;
    }
    if (cc->page_gen != 0) {
        if (tb->page_id >= cc->page_count) {
            return 0;
        }
        if (cc->page_gen[tb->page_id] != tb->page_gen) {
            return 0;
        }
    }
    return tb;
}

static mm_u32 direct_branch_target(const struct mm_fetch_result *f, const struct mm_decoded *d)
{
    mm_u32 pc = f->pc_fetch;
    switch (d->kind) {
    case MM_OP_B_UNCOND:
    case MM_OP_B_UNCOND_WIDE:
    case MM_OP_B_COND:
    case MM_OP_B_COND_WIDE:
    case MM_OP_BL:
        return (pc + 4u + d->imm) | 1u;
    default:
        return 0u;
    }
}

struct mm_tb *mm_tb_build(struct mm_code_cache *cc,
                          struct mm_cpu *cpu,
                          struct mm_memmap *map,
                          struct mm_scs *scs,
                          mm_u32 pc,
                          enum mm_sec_state sec,
                          mm_bool opt_gdb)
{
    struct mm_tb *tb = 0;
    struct mm_tb_op ops[64];
    mm_u32 op_count = 0;
    mm_u32 cycles = 0;
    mm_u32 page_base = code_page_base(pc);
    mm_u32 page_id = 0u;
    mm_u32 page_gen = 0u;
    mm_u32 idx;

    if (cc == 0 || cpu == 0 || map == 0 || scs == 0) {
        return 0;
    }
    if (cc->page_gen == 0 || pc < cc->page_base) {
        return 0;
    }
    page_id = code_page_id(pc, cc->page_base);
    if (page_id >= cc->page_count) {
        return 0;
    }
    page_gen = cc->page_gen[page_id];

    while (op_count < (sizeof(ops) / sizeof(ops[0]))) {
        struct mm_fetch_result f = mm_fetch_t32_memmap_at(map, sec, pc);
        struct mm_decoded d;

        if (f.fault) {
            break;
        }
        if (f.len == 4u && code_page_base(f.pc_fetch + 2u) != page_base) {
            break;
        }
        (void)cpu;
        (void)scs;
        (void)opt_gdb;
        d = mm_decode_t32(&f);
        if (!cc->fpu_ok && f.len == 4u && mm_is_vfp_insn_fast(f.insn)) {
            d.undefined = MM_TRUE;
        }

        ops[op_count].f = f;
        ops[op_count].d = d;
        op_count++;
        cycles++;

        if (d.undefined || d.kind == MM_OP_IT) {
            break;
        }
        if (d.kind == MM_OP_POP || d.kind == MM_OP_LDM) {
            if (ldm_pop_writes_pc(d.kind, d.imm)) {
                break;
            }
        } else if (is_control_flow(d.kind)) {
            break;
        }

        pc = f.pc_fetch + f.len;
        if (code_page_base(pc) != page_base) {
            break;
        }
    }

    if (op_count == 0) {
        return 0;
    }

    tb = (struct mm_tb *)calloc(1, sizeof(*tb) + sizeof(struct mm_tb_op) * (op_count - 1u));
    if (tb == 0) {
        return 0;
    }
    tb->start_pc = ops[0].f.pc_fetch;
    tb->end_pc = ops[op_count - 1u].f.pc_fetch + ops[op_count - 1u].f.len;
    tb->sec = sec;
    tb->page_id = page_id;
    tb->page_gen = page_gen;
    tb->op_count = (mm_u16)op_count;
    tb->cycles_est = (mm_u16)cycles;
    tb->fallthrough_pc = tb->end_pc | 1u;
    tb->branch_pc = direct_branch_target(&ops[op_count - 1u].f, &ops[op_count - 1u].d);
    tb->fpu_ok = cc->fpu_ok;
    tb->fallthrough_idx = M33MU_TB_ENTRIES;
    tb->branch_idx = M33MU_TB_ENTRIES;
    memcpy(tb->ops, ops, sizeof(struct mm_tb_op) * op_count);

    idx = tb_index(tb->start_pc);
    if (cc->tb_cache[idx] != 0) {
        free(cc->tb_cache[idx]);
    }
    cc->tb_cache_gen[idx]++;
    cc->tb_cache[idx] = tb;
    tb->cache_gen = cc->tb_cache_gen[idx];
    page_has_tb_set(cc, page_id);
    return tb;
}

struct mm_tb *mm_tb_run(struct mm_tb *tb,
                        struct mm_execute_ctx *exec_ctx,
                        mm_bool *done_out,
                        mm_bool *bkpt_hit_out,
                        mm_u32 *bkpt_imm_out,
                        mm_u32 *ops_executed_out)
{
    mm_bool bkpt_hit = MM_FALSE;
    mm_u32 bkpt_imm = 0;
    mm_u8 *it_pattern = 0;
    mm_u8 *it_remaining = 0;
    mm_u8 *it_cond = 0;
    mm_u32 ops_executed = 0;
    if (tb == 0 || exec_ctx == 0) {
        return 0;
    }
    it_pattern = exec_ctx->it_pattern;
    it_remaining = exec_ctx->it_remaining;
    it_cond = exec_ctx->it_cond;
    for (mm_u32 i = 0; i < tb->op_count; ++i) {
        mm_u32 next_pc;
        mm_bool execute_it = MM_TRUE;
        exec_ctx->fetch = &tb->ops[i].f;
        exec_ctx->dec = &tb->ops[i].d;
        mm_memmap_set_last_pc(tb->ops[i].f.pc_fetch);
        next_pc = (tb->ops[i].f.pc_fetch + tb->ops[i].f.len) | 1u;
        exec_ctx->cpu->r[15] = next_pc;
        execute_it = mm_it_should_execute(exec_ctx->cpu, &tb->ops[i].d,
                                          it_pattern, it_remaining, it_cond);
        if (!execute_it && tb->ops[i].d.kind != MM_OP_IT) {
            mm_it_advance(exec_ctx->cpu, &tb->ops[i].d, it_pattern, it_remaining, it_cond);
            ops_executed++;
            continue;
        }
        {
            enum mm_exec_status status = mm_execute_decoded(exec_ctx);
            if (status == MM_EXEC_CONTINUE) {
                if (exec_ctx->done != 0 && *exec_ctx->done) {
                    if (tb->ops[i].d.kind == MM_OP_BKPT) {
                        bkpt_hit = MM_TRUE;
                        bkpt_imm = tb->ops[i].d.imm;
                    }
                    if (done_out != 0) {
                        *done_out = MM_TRUE;
                    }
                    if (bkpt_hit_out != 0) {
                        *bkpt_hit_out = bkpt_hit;
                    }
                    if (bkpt_imm_out != 0) {
                        *bkpt_imm_out = bkpt_imm;
                    }
                    if (ops_executed_out != 0) {
                        *ops_executed_out = ops_executed;
                    }
                    return 0;
                }
                ops_executed++;
                /* Only advance IT state when PC stepped sequentially. A taken
                 * branch (incl. EXC_RETURN that just resynced ITSTATE from the
                 * saved frame) leaves cpu->r[15] != next_pc; advancing then
                 * would consume one slot of the resumed IT block. */
                if (exec_ctx->cpu->r[15] == next_pc) {
                    mm_it_advance(exec_ctx->cpu, &tb->ops[i].d, it_pattern, it_remaining, it_cond);
                }
                if (exec_ctx->cpu->sleeping || exec_ctx->cpu->r[15] != next_pc) {
                    if (bkpt_hit_out != 0) {
                        *bkpt_hit_out = bkpt_hit;
                    }
                    if (bkpt_imm_out != 0) {
                        *bkpt_imm_out = bkpt_imm;
                    }
                    if (ops_executed_out != 0) {
                        *ops_executed_out = ops_executed;
                    }
                    return 0;
                }
                continue;
            }
        }
        if (exec_ctx->done != 0 && *exec_ctx->done) {
            if (tb->ops[i].d.kind == MM_OP_BKPT) {
                bkpt_hit = MM_TRUE;
                bkpt_imm = tb->ops[i].d.imm;
            }
            if (done_out != 0) {
                *done_out = MM_TRUE;
            }
        }
        if (exec_ctx->cpu->r[15] == next_pc) {
            mm_it_advance(exec_ctx->cpu, &tb->ops[i].d, it_pattern, it_remaining, it_cond);
        }
        ops_executed++;
        if (exec_ctx->cpu->sleeping) {
            break;
        }
        if (exec_ctx->cpu->r[15] != next_pc) {
            break;
        }
        if (done_out != 0 && *done_out) {
            if (!bkpt_hit && tb_trace_enabled()) {
                fprintf(stderr, "[TB_STOP] kind=%u pc=0x%08lx\n",
                        (unsigned)tb->ops[i].d.kind,
                        (unsigned long)tb->ops[i].f.pc_fetch);
            }
            if (bkpt_hit_out != 0) {
                *bkpt_hit_out = bkpt_hit;
            }
            if (bkpt_imm_out != 0) {
                *bkpt_imm_out = bkpt_imm;
            }
            if (ops_executed_out != 0) {
                *ops_executed_out = ops_executed;
            }
            return 0;
        }
    }
    if (bkpt_hit_out != 0) {
        *bkpt_hit_out = bkpt_hit;
    }
    if (bkpt_imm_out != 0) {
        *bkpt_imm_out = bkpt_imm;
    }
    if (ops_executed_out != 0) {
        *ops_executed_out = ops_executed;
    }
    return tb;
}

struct mm_tb *mm_tb_chain_lookup(struct mm_code_cache *cc,
                                 mm_u32 idx,
                                 mm_u32 expected_gen,
                                 mm_u32 pc,
                                 enum mm_sec_state sec)
{
    struct mm_tb *tb;
    if (cc == 0) {
        return 0;
    }
    if (idx >= M33MU_TB_ENTRIES) {
        return 0;
    }
    if (cc->tb_cache_gen[idx] != expected_gen) {
        return 0;
    }
    tb = cc->tb_cache[idx];
    if (tb == 0) {
        return 0;
    }
    if (tb->start_pc != pc || tb->sec != sec) {
        return 0;
    }
    if (cc->page_gen != 0) {
        if (tb->page_id >= cc->page_count) {
            return 0;
        }
        if (cc->page_gen[tb->page_id] != tb->page_gen) {
            return 0;
        }
    }
    return tb;
}
