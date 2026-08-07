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

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "m33mu/cpu_db.h"
#include "m33mu/target.h"
#include "m33mu/cpu.h"
#include "m33mu/memmap.h"
#include "m33mu/covdump.h"
#include "m33mu/vector.h"
#include "m33mu/scs.h"
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/capstone.h"
#include "m33mu/nvic.h"
#include "m33mu/gdbstub.h"
#include "m33mu/exec_helpers.h"
#include "m33mu/execute.h"
#include "m33mu/core_sys.h"
#include "m33mu/code_cache.h"
#include "m33mu/trace.h"
#include "mcxn947/mcxn947_romapi.h"
#include "lpc55s69/lpc55s69_romapi.h"
#include "rw612/rw612_romapi.h"
#include "rp2350/rp2350_mmio.h"
#include "stm32h533/stm32h533_mmio.h"
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h5f4/stm32h5f4_mmio.h"
#include "stm32h5_mmio.h"
#include "m33mu/mem_prot.h"
#include "m33mu/exc_return.h"
#include "m33mu/tz.h"
#include "m33mu/exception.h"
#include "m33mu/table_branch.h"
#include "m33mu/timer.h"
#include "m33mu/target_hal.h"
#include "m33mu/spiflash.h"
#include "m33mu/usbdev.h"
#include "m33mu/eth_backend.h"
#include "stm32_crypto.h"
#include "rp2350/rp2350_bootrom.h"
#include "rp2350/rp2350_usb.h"
#ifdef M33MU_HAS_LIBTPMS
#include "m33mu/tpm_tis.h"
#endif
#include "m33mu/iotsafe_uart.h"
#include "m33mu/ta100.h"
#include "m33mu/se050.h"
#ifdef M33MU_HAS_RUST_PLUGINS
#include "m33mu/atecc608.h"
#include "m33mu/stsafe.h"
#include "m33mu/tropic01.h"
#endif
#include "tui.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <signal.h>
#ifdef M33MU_HAS_LIBDW
#include <elfutils/libdwfl.h>
#endif
#ifdef M33MU_HAS_LIBELF
#include <libelf.h>
#include <gelf.h>
#endif

#define CCR_DIV_0_TRP (1u << 4)
#define CCR_STKALIGN (1u << 9)
#define UFSR_UNDEFINSTR (1u << 16)
#define UFSR_DIVBYZERO (1u << 25)
#define UFSR_STKOF (1u << 20)

#define BOOT_TYPE_NORMAL 0u
#define BOOT_TYPE_BOOTSEL 2u
#define BOOT_TYPE_RAM_IMAGE 3u
#define BOOT_TYPE_FLASH_UPDATE 4u
#define BOOT_TYPE_CHAINED_FLAG 0x80u

#define BOOT_PARTITION_NONE ((mm_i8)-1)

#define BOOT_DIAGNOSTIC_HAS_PARTITION_TABLE 0x10u
#define BOOT_DIAGNOSTIC_CONSIDERED 0x20u
#define BOOT_DIAGNOSTIC_CHOSEN 0x40u
#define BOOT_DIAGNOSTIC_IMAGE_LAUNCHED 0x4000u

#define PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB 0u
#define PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS 0x00001fffu
#define PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB 13u
#define PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS 0x03ffe000u
#define PICOBIN_PARTITION_PERMISSION_NSBOOT_W_BITS 0x80000000u
/* UF2 format constants (subset). */
#define UF2_MAGIC_START0 0x0A324655u
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u
#define UF2_FLAG_NOT_MAIN_FLASH          0x00000001u
#define UF2_FLAG_FILE_CONTAINER          0x00001000u
#define UF2_FLAG_FAMILY_ID_PRESENT       0x00002000u
#define UF2_FLAG_MD5_PRESENT             0x00004000u
#define UF2_FLAG_EXTENSION_FLAGS_PRESENT 0x00008000u
#define UF2_EXTENSION_RP2_IGNORE_BLOCK   0x9957e304u

#define UF2_PAYLOAD_SIZE 256u
#define UF2_BLOCK_SIZE 512u

#define UF2_FAMILY_ID_RP2040       0xe48bff56u
#define UF2_FAMILY_ID_ABSOLUTE     0xe48bff57u
#define UF2_FAMILY_ID_DATA         0xe48bff58u
#define UF2_FAMILY_ID_RP2350_ARM_S 0xe48bff59u
#define UF2_FAMILY_ID_RP2350_RISCV 0xe48bff5au
#define UF2_FAMILY_ID_RP2350_ARM_NS 0xe48bff5bu

/* BusFault Status Register (BFSR) bits within CFSR. */
#define BFSR_IBUSERR   (1u << 8)
#define BFSR_PRECISERR (1u << 9)
#define BFSR_IMPRECISERR (1u << 10)
#define BFSR_UNSTKERR  (1u << 11)
#define BFSR_STKERR    (1u << 12)
#define BFSR_BFARVALID (1u << 15)

#define MM_CPU_HZ 64000000ull
#define NS_PER_SEC 1000000000ull
#define DEFAULT_BATCH_CYCLES 64ull         /* ~1 us @ 64 MHz */
#define DEFAULT_SYNC_GRANULARITY 640ull    /* ~10 us pacing interval */
#define IDLE_SLEEP_NS 200000ull            /* 200 us host nap when fully idle */

static mm_bool g_quit_on_faults = MM_FALSE;
static mm_bool g_call_trace = MM_FALSE;
static mm_bool g_record_start_set = MM_FALSE;
static mm_bool g_record_started = MM_TRUE;
static mm_u32 g_record_start_pc = 0;
static mm_bool g_record_start_dump = MM_FALSE;
static mm_bool g_record_start_dump_ram = MM_FALSE;
static mm_bool g_record_end_dump_ram = MM_FALSE;
static mm_u32 g_record_start_window = 0;
static mm_u32 g_record_start_remaining = 0;
static mm_bool g_record_trace_live = MM_TRUE;
static FILE *g_record_trace_fp = NULL;
static mm_bool g_record_stop_set = MM_FALSE;
static mm_u32 g_record_stop_pc = 0;
static mm_u32 g_record_dump_addr = 0;
static mm_u32 g_record_dump_count = 0;

static mm_u64 host_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((mm_u64)ts.tv_sec * NS_PER_SEC) + (mm_u64)ts.tv_nsec;
}

static mm_u64 deadline_ns(mm_u64 vcycles, mm_u64 host0_ns, mm_u64 cpu_hz)
{
    __int128 prod = (__int128)vcycles * (__int128)NS_PER_SEC;
    if (cpu_hz == 0) {
        cpu_hz = MM_CPU_HZ;
    }
    prod /= (__int128)cpu_hz;
    return host0_ns + (mm_u64)prod;
}

static int load_file_at(const char *path, mm_u8 *dst, size_t max_len, mm_u32 offset, size_t *loaded);
void mm_system_request_reset(void);
static void record_bus_fault(struct mm_scs *scs, enum mm_sec_state sec, mm_u32 addr, mm_u32 bfsr_bits);
static mm_bool raise_hard_fault(struct mm_cpu *cpu, struct mm_memmap *map, struct mm_scs *scs, mm_u32 fault_pc, mm_u32 fault_xpsr);
static mm_bool fpu_access_allowed(const struct mm_cpu *cpu, const struct mm_scs *scs);
static mm_u32 shcsr_active_mask_for_exc(mm_u32 exc_num);
static void shcsr_set_exception_active(struct mm_scs *scs, enum mm_sec_state sec, mm_u32 exc_num);
static void shcsr_clear_exception_active(struct mm_scs *scs, enum mm_sec_state sec, mm_u32 exc_num);
static void scs_set_vectactive(struct mm_scs *scs, enum mm_sec_state sec, mm_u32 exc_num);
static struct mm_decoded decode_t32_fast(const struct mm_fetch_result *fetch,
                                         const struct mm_cpu *cpu,
                                         const struct mm_scs *scs);

static mm_bool parse_hex_u32(const char *s, mm_u32 *out)
{
    char *endp = 0;
    unsigned long v;
    if (s == 0 || out == 0) {
        return MM_FALSE;
    }
    v = strtoul(s, &endp, 0);
    if (endp == s) {
        return MM_FALSE;
    }
    *out = (mm_u32)v;
    return MM_TRUE;
}

static mm_bool parse_hex_u64(const char *s, mm_u64 *out)
{
    char *endp = 0;
    unsigned long long v;
    if (s == 0 || out == 0) {
        return MM_FALSE;
    }
    v = strtoull(s, &endp, 0);
    if (endp == s) {
        return MM_FALSE;
    }
    *out = (mm_u64)v;
    return MM_TRUE;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static mm_bool parse_hex_blob(const char *s, mm_u8 *out, mm_u32 out_cap, mm_u32 *out_len)
{
    const char *hex = s;
    mm_u32 len = 0u;
    mm_u32 digits = 0u;
    if (s == 0 || out == 0 || out_len == 0) {
        return MM_FALSE;
    }
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }
    while (hex[digits] != '\0') {
        if (hex_nibble(hex[digits]) < 0) {
            return MM_FALSE;
        }
        digits++;
    }
    if ((digits == 0u) || ((digits & 1u) != 0u)) {
        return MM_FALSE;
    }
    len = digits / 2u;
    if (len > out_cap) {
        return MM_FALSE;
    }
    while (*hex != '\0') {
        int hi = hex_nibble(hex[0]);
        int lo = hex_nibble(hex[1]);
        out[*out_len] = (mm_u8)((hi << 4) | lo);
        (*out_len)++;
        hex += 2;
    }
    return MM_TRUE;
}

static mm_u64 splitmix64_next(mm_u64 *state)
{
    mm_u64 z;
    *state += 0x9e3779b97f4a7c15ull;
    z = *state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void flip_buffer_bit(mm_u8 *buf, size_t bit_index)
{
    size_t byte_index = bit_index >> 3;
    unsigned bit_in_byte = (unsigned)(bit_index & 7u);
    buf[byte_index] ^= (mm_u8)(1u << bit_in_byte);
}

static void apply_puf_noise(mm_u8 *ram, size_t ram_size, mm_u64 puf_seed,
                            mm_u64 cold_boot_count, mm_u32 flips_per_codeword)
{
    static const mm_u64 noise_seed_salt = 0x5055464e4f495345ull;
    mm_u64 noise_state = puf_seed ^ noise_seed_salt ^ (cold_boot_count * 0x9e3779b97f4a7c15ull);
    size_t total_bits = ram_size * 8u;
    size_t block_count = total_bits / 127u;
    size_t block;

    if (ram == 0 || flips_per_codeword == 0u) {
        return;
    }

    for (block = 0; block < block_count; ++block) {
        mm_u8 chosen[127];
        mm_u32 flipped = 0;
        size_t base_bit = block * 127u;

        memset(chosen, 0, sizeof(chosen));
        while (flipped < flips_per_codeword) {
            mm_u32 pos = (mm_u32)(splitmix64_next(&noise_state) % 127u);
            if (chosen[pos]) {
                continue;
            }
            chosen[pos] = 1u;
            flip_buffer_bit(ram, base_bit + pos);
            flipped++;
        }
    }
}

static mm_bool t32_is_32bit_prefix_local(mm_u16 prefix)
{
    return (prefix & 0xF800u) >= 0xE800u;
}

static void dump_trace_tail(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 count)
{
    mm_u32 i;
    if (cpu == 0 || map == 0 || count == 0u) {
        return;
    }
    if (!mm_trace_enabled()) {
        fprintf(stderr, "[TRACE_TAIL] trace not enabled\n");
        return;
    }
    fprintf(stderr, "[TRACE_TAIL] dumping last %lu steps (most recent first)\n", (unsigned long)count);
    for (i = 0; i < count; ++i) {
        mm_u32 pc;
        mm_u32 hw1 = 0;
        mm_u32 hw2 = 0;
        mm_u32 insn = 0;
        mm_u32 len = 0;
        if (!mm_trace_undo_step(cpu, map)) {
            fprintf(stderr, "[TRACE_TAIL] no more history at step %lu\n", (unsigned long)i);
            break;
        }
        pc = cpu->r[15] & ~1u;
        if (mm_memmap_fetch_read16(map, cpu->sec_state, pc, &hw1)) {
            if (t32_is_32bit_prefix_local((mm_u16)hw1) &&
                mm_memmap_fetch_read16(map, cpu->sec_state, pc + 2u, &hw2)) {
                insn = (hw1 << 16) | (hw2 & 0xffffu);
                len = 4u;
            } else {
                insn = hw1 & 0xffffu;
                len = 2u;
            }
        }
        fprintf(stderr,
                "[TRACE_TAIL] #%lu pc=0x%08lx len=%lu insn=0x%08lx "
                "r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx "
                "r4=0x%08lx r5=0x%08lx r6=0x%08lx r7=0x%08lx "
                "r8=0x%08lx r9=0x%08lx r10=0x%08lx r11=0x%08lx "
                "r12=0x%08lx sp=0x%08lx lr=0x%08lx xpsr=0x%08lx\n",
                (unsigned long)i,
                (unsigned long)pc,
                (unsigned long)len,
                (unsigned long)insn,
                (unsigned long)cpu->r[0], (unsigned long)cpu->r[1],
                (unsigned long)cpu->r[2], (unsigned long)cpu->r[3],
                (unsigned long)cpu->r[4], (unsigned long)cpu->r[5],
                (unsigned long)cpu->r[6], (unsigned long)cpu->r[7],
                (unsigned long)cpu->r[8], (unsigned long)cpu->r[9],
                (unsigned long)cpu->r[10], (unsigned long)cpu->r[11],
                (unsigned long)cpu->r[12], (unsigned long)cpu->r[13],
                (unsigned long)cpu->r[14], (unsigned long)cpu->xpsr);
    }
}

static void dump_bytes(struct mm_memmap *map, enum mm_sec_state sec,
                       mm_u32 addr, mm_u32 len, const char *label)
{
    mm_u32 i;
    mm_u8 byte = 0;

    fprintf(stderr, "[BKPT_DUMP] %s addr=0x%08lx len=%lu\n",
            label, (unsigned long)addr, (unsigned long)len);
    for (i = 0; i < len; ++i) {
        if (!mm_memmap_read8(map, sec, addr + i, &byte)) {
            fprintf(stderr, "[BKPT_DUMP] read fault at 0x%08lx\n",
                    (unsigned long)(addr + i));
            break;
        }
        if ((i % 16u) == 0u) {
            fprintf(stderr, "[BKPT_DUMP] 0x%08lx: ", (unsigned long)(addr + i));
        }
        fprintf(stderr, "%02x ", (unsigned)byte);
        if ((i % 16u) == 15u || i + 1u == len) {
            fprintf(stderr, "\n");
        }
    }
}

static void dump_record_start_context(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 r0;
    mm_u32 r1;
    mm_u32 r2;
    if (cpu == 0 || map == 0) {
        return;
    }
    r0 = cpu->r[0];
    r1 = cpu->r[1];
    r2 = cpu->r[2];
    fprintf(stderr, "[RECORD_START] pc=0x%08lx sp=0x%08lx lr=0x%08lx r0=0x%08lx r1=0x%08lx r2=0x%08lx\n",
            (unsigned long)(cpu->r[15] & ~1u),
            (unsigned long)cpu->r[13],
            (unsigned long)cpu->r[14],
            (unsigned long)r0, (unsigned long)r1, (unsigned long)r2);
    fprintf(stderr,
            "[RECORD_START_REGS] r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx "
            "r4=0x%08lx r5=0x%08lx r6=0x%08lx r7=0x%08lx r8=0x%08lx r9=0x%08lx "
            "r10=0x%08lx r11=0x%08lx r12=0x%08lx sp=0x%08lx lr=0x%08lx xpsr=0x%08lx\n",
            (unsigned long)cpu->r[0], (unsigned long)cpu->r[1],
            (unsigned long)cpu->r[2], (unsigned long)cpu->r[3],
            (unsigned long)cpu->r[4], (unsigned long)cpu->r[5],
            (unsigned long)cpu->r[6], (unsigned long)cpu->r[7],
            (unsigned long)cpu->r[8], (unsigned long)cpu->r[9],
            (unsigned long)cpu->r[10], (unsigned long)cpu->r[11],
            (unsigned long)cpu->r[12], (unsigned long)cpu->r[13],
            (unsigned long)cpu->r[14], (unsigned long)cpu->xpsr);
    dump_bytes(map, cpu->sec_state, r1, 256u, "record_a (r1)");
    dump_bytes(map, cpu->sec_state, r2, 256u, "record_b (r2)");
    dump_bytes(map, cpu->sec_state, r0, 512u, "record_r (r0)");
    dump_bytes(map, cpu->sec_state, cpu->r[13], 4096u, "record_sp (sp)");
    if (g_record_start_dump_ram) {
        if (map->ram_size_ns > 0u) {
            dump_bytes(map, MM_NONSECURE, map->ram_base_ns, map->ram_size_ns, "record_ram_ns");
        }
        if (map->ram_size_s > 0u) {
            dump_bytes(map, MM_SECURE, map->ram_base_s, map->ram_size_s, "record_ram_s");
        }
    }

    g_record_stop_pc = cpu->r[14] & ~1u;
    g_record_stop_set = MM_TRUE;
    g_record_dump_addr = r0;
}

static void dump_record_window(struct mm_cpu *cpu, struct mm_memmap *map)
{
    if (g_record_start_window == 0u) {
        return;
    }
    fprintf(stderr, "[RECORD_WINDOW] pc=0x%08lx steps=%lu\n",
            (unsigned long)(cpu->r[15] & ~1u),
            (unsigned long)g_record_start_window);
    if (g_record_end_dump_ram) {
        if (map->ram_size_ns > 0u) {
            dump_bytes(map, MM_NONSECURE, map->ram_base_ns, map->ram_size_ns, "record_ram_ns_end");
        }
        if (map->ram_size_s > 0u) {
            dump_bytes(map, MM_SECURE, map->ram_base_s, map->ram_size_s, "record_ram_s_end");
        }
    }
    dump_trace_tail(cpu, map, g_record_start_window);
    g_record_start_window = 0u;
    g_record_start_remaining = 0u;
}

static void record_window_step(struct mm_cpu *cpu, struct mm_memmap *map)
{
    if (g_record_start_remaining == 0u) {
        return;
    }
    g_record_start_remaining--;
    if (g_record_start_remaining == 0u) {
        dump_record_window(cpu, map);
    }
}

static mm_bool mm_bootapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    if (mm_rp2350_bootrom_handle(cpu, map)) {
        return MM_TRUE;
    }
    if (mm_mcxn947_romapi_handle(cpu, map)) {
        return MM_TRUE;
    }
    if (mm_lpc55s69_romapi_handle(cpu, map)) {
        return MM_TRUE;
    }
    if (mm_rw612_romapi_handle(cpu, map)) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

static void finish_trace_step_if_started(mm_bool trace_started,
                                         struct mm_cpu *cpu,
                                         struct mm_memmap *map)
{
    if (!trace_started) {
        return;
    }
    mmio_bus_end_step(&map->mmio, mm_trace_get_undo_sink());
    mm_trace_end_step(cpu);
    record_window_step(cpu, map);
}

static mm_u32 cfg_total_ram(const struct mm_target_cfg *cfg)
{
    mm_u32 total = 0;
    mm_u32 i;
    if (cfg == 0) return 0;
    if (cfg->ram_regions != 0 && cfg->ram_region_count > 0u) {
        for (i = 0; i < cfg->ram_region_count; ++i) {
            total += cfg->ram_regions[i].size;
        }
        return total;
    }
    return cfg->ram_size_s;
}

static void handle_timeout_alarm(int sig)
{
    (void)sig;
    _exit(127);
}

static void dump_cpu_regs(const struct mm_cpu *cpu, const char *tag)
{
    if (cpu == 0 || tag == 0) {
        return;
    }
    printf("[%s] r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx r4=0x%08lx r5=0x%08lx r6=0x%08lx r7=0x%08lx\n",
           tag,
           (unsigned long)cpu->r[0], (unsigned long)cpu->r[1],
           (unsigned long)cpu->r[2], (unsigned long)cpu->r[3],
           (unsigned long)cpu->r[4], (unsigned long)cpu->r[5],
           (unsigned long)cpu->r[6], (unsigned long)cpu->r[7]);
    printf("[%s] r8=0x%08lx r9=0x%08lx r10=0x%08lx r11=0x%08lx r12=0x%08lx r13=0x%08lx r14=0x%08lx r15=0x%08lx\n",
           tag,
           (unsigned long)cpu->r[8], (unsigned long)cpu->r[9],
           (unsigned long)cpu->r[10], (unsigned long)cpu->r[11],
           (unsigned long)cpu->r[12], (unsigned long)cpu->r[13],
           (unsigned long)cpu->r[14], (unsigned long)cpu->r[15]);
    printf("[%s] xpsr=0x%08lx ipsr=%lu mode=%d sec=%d ctrl_s=0x%08lx ctrl_ns=0x%08lx msp_s=0x%08lx msp_ns=0x%08lx psp_s=0x%08lx psp_ns=0x%08lx active_sp=0x%08lx\n",
           tag,
           (unsigned long)cpu->xpsr,
           (unsigned long)(cpu->xpsr & 0x1ffu),
           (int)cpu->mode,
           (int)cpu->sec_state,
           (unsigned long)cpu->control_s,
           (unsigned long)cpu->control_ns,
           (unsigned long)cpu->msp_s,
           (unsigned long)cpu->msp_ns,
           (unsigned long)cpu->psp_s,
           (unsigned long)cpu->psp_ns,
           (unsigned long)mm_cpu_get_active_sp((struct mm_cpu *)cpu));
}

static void dump_exc_stack_state(const struct mm_cpu *cpu, const char *tag)
{
    mm_u32 i;
    mm_u32 max;
    if (cpu == 0 || tag == 0) {
        return;
    }
    max = cpu->exc_depth;
    if (max > MM_EXC_STACK_MAX) {
        max = MM_EXC_STACK_MAX;
    }
    printf("[%s] exc_depth=%u\n", tag, (unsigned)cpu->exc_depth);
    for (i = 0; i < max; ++i) {
        printf("[%s] exc[%u] sp=0x%08lx use_psp=%u sec=%u\n",
               tag,
               (unsigned)i,
               (unsigned long)cpu->exc_sp[i],
               (unsigned)cpu->exc_use_psp[i],
               (unsigned)cpu->exc_sec[i]);
    }
}

static mm_bool parse_addr_size(const char *s, mm_u32 *addr_out, mm_u32 *size_out)
{
    const char *sep;
    mm_u32 addr = 0;
    mm_u32 size = 4;
    if (s == 0 || addr_out == 0 || size_out == 0) {
        return MM_FALSE;
    }
    sep = strchr(s, ':');
    if (sep == 0) {
        if (!parse_hex_u32(s, &addr)) {
            return MM_FALSE;
        }
    } else {
        if (!parse_hex_u32(s, &addr)) {
            return MM_FALSE;
        }
        if (!parse_hex_u32(sep + 1, &size)) {
            return MM_FALSE;
        }
        if (size == 0u) {
            size = 4u;
        }
    }
    *addr_out = addr;
    *size_out = size;
    return MM_TRUE;
}

struct mm_image_spec {
    char *path;
    mm_u32 offset;
    size_t loaded;
    mm_u32 load_start;
    mm_u32 load_end;
    mm_u8 type;
};

enum mm_image_type {
    MM_IMAGE_BIN = 0,
    MM_IMAGE_ELF,
    MM_IMAGE_IHEX,
    MM_IMAGE_UF2,
    MM_IMAGE_UNKNOWN
};

enum mm_boot_mode {
    MM_BOOT_FLASH = 0,
    MM_BOOT_RAM,
    MM_BOOT_SPIFLASH
};

enum mm_load_target {
    MM_LOAD_FLASH = 0,
    MM_LOAD_RAM,
    MM_LOAD_SPIFLASH
};

struct mm_load_targets {
    mm_u8 *flash;
    size_t flash_size;
    mm_u8 *ram;
    size_t ram_size;
    mm_u8 *spiflash;
    size_t spiflash_size;
    mm_u32 spiflash_base;
};

static int load_image_autodetect(struct mm_image_spec *img,
                                 const struct mm_target_cfg *cfg,
                                 const struct mm_load_targets *targets,
                                 enum mm_boot_mode boot_mode,
                                 const char *cpu_name);

static mm_u32 read_u32_le(const mm_u8 *buf)
{
    return (mm_u32)buf[0]
        | ((mm_u32)buf[1] << 8)
        | ((mm_u32)buf[2] << 16)
        | ((mm_u32)buf[3] << 24);
}

static mm_bool rp2350_vector_valid(const struct mm_target_cfg *cfg,
                                   const mm_u8 *flash,
                                   size_t flash_size,
                                   mm_u32 offset)
{
    mm_u32 sp;
    mm_u32 pc;
    mm_u32 ram_start;
    mm_u32 ram_end;
    mm_u32 flash_start;
    mm_u32 flash_end;

    if (cfg == 0 || flash == 0) return MM_FALSE;
    if (offset + 8u > flash_size) return MM_FALSE;

    sp = read_u32_le(flash + offset);
    pc = read_u32_le(flash + offset + 4u);

    ram_start = cfg->ram_base_s;
    ram_end = cfg->ram_base_s + cfg->ram_size_s;
    if (sp < ram_start || sp > ram_end) {
        ram_start = cfg->ram_base_ns;
        ram_end = cfg->ram_base_ns + cfg->ram_size_ns;
        if (sp < ram_start || sp > ram_end) {
            return MM_FALSE;
        }
    }

    flash_start = cfg->flash_base_s;
    flash_end = cfg->flash_base_s + cfg->flash_size_s;
    if (pc < flash_start || pc >= flash_end) {
        flash_start = cfg->flash_base_ns;
        flash_end = cfg->flash_base_ns + cfg->flash_size_ns;
        if (pc < flash_start || pc >= flash_end) {
            return MM_FALSE;
        }
    }
    if ((pc & 1u) == 0u) return MM_FALSE;

    return MM_TRUE;
}

static mm_u32 default_rp2350_boot_offset(const char *cpu_name,
                                         const struct mm_target_cfg *cfg,
                                         const struct mm_image_spec *images,
                                         int image_count,
                                         const mm_u8 *flash,
                                         size_t flash_size)
{
    int i;
    if (cpu_name == 0 || strcmp(cpu_name, "rp2350") != 0) {
        return 0u;
    }
    if (images == 0 || image_count <= 0) {
        return 0u;
    }
    for (i = 0; i < image_count; ++i) {
        if (images[i].offset != 0u) {
            return 0u;
        }
    }
    if (rp2350_vector_valid(cfg, flash, flash_size, 0u)) {
        return 0u;
    }
    if (rp2350_vector_valid(cfg, flash, flash_size, 0x100u)) {
        return 0x100u;
    }
    return 0u;
}

static mm_bool reload_images(struct mm_image_spec *images,
                             int image_count,
                             struct mm_load_targets *targets,
                             const struct mm_target_cfg *cfg,
                             enum mm_boot_mode boot_mode,
                             const char *cpu_name,
                             size_t *loaded_total,
                             size_t *loaded_max_end)
{
    int i;
    size_t idx;
    size_t total = 0;
    size_t max_end = 0;
    if (images == 0 || targets == 0 || loaded_total == 0 || loaded_max_end == 0) {
        return MM_FALSE;
    }
    if (targets->flash != 0) {
        for (idx = 0; idx < targets->flash_size; ++idx) {
            targets->flash[idx] = 0xFFu;
        }
    }
    if (boot_mode == MM_BOOT_RAM && targets->ram != 0) {
        for (idx = 0; idx < targets->ram_size; ++idx) {
            targets->ram[idx] = (mm_u8)(rand() & 0xFF);
        }
    }
    for (i = 0; i < image_count; ++i) {
        if (load_image_autodetect(&images[i], cfg, targets, boot_mode, cpu_name) != 0) {
            fprintf(stderr, "failed to reload image %s\n", images[i].path);
            return MM_FALSE;
        }
        total += images[i].loaded;
        if ((size_t)images[i].load_end > max_end) {
            max_end = (size_t)images[i].load_end;
        }
    }
    *loaded_total = total;
    *loaded_max_end = max_end;
    return MM_TRUE;
}

static mm_bool target_should_run(mm_bool opt_gdb,
                                 const struct mm_gdb_stub *gdb,
                                 mm_bool tui_paused,
                                 mm_bool tui_step)
{
    if (opt_gdb) {
        return mm_gdb_stub_should_run(gdb);
    }
    return (!tui_paused || tui_step);
}

static void apply_reset_view(struct mm_tui *tui,
                             struct mm_cpu *cpu,
                             struct mm_memmap *map,
                             mm_u64 cycle_total,
                             mm_u64 *steps_offset,
                             mm_u64 *steps_latched)
{
    mm_u32 sp = 0;
    mm_u32 pc = 0;
    mm_u32 vtor = 0;

    mm_system_request_reset();
    printf("[EMULATION] Reset\n");
    if (tui != 0) {
        mm_tui_close_devices(tui);
    }
    if (cpu == 0 || map == 0 || steps_offset == 0) {
        return;
    }
    vtor = cpu->vtor_s;
    (void)mm_memmap_read(map, MM_SECURE, vtor, 4u, &sp);
    (void)mm_vector_read(map, MM_SECURE, vtor, MM_VECT_RESET, &pc);
    *steps_offset = cycle_total;
    if (steps_latched != 0) {
        *steps_latched = 0;
    }
    if (tui != 0) {
        mm_tui_set_core_state(tui,
                              pc | 1u,
                              sp,
                              (mm_u8)MM_SECURE,
                              (mm_u8)MM_THREAD,
                              0u);
    }
}

static void set_tui_image0(struct mm_tui *tui, struct mm_image_spec *images, int image_count)
{
    int i;
    if (tui == 0 || images == 0) return;
    for (i = 0; i < image_count; ++i) {
        if (images[i].offset == 0 && images[i].path != 0) {
            mm_tui_set_image0(tui, images[i].path);
            return;
        }
    }
}

static void apply_target_boot_seed_regs(struct mm_cpu *cpu, const char *cpu_name)
{
    int reg;

    if (cpu == 0 || cpu_name == 0) {
        return;
    }

    /* Architecturally these are undefined at boot. Match the observed
     * STM32H563 secure reset state so early callee-saved register spills
     * line up with hardware. */
    if (strcmp(cpu_name, "stm32h563") == 0) {
        for (reg = 8; reg <= 12; ++reg) {
            cpu->r[reg] = 0xffffffffu;
        }
    }
}

static void launch_gdb_tui(const struct mm_tui *tui)
{
    char elf_path[512];
    char cmd[1024];
    const char *elf = 0;
    int port;
    size_t len;
    if (tui == 0) return;
    port = tui->gdb_port != 0 ? tui->gdb_port : 1234;
    if (tui->image0_path[0] != '\0') {
        len = strlen(tui->image0_path);
        if (len > 4 && strcmp(tui->image0_path + len - 4, ".bin") == 0) {
            snprintf(elf_path, sizeof(elf_path), "%.*s.elf", (int)(len - 4), tui->image0_path);
            if (access(elf_path, R_OK) == 0) {
                elf = elf_path;
            }
        }
    }
    if (elf != 0) {
        snprintf(cmd, sizeof(cmd),
                 "exec arm-none-eabi-gdb -n -q -ex \"file %s\" -ex \"tar rem:%d\" -ex \"tui enable\" -ex \"focus cmd\"",
                 elf, port);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "exec arm-none-eabi-gdb -n -q -ex \"tar rem:%d\" -ex \"tui enable\" -ex \"focus cmd\"",
                 port);
    }
    printf("GDB launch command: %s\n", cmd);
    {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return;
        }
        if (pid == 0) {
            execl("/usr/bin/x-terminal-emulator", "/usr/bin/x-terminal-emulator",
                  "-e", "/bin/sh", "-c", cmd, (char *)0);
            perror("execl(/usr/bin/x-terminal-emulator)");
            _exit(127);
        }
    }
    return;
}

#ifdef M33MU_HAS_LIBDW
struct mm_symbol_ctx {
    Dwfl *dwfl;
    mm_bool ready;
};

static struct mm_symbol_ctx g_symbol_ctx;

static void symbol_ctx_reset(void)
{
    if (g_symbol_ctx.dwfl != 0) {
        dwfl_end(g_symbol_ctx.dwfl);
        g_symbol_ctx.dwfl = 0;
    }
    g_symbol_ctx.ready = MM_FALSE;
}

static mm_bool symbol_ctx_build(const char **elf_paths, size_t count)
{
    static const Dwfl_Callbacks callbacks = {
        .find_elf = dwfl_build_id_find_elf,
        .find_debuginfo = dwfl_standard_find_debuginfo,
        .section_address = dwfl_offline_section_address
    };
    Dwfl *dwfl;
    size_t i;
    mm_bool any_added = MM_FALSE;

    symbol_ctx_reset();
    if (elf_paths == 0 || count == 0u) {
        return MM_FALSE;
    }
    dwfl = dwfl_begin(&callbacks);
    if (dwfl == 0) {
        return MM_FALSE;
    }
    for (i = 0; i < count; ++i) {
        if (elf_paths[i] == 0 || elf_paths[i][0] == '\0') {
            continue;
        }
        if (dwfl_report_offline(dwfl, elf_paths[i], elf_paths[i], -1) != 0) {
            any_added = MM_TRUE;
        }
    }
    if (!any_added || dwfl_report_end(dwfl, 0, 0) != 0) {
        dwfl_end(dwfl);
        return MM_FALSE;
    }
    g_symbol_ctx.dwfl = dwfl;
    g_symbol_ctx.ready = MM_TRUE;
    return MM_TRUE;
}
#else
static mm_bool symbol_ctx_build(const char **elf_paths, size_t count)
{
    (void)elf_paths;
    (void)count;
    return MM_FALSE;
}
#endif

static mm_bool symbol_lookup_name(mm_u32 pc, char *out, size_t out_len)
{
#ifdef M33MU_HAS_LIBDW
    Dwfl_Module *mod;
    const char *name;
    mm_u32 addr;
    size_t len;
    if (out == 0 || out_len == 0) return MM_FALSE;
    out[0] = '\0';
    if (!g_symbol_ctx.ready || g_symbol_ctx.dwfl == 0) {
        return MM_FALSE;
    }
    addr = pc & ~1u;
    mod = dwfl_addrmodule(g_symbol_ctx.dwfl, addr);
    if (mod == 0) {
        return MM_FALSE;
    }
    name = dwfl_module_addrname(mod, addr);
    if (name == 0 || name[0] == '\0' || name[0] == '?') {
        return MM_FALSE;
    }
    len = strlen(name);
    if (len >= out_len) {
        len = out_len - 1u;
    }
    memcpy(out, name, len);
    out[len] = '\0';
    return MM_TRUE;
#else
    (void)pc;
    (void)out;
    (void)out_len;
    return MM_FALSE;
#endif
}

#ifdef M33MU_HAS_LIBDW
#endif

static const char *path_basename(const char *path)
{
    const char *slash;
    if (path == 0) return "";
    slash = strrchr(path, '/');
    return (slash != 0) ? (slash + 1) : path;
}

static mm_bool symbol_stem_match(const char *a, const char *b)
{
    const char *abase = path_basename(a);
    const char *bbase = path_basename(b);
    const char *adot = strrchr(abase, '.');
    const char *bdot = strrchr(bbase, '.');
    size_t alen = (adot != 0) ? (size_t)(adot - abase) : strlen(abase);
    size_t blen = (bdot != 0) ? (size_t)(bdot - bbase) : strlen(bbase);
    if (alen == 0 || blen == 0 || alen != blen) {
        return MM_FALSE;
    }
    return (memcmp(abase, bbase, alen) == 0) ? MM_TRUE : MM_FALSE;
}

static mm_bool add_symbol_path(const char *path, const char **list, size_t *count, size_t max)
{
    size_t i;
    if (path == 0 || path[0] == '\0' || list == 0 || count == 0 || *count >= max) {
        return MM_FALSE;
    }
    for (i = 0; i < *count; ++i) {
        if (strcmp(list[i], path) == 0) {
            return MM_FALSE;
        }
    }
    list[*count] = path;
    (*count)++;
    return MM_TRUE;
}

static void build_symbol_db(const struct mm_image_spec *images,
                            int image_count,
                            const char **gdb_symbols,
                            size_t gdb_symbol_count)
{
    const char *paths[32];
    char elf_paths[32][512];
    size_t path_count = 0;
    size_t elf_count = 0;
    int i;

    if (gdb_symbols != 0 && gdb_symbol_count > 0u) {
        for (i = 0; i < (int)gdb_symbol_count; ++i) {
            const char *sym = gdb_symbols[i];
            if (sym != 0 && sym[0] != '\0' && access(sym, R_OK) == 0) {
                if (add_symbol_path(sym, paths, &path_count, 32u)) {
                    printf("Symbols: %s (from --gdb-symbols)\n", sym);
                }
            }
        }
    }
    for (i = 0; i < image_count; ++i) {
        const struct mm_image_spec *img = (images != 0) ? &images[i] : 0;
        const char *img_path = (img != 0) ? img->path : 0;
        size_t len;
        if (img_path == 0 || img_path[0] == '\0') continue;
        len = strlen(img_path);
        if (img != 0 && img->type == MM_IMAGE_ELF) {
            if (access(img_path, R_OK) == 0) {
                mm_bool skip = MM_FALSE;
                if (gdb_symbols != 0) {
                    int si;
                    for (si = 0; si < (int)gdb_symbol_count; ++si) {
                        if (symbol_stem_match(img_path, gdb_symbols[si])) {
                            skip = MM_TRUE;
                            break;
                        }
                    }
                }
                if (!skip) {
                    if (add_symbol_path(img_path, paths, &path_count, 32u)) {
                        printf("Symbols: %s (auto for %s)\n", img_path, img_path);
                    }
                }
            }
        } else if (len > 4) {
            if (elf_count < 32u) {
                snprintf(elf_paths[elf_count], sizeof(elf_paths[elf_count]),
                         "%.*s.elf", (int)(len - 4), img_path);
                if (access(elf_paths[elf_count], R_OK) == 0) {
                    mm_bool skip = MM_FALSE;
                    if (gdb_symbols != 0) {
                        int si;
                        for (si = 0; si < (int)gdb_symbol_count; ++si) {
                            if (symbol_stem_match(elf_paths[elf_count], gdb_symbols[si])) {
                                skip = MM_TRUE;
                                break;
                            }
                        }
                    }
                    if (!skip) {
                        if (add_symbol_path(elf_paths[elf_count], paths, &path_count, 32u)) {
                            printf("Symbols: %s (auto for %s)\n", elf_paths[elf_count], img_path);
                        }
                    }
                }
                elf_count++;
            }
        }
    }
    (void)symbol_ctx_build(paths, path_count);
}

static void calltrace_lookup_name(mm_u32 pc, char *out, size_t out_len)
{
    if (out == 0 || out_len == 0) return;
    out[0] = '\0';
    if (symbol_lookup_name(pc, out, out_len)) {
        return;
    }
    snprintf(out, out_len, "unknown");
}

static void calltrace_log_call(mm_u32 addr,
                               const struct mm_cpu *cpu)
{
    char name[128];
    if (!g_call_trace || cpu == 0) return;
    calltrace_lookup_name(addr, name, sizeof(name));
    printf("[CALL TRACE] 0x%08lx: %s(r0=0x%08lx, r1=0x%08lx, r2=0x%08lx, r3=0x%08lx)\n",
           (unsigned long)(addr & ~1u),
           name,
           (unsigned long)cpu->r[0],
           (unsigned long)cpu->r[1],
           (unsigned long)cpu->r[2],
           (unsigned long)cpu->r[3]);
}

static void calltrace_log_return(mm_u32 addr,
                                 const struct mm_cpu *cpu)
{
    char name[128];
    if (!g_call_trace || cpu == 0) return;
    calltrace_lookup_name(addr, name, sizeof(name));
    printf("[CALL TRACE] 0x%08lx: %s returned 0x%08lx\n",
           (unsigned long)(addr & ~1u),
           name,
           (unsigned long)cpu->r[0]);
}

static void calltrace_log_nsc(mm_u32 addr)
{
    char name[128];
    if (!g_call_trace) return;
    calltrace_lookup_name(addr, name, sizeof(name));
    printf("[CALL TRACE] 0x%08lx: NSC call %s (on SG)\n",
           (unsigned long)(addr & ~1u),
           name);
}

static void calltrace_log_ns_resume(mm_u32 addr)
{
    if (!g_call_trace) return;
    printf("[CALL TRACE] 0x%08lx: NS RESUME (from SG)\n",
           (unsigned long)(addr & ~1u));
}

static void calltrace_log_interrupt(mm_u32 addr, mm_u32 irq)
{
    if (!g_call_trace) return;
    printf("[CALL TRACE] 0x%08lx: INTERRUPT #%lu\n",
           (unsigned long)(addr & ~1u),
           (unsigned long)irq);
}

static void calltrace_log_irq_resume(mm_u32 addr)
{
    if (!g_call_trace) return;
    printf("[CALL TRACE] 0x%08lx: RESUME from IRQ\n",
           (unsigned long)(addr & ~1u));
}

static void calltrace_handle_decoded(const struct mm_cpu *cpu,
                                     const struct mm_fetch_result *fetch,
                                     const struct mm_decoded *dec)
{
    mm_u32 target;
    if (!g_call_trace || cpu == 0 || fetch == 0 || dec == 0) {
        return;
    }
    switch (dec->kind) {
    case MM_OP_BL:
        target = (fetch->pc_fetch + 4u + dec->imm) | 1u;
        calltrace_log_call(target, cpu);
        break;
    case MM_OP_BLX:
        target = cpu->r[dec->rm];
        calltrace_log_call(target, cpu);
        break;
    case MM_OP_BLXNS:
        target = cpu->r[dec->rm];
        calltrace_log_call(target, cpu);
        break;
    case MM_OP_BX:
        if (dec->rm == 14u) {
            target = cpu->r[14];
            if ((target & 0xffffff00u) == 0xffffff00u) {
                break;
            }
            if (cpu->sec_state == MM_NONSECURE &&
                cpu->tz_depth > 0 &&
                target == MM_TZ_FNC_RETURN) {
                calltrace_log_ns_resume(fetch->pc_fetch);
                break;
            }
            calltrace_log_return(fetch->pc_fetch, cpu);
        }
        break;
    case MM_OP_POP:
        if ((dec->imm & 0x0100u) != 0u) {
            calltrace_log_return(fetch->pc_fetch, cpu);
        }
        break;
    case MM_OP_SG:
        calltrace_log_nsc(fetch->pc_fetch);
        break;
    default:
        break;
    }
}

static mm_bool handle_tui(struct mm_tui *tui,
                          mm_bool opt_tui,
                          mm_bool *opt_capstone,
                          mm_bool *opt_gdb,
                          struct mm_gdb_stub *gdb,
                          const char *cpu_name,
                          struct mm_cpu *cpu,
                          struct mm_scs *scs,
                          struct mm_memmap *map,
                          mm_u64 cycle_total,
                          mm_u64 *steps_offset,
                          mm_u64 *steps_latched,
                          mm_bool *tui_paused,
                          mm_bool *tui_step,
                          mm_bool *reload_pending,
                          int gdb_port)
{
    mm_u32 actions;
    mm_bool running;
    char func_name[128];
    mm_u32 func_pc;

    if (!opt_tui || tui == 0) {
        return MM_FALSE;
    }
    running = target_should_run((opt_gdb != 0 ? *opt_gdb : MM_FALSE), gdb,
                                (tui_paused != 0 ? *tui_paused : MM_FALSE),
                                (tui_step != 0 ? *tui_step : MM_FALSE));
    if (steps_offset == 0) {
        return MM_FALSE;
    }
    if (cycle_total < *steps_offset) {
        *steps_offset = cycle_total;
    }
    mm_tui_set_target_running(tui, running);
    mm_tui_set_gdb_status(tui, (opt_gdb != 0 && *opt_gdb) ? gdb->connected : MM_FALSE,
                          (opt_gdb != 0 && *opt_gdb) ? gdb_port : 0);
#ifdef M33MU_USE_LIBCAPSTONE
    mm_tui_set_capstone(tui, capstone_available() ? MM_TRUE : MM_FALSE,
                        capstone_is_enabled() ? MM_TRUE : MM_FALSE);
#else
    mm_tui_set_capstone(tui, MM_FALSE, MM_FALSE);
#endif
    if (cpu != 0 && steps_latched != 0) {
        mm_bool fpu_enabled = MM_FALSE;
        if (scs != 0) {
            fpu_enabled = fpu_access_allowed(cpu, scs);
        }
        mm_tui_set_core_state(tui,
                              cpu->r[15],
                              mm_cpu_get_active_sp(cpu),
                              (mm_u8)cpu->sec_state,
                              (mm_u8)cpu->mode,
                              *steps_latched);
        mm_tui_set_registers(tui, cpu, fpu_enabled);
    }
    func_pc = 0u;
    func_name[0] = '\0';
    if (!running) {
        func_pc = (cpu != 0) ? cpu->r[15] : tui->core_pc;
        if (tui->func_pc != func_pc) {
            if (symbol_lookup_name(func_pc, func_name, sizeof(func_name))) {
                mm_tui_set_function(tui, func_pc, func_name);
            } else {
                mm_tui_set_function(tui, func_pc, 0);
            }
        }
    } else if (tui->func_valid) {
        mm_tui_set_function(tui, 0u, 0);
    }
    if (cpu_name != 0) {
        mm_tui_set_cpu_name(tui, cpu_name);
    }
    if (map != 0) {
        mm_tui_set_memory_map(tui, map);
    }
    actions = mm_tui_take_actions(tui);
    if ((actions & MM_TUI_ACTION_QUIT) != 0u) {
        return MM_TRUE;
    }
    if ((actions & MM_TUI_ACTION_RESET) != 0u) {
        apply_reset_view(tui, cpu, map, cycle_total, steps_offset, steps_latched);
    }
    if ((actions & MM_TUI_ACTION_RELOAD) != 0u) {
        if (reload_pending != 0) {
            *reload_pending = MM_TRUE;
        }
    }
    if ((actions & MM_TUI_ACTION_PAUSE) != 0u) {
        if (opt_gdb != 0 && *opt_gdb && gdb != 0) {
            gdb->running = MM_FALSE;
            mm_gdb_stub_notify_stop(gdb, 5);
        } else if (tui_paused != 0) {
            *tui_paused = MM_TRUE;
        }
    }
    if ((actions & MM_TUI_ACTION_CONTINUE) != 0u) {
        if (opt_gdb != 0 && *opt_gdb && gdb != 0) {
            gdb->running = MM_TRUE;
        } else if (tui_paused != 0) {
            *tui_paused = MM_FALSE;
        }
    }
    if ((actions & MM_TUI_ACTION_STEP) != 0u) {
        if (opt_gdb != 0 && *opt_gdb && gdb != 0) {
            gdb->step_pending = MM_TRUE;
            gdb->running = MM_TRUE;
        } else {
            if (tui_step != 0) *tui_step = MM_TRUE;
            if (tui_paused != 0) *tui_paused = MM_FALSE;
        }
    }
    if ((actions & MM_TUI_ACTION_TOGGLE_CAPSTONE) != 0u) {
#ifdef M33MU_USE_LIBCAPSTONE
        if (capstone_available()) {
            if (!capstone_is_enabled()) {
                if (capstone_init()) {
                    (void)capstone_set_enabled(MM_TRUE);
                    if (opt_capstone != 0) {
                        *opt_capstone = MM_TRUE;
                    }
                } else {
                    fprintf(stderr, "failed to initialize capstone\n");
                }
            } else {
                (void)capstone_set_enabled(MM_FALSE);
                if (opt_capstone != 0) {
                    *opt_capstone = MM_FALSE;
                }
            }
        }
#else
        (void)opt_capstone;
#endif
    }
    if ((actions & MM_TUI_ACTION_LAUNCH_GDB) != 0u) {
        if (gdb_port <= 0 || gdb_port > 65535) {
            fprintf(stderr, "invalid gdb port: %d\n", gdb_port);
        } else {
            if (gdb != 0 && *opt_gdb) {
                mm_gdb_stub_close(gdb);
                *opt_gdb = MM_FALSE;
            }
            mm_gdb_stub_set_cpu_name(gdb, cpu_name);
            printf("Starting GDB server on port %d...\n", gdb_port);
            if (mm_gdb_stub_start(gdb, gdb_port)) {
                *opt_gdb = MM_TRUE;
                (void)launch_gdb_tui(tui);
                printf("Waiting for GDB connection in background...\n");
            } else {
                fprintf(stderr, "Failed to start GDB server\n");
            }
        }
    }
    return MM_FALSE;
}

static mm_bool parse_u32(const char *s, mm_u32 *out);
static mm_bool parse_usb_spec(const char *spec, char *udc_out, size_t udc_len);
static struct mm_decoded decode_t32_fast(const struct mm_fetch_result *fetch,
                                         const struct mm_cpu *cpu,
                                         const struct mm_scs *scs);
static void host_sync_if_needed(mm_u64 vcycles,
                                mm_u64 *vcycles_last_sync,
                                mm_u64 host0_ns,
                                mm_u64 sync_granularity_cycles,
                                mm_u64 cpu_hz)
{
    mm_u64 delta_cycles;
    mm_u64 now_ns;
    mm_u64 target_ns;

    if (vcycles_last_sync == NULL) {
        return;
    }
    delta_cycles = vcycles - *vcycles_last_sync;
    if (delta_cycles < sync_granularity_cycles) {
        return;
    }

    target_ns = deadline_ns(vcycles, host0_ns, cpu_hz);
    now_ns = host_now_ns();
    if (now_ns < target_ns) {
        mm_u64 sleep_ns = target_ns - now_ns;
        struct timespec req;
        req.tv_sec = (time_t)(sleep_ns / NS_PER_SEC);
        req.tv_nsec = (long)(sleep_ns % NS_PER_SEC);
        nanosleep(&req, 0);
    }
    *vcycles_last_sync = vcycles;
}

static struct mm_cpu *g_cpu0 = 0;
static struct mm_cpu *g_cpu1 = 0;
static struct mm_nvic *g_nvic0 = 0;
static struct mm_nvic *g_nvic1 = 0;

static struct mm_nvic *nvic_for_cpu(const struct mm_cpu *cpu)
{
    if (cpu == g_cpu1) return g_nvic1;
    return g_nvic0;
}

static mm_bool addr_in_flash(const struct mm_memmap *map, mm_u32 addr)
{
    mm_u32 base = 0u;
    mm_u32 size = 0u;
    if (map == 0) return MM_FALSE;
    if (map->flash_size_ns != 0u) {
        base = map->flash_base_ns;
        size = map->flash_size_ns;
    } else if (map->flash.length > 0u) {
        base = map->flash.base;
        size = (mm_u32)map->flash.length;
    }
    if (size != 0u && addr >= base && addr < (base + size)) {
        return MM_TRUE;
    }
    if (map->flash_size_s != 0u) {
        base = map->flash_base_s;
        size = map->flash_size_s;
        if (addr >= base && addr < (base + size)) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool addr_in_ram_region(mm_u32 addr, mm_u32 base, mm_u32 size)
{
    if (size == 0u) return MM_FALSE;
    return (addr >= base && addr < (base + size)) ? MM_TRUE : MM_FALSE;
}

static mm_bool addr_exec_ok(const struct mm_memmap *map, mm_u32 addr)
{
    mm_u32 instr = 0u;
    if (map == 0 || addr == 0u) return MM_FALSE;
    if (!mm_memmap_fetch_read16(map, MM_NONSECURE, addr, &instr)) return MM_FALSE;
    return (instr != 0u) ? MM_TRUE : MM_FALSE;
}

static void rp2350_usb_sync_vector_ready(const struct mm_scs *scs, const struct mm_memmap *map)
{
    mm_u32 entry = 0;
    mm_u32 vec_addr;
    mm_bool valid = MM_FALSE;
    mm_u32 entry_addr = 0u;
    mm_u32 slot1 = 0u;
    mm_u32 slot1_addr;
    mm_u32 slot_handler = 0u;
    static mm_u32 last_entry = 0;
    static mm_bool last_valid = MM_FALSE;
    if (scs == 0 || map == 0) return;
    vec_addr = scs->vtor_ns + (mm_u32)(16u + 14u) * 4u;
    if (mm_memmap_read(map, MM_NONSECURE, vec_addr, 4u, &entry)) {
        entry_addr = entry & ~1u;
        valid = (entry_addr != 0u && addr_in_flash(map, entry_addr)) ? MM_TRUE : MM_FALSE;
        if (!valid && entry_addr != 0u &&
            addr_in_ram_region(scs->vtor_ns, scs->vtor_ns, 0x1000u)) {
            if (mm_memmap_read(map, MM_NONSECURE, scs->vtor_ns + 4u, 4u, &slot1)) {
                slot1_addr = slot1 & ~1u;
                if (slot1_addr != 0u && addr_in_flash(map, slot1_addr) &&
                    addr_in_ram_region(entry_addr, scs->vtor_ns, 0x1000u) &&
                    mm_memmap_read(map, MM_NONSECURE, entry_addr + 8u, 4u, &slot_handler) &&
                    slot_handler != 0u && addr_exec_ok(map, slot_handler & ~1u)) {
                    valid = MM_TRUE;
                }
            }
        }
        if (entry != last_entry || valid != last_valid) {
            last_entry = entry;
            last_valid = valid;
        }
        mm_rp2350_usb_set_irq_vector_ready(valid);
    } else {
        mm_rp2350_usb_set_irq_vector_ready(MM_FALSE);
        last_valid = MM_FALSE;
    }
}

static void update_tui_steps_latched(mm_bool opt_gdb,
                                     struct mm_gdb_stub *gdb,
                                     mm_bool tui_paused,
                                     mm_bool tui_step,
                                     mm_u64 cycle_total,
                                     mm_u64 *steps_offset,
                                     mm_u64 *steps_latched)
{
    if (steps_offset == 0 || steps_latched == 0) {
        return;
    }
    if (cycle_total < *steps_offset) {
        *steps_offset = cycle_total;
    }
    if (target_should_run(opt_gdb, gdb, tui_paused, tui_step)) {
        *steps_latched = cycle_total - *steps_offset;
    }
}

static int load_file(const char *path, mm_u8 *dst, size_t max_len, size_t *loaded)
{
    FILE *f;
    size_t n;
    f = fopen(path, "rb");
    if (f == NULL) {
        perror("open");
        return -1;
    }
    n = fread(dst, 1, max_len, f);
    fclose(f);
    *loaded = n;
    return 0;
}

static int load_file_at(const char *path, mm_u8 *dst, size_t max_len, mm_u32 offset, size_t *loaded)
{
    if (dst == 0 || loaded == 0) {
        return -1;
    }
    if ((size_t)offset > max_len) {
        fprintf(stderr, "image offset 0x%08lx out of bounds\n", (unsigned long)offset);
        return -1;
    }
    return load_file(path, dst + offset, max_len - (size_t)offset, loaded);
}

static const char *image_type_name(mm_u8 type)
{
    switch (type) {
    case MM_IMAGE_ELF: return "ELF";
    case MM_IMAGE_IHEX: return "IHEX";
    case MM_IMAGE_UF2: return "UF2";
    case MM_IMAGE_BIN: return "BIN";
    default: return "UNKNOWN";
    }
}

static mm_u8 detect_image_type(const char *path)
{
    FILE *f;
    unsigned char buf[8];
    size_t n;
    if (path == 0) return MM_IMAGE_UNKNOWN;
    {
        const char *ext = strrchr(path, '.');
        if (ext != 0) {
            if (strcmp(ext, ".elf") == 0) return MM_IMAGE_ELF;
            if (strcmp(ext, ".hex") == 0 || strcmp(ext, ".ihex") == 0) return MM_IMAGE_IHEX;
            if (strcmp(ext, ".uf2") == 0) return MM_IMAGE_UF2;
        }
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return MM_IMAGE_UNKNOWN;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n >= 4 && buf[0] == 0x7fu && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        return MM_IMAGE_ELF;
    }
    if (n >= 8) {
        mm_u32 m0 = (mm_u32)buf[0] | ((mm_u32)buf[1] << 8) | ((mm_u32)buf[2] << 16) | ((mm_u32)buf[3] << 24);
        mm_u32 m1 = (mm_u32)buf[4] | ((mm_u32)buf[5] << 8) | ((mm_u32)buf[6] << 16) | ((mm_u32)buf[7] << 24);
        if (m0 == UF2_MAGIC_START0 && m1 == UF2_MAGIC_START1) {
            return MM_IMAGE_UF2;
        }
    }
    if (n > 0 && buf[0] == ':') {
        return MM_IMAGE_IHEX;
    }
    return MM_IMAGE_BIN;
}

static mm_bool map_flash_offset(const struct mm_target_cfg *cfg, mm_u32 addr, mm_u32 *off_out)
{
    if (cfg == 0 || off_out == 0) return MM_FALSE;
    if (addr >= cfg->flash_base_s && addr < (cfg->flash_base_s + cfg->flash_size_s)) {
        *off_out = addr - cfg->flash_base_s;
        return MM_TRUE;
    }
    if (addr >= cfg->flash_base_ns && addr < (cfg->flash_base_ns + cfg->flash_size_ns)) {
        *off_out = addr - cfg->flash_base_ns;
        return MM_TRUE;
    }
    if (addr < cfg->flash_size_s) {
        *off_out = addr;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool map_ram_offset(const struct mm_target_cfg *cfg, mm_u32 addr, mm_u32 *off_out)
{
    if (cfg == 0 || off_out == 0) return MM_FALSE;
    if (addr >= cfg->ram_base_s && addr < (cfg->ram_base_s + cfg->ram_size_s)) {
        *off_out = addr - cfg->ram_base_s;
        return MM_TRUE;
    }
    if (addr >= cfg->ram_base_ns && addr < (cfg->ram_base_ns + cfg->ram_size_ns)) {
        *off_out = addr - cfg->ram_base_ns;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool map_spiflash_offset(const struct mm_load_targets *targets, mm_u32 addr, mm_u32 *off_out)
{
    if (targets == 0 || off_out == 0 || targets->spiflash == 0) return MM_FALSE;
    if (addr >= targets->spiflash_base && addr < (targets->spiflash_base + (mm_u32)targets->spiflash_size)) {
        *off_out = addr - targets->spiflash_base;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool map_target_offset(const struct mm_target_cfg *cfg,
                                 const struct mm_load_targets *targets,
                                 mm_u32 addr,
                                 enum mm_load_target *target_out,
                                 mm_u32 *off_out)
{
    mm_u32 off = 0;
    if (cfg == 0 || targets == 0 || target_out == 0 || off_out == 0) return MM_FALSE;
    if (map_spiflash_offset(targets, addr, &off)) {
        *target_out = MM_LOAD_SPIFLASH;
        *off_out = off;
        return MM_TRUE;
    }
    if (map_flash_offset(cfg, addr, &off)) {
        *target_out = MM_LOAD_FLASH;
        *off_out = off;
        return MM_TRUE;
    }
    if (map_ram_offset(cfg, addr, &off)) {
        *target_out = MM_LOAD_RAM;
        *off_out = off;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_u8 *load_target_ptr(const struct mm_load_targets *targets,
                              enum mm_load_target target,
                              mm_u32 off,
                              size_t len,
                              size_t *size_out)
{
    mm_u8 *base = 0;
    size_t size = 0;
    if (targets == 0) return 0;
    switch (target) {
    case MM_LOAD_RAM:
        base = targets->ram;
        size = targets->ram_size;
        break;
    case MM_LOAD_SPIFLASH:
        base = targets->spiflash;
        size = targets->spiflash_size;
        break;
    case MM_LOAD_FLASH:
    default:
        base = targets->flash;
        size = targets->flash_size;
        break;
    }
    if (base == 0 || (size_t)off >= size) return 0;
    if ((size_t)off + len > size) return 0;
    if (size_out) *size_out = size;
    return base + off;
}

static const char *boot_mode_name(enum mm_boot_mode mode)
{
    switch (mode) {
    case MM_BOOT_RAM: return "ram";
    case MM_BOOT_SPIFLASH: return "spiflash";
    case MM_BOOT_FLASH:
    default:
        break;
    }
    return "flash";
}

static mm_bool parse_boot_mode(const char *s, enum mm_boot_mode *mode_out)
{
    if (s == 0 || mode_out == 0) return MM_FALSE;
    if (strcmp(s, "flash") == 0) {
        *mode_out = MM_BOOT_FLASH;
        return MM_TRUE;
    }
    if (strcmp(s, "ram") == 0) {
        *mode_out = MM_BOOT_RAM;
        return MM_TRUE;
    }
    if (strcmp(s, "spiflash") == 0) {
        *mode_out = MM_BOOT_SPIFLASH;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static int load_elf_segments(const char *path,
                             const struct mm_target_cfg *cfg,
                             const struct mm_load_targets *targets,
                             mm_u32 offset,
                             mm_u32 *start_out,
                             mm_u32 *end_out,
                             size_t *loaded_out)
{
#ifdef M33MU_HAS_LIBELF
    int fd = -1;
    FILE *f = NULL;
    Elf *elf = NULL;
    GElf_Ehdr ehdr;
    size_t phnum = 0;
    size_t i;
    mm_bool any = MM_FALSE;
    mm_u32 min_addr = 0xffffffffu;
    mm_u32 max_addr = 0u;
    size_t loaded = 0;

    if (start_out) *start_out = 0u;
    if (end_out) *end_out = 0u;
    if (loaded_out) *loaded_out = 0u;
    if (cfg == 0 || targets == 0) {
        return -1;
    }
    if (elf_version(EV_CURRENT) == EV_NONE) {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    fd = fileno(f);
    elf = elf_begin(fd, ELF_C_READ, NULL);
    if (elf == NULL) {
        fclose(f);
        return -1;
    }
    if (gelf_getehdr(elf, &ehdr) == NULL) {
        elf_end(elf);
        fclose(f);
        return -1;
    }
    if (elf_getphdrnum(elf, &phnum) != 0) {
        elf_end(elf);
        fclose(f);
        return -1;
    }
    for (i = 0; i < phnum; ++i) {
        GElf_Phdr phdr;
        mm_u32 addr;
        mm_u32 off;
        size_t to_read;
        enum mm_load_target target;
        mm_u8 *dst;
        if (gelf_getphdr(elf, (int)i, &phdr) == NULL) {
            continue;
        }
        if (phdr.p_type != PT_LOAD || phdr.p_filesz == 0) {
            continue;
        }
        addr = (mm_u32)phdr.p_paddr;
        if (addr == 0u) {
            addr = (mm_u32)phdr.p_vaddr;
        }
        if (!map_target_offset(cfg, targets, addr, &target, &off)) {
            fprintf(stderr, "warning: ELF segment 0x%08lx out of range (file %s)\n",
                    (unsigned long)addr, path);
            continue;
        }
        if (target == MM_LOAD_FLASH && addr < cfg->flash_size_s && offset != 0u) {
            off += offset;
        }
        to_read = (size_t)phdr.p_filesz;
        dst = load_target_ptr(targets, target, off, to_read, 0);
        if (dst == 0) {
            fprintf(stderr, "warning: ELF segment 0x%08lx out of bounds (file %s)\n",
                    (unsigned long)addr, path);
            continue;
        }
        if (fseek(f, (long)phdr.p_offset, SEEK_SET) != 0) {
            continue;
        }
        if (fread(dst, 1, to_read, f) != to_read) {
            fprintf(stderr, "warning: short read for ELF segment (file %s)\n", path);
        }
        if (addr < min_addr) min_addr = addr;
        if (addr + (mm_u32)to_read > max_addr) max_addr = addr + (mm_u32)to_read;
        loaded += to_read;
        any = MM_TRUE;
    }
    elf_end(elf);
    fclose(f);
    if (!any) {
        return -1;
    }
    if (start_out) *start_out = min_addr;
    if (end_out) *end_out = max_addr;
    if (loaded_out) *loaded_out = loaded;
    return 0;
#else
    (void)path;
    (void)cfg;
    (void)targets;
    (void)offset;
    if (start_out) *start_out = 0u;
    if (end_out) *end_out = 0u;
    if (loaded_out) *loaded_out = 0u;
    return -1;
#endif
}

static mm_bool parse_ihex_byte(const char *s, mm_u8 *out)
{
    int hi, lo;
    if (s == 0 || out == 0) return MM_FALSE;
    hi = (s[0] >= '0' && s[0] <= '9') ? (s[0] - '0')
        : (s[0] >= 'a' && s[0] <= 'f') ? (s[0] - 'a' + 10)
        : (s[0] >= 'A' && s[0] <= 'F') ? (s[0] - 'A' + 10)
        : -1;
    lo = (s[1] >= '0' && s[1] <= '9') ? (s[1] - '0')
        : (s[1] >= 'a' && s[1] <= 'f') ? (s[1] - 'a' + 10)
        : (s[1] >= 'A' && s[1] <= 'F') ? (s[1] - 'A' + 10)
        : -1;
    if (hi < 0 || lo < 0) return MM_FALSE;
    *out = (mm_u8)((hi << 4) | lo);
    return MM_TRUE;
}

static int load_ihex(const char *path,
                     const struct mm_target_cfg *cfg,
                     const struct mm_load_targets *targets,
                     mm_u32 offset,
                     mm_u32 *start_out,
                     mm_u32 *end_out,
                     size_t *loaded_out)
{
    FILE *f;
    char line[1024];
    mm_u32 upper = 0;
    mm_u32 min_addr = 0xffffffffu;
    mm_u32 max_addr = 0u;
    size_t loaded = 0;
    mm_bool any = MM_FALSE;

    if (start_out) *start_out = 0u;
    if (end_out) *end_out = 0u;
    if (loaded_out) *loaded_out = 0u;
    if (cfg == 0 || targets == 0) {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        mm_u8 count;
        mm_u8 rectype;
        mm_u8 b;
        mm_u32 addr;
        mm_u32 full;
        int i;
        if (line[0] != ':') {
            continue;
        }
        if (!parse_ihex_byte(line + 1, &count)) {
            continue;
        }
        if (!parse_ihex_byte(line + 3, &b)) {
            continue;
        }
        addr = (mm_u32)b << 8;
        if (!parse_ihex_byte(line + 5, &b)) {
            continue;
        }
        addr |= b;
        if (!parse_ihex_byte(line + 7, &rectype)) {
            continue;
        }
        if (rectype == 0x00u) {
            full = (upper << 16) + addr;
            for (i = 0; i < count; ++i) {
                mm_u8 byte;
                mm_u32 addr_abs = full + (mm_u32)i;
                mm_u32 off = 0;
                enum mm_load_target target;
                mm_u8 *dst;
                size_t pos = 9u + (size_t)i * 2u;
                if (!parse_ihex_byte(line + pos, &byte)) {
                    break;
                }
                if (!map_target_offset(cfg, targets, addr_abs, &target, &off)) {
                    continue;
                }
                if (target == MM_LOAD_FLASH && addr_abs < cfg->flash_size_s && offset != 0u) {
                    off += offset;
                }
                dst = load_target_ptr(targets, target, off, 1u, 0);
                if (dst == 0) {
                    continue;
                }
                *dst = byte;
            }
            if (full < min_addr) min_addr = full;
            if (full + (mm_u32)count > max_addr) max_addr = full + (mm_u32)count;
            loaded += count;
            any = MM_TRUE;
        } else if (rectype == 0x01u) {
            break;
        } else if (rectype == 0x04u) {
            mm_u8 hi, lo;
            if (parse_ihex_byte(line + 9, &hi) && parse_ihex_byte(line + 11, &lo)) {
                upper = ((mm_u32)hi << 8) | (mm_u32)lo;
            }
        } else if (rectype == 0x02u) {
            mm_u8 hi, lo;
            if (parse_ihex_byte(line + 9, &hi) && parse_ihex_byte(line + 11, &lo)) {
                upper = (((mm_u32)hi << 8) | (mm_u32)lo) >> 4;
            }
        }
    }
    fclose(f);
    if (!any) {
        return -1;
    }
    if (start_out) *start_out = min_addr;
    if (end_out) *end_out = max_addr;
    if (loaded_out) *loaded_out = loaded;
    return 0;
}

static mm_u32 uf2_u32(const mm_u8 *p)
{
    return (mm_u32)p[0] | ((mm_u32)p[1] << 8) | ((mm_u32)p[2] << 16) | ((mm_u32)p[3] << 24);
}

static mm_bool uf2_addr_in_ram(const struct mm_target_cfg *cfg, mm_u32 addr, mm_u32 size)
{
    mm_u32 off;
    if (cfg == 0 || size == 0u) return MM_FALSE;
    if (!map_ram_offset(cfg, addr, &off)) return MM_FALSE;
    if (!map_ram_offset(cfg, addr + size - 1u, &off)) return MM_FALSE;
    return MM_TRUE;
}

static mm_bool uf2_family_allowed_rp2350(mm_u32 family_id, mm_bool ram_target)
{
    if (ram_target) {
        return (family_id == UF2_FAMILY_ID_ABSOLUTE ||
                family_id == UF2_FAMILY_ID_DATA ||
                family_id == UF2_FAMILY_ID_RP2350_ARM_S ||
                family_id == UF2_FAMILY_ID_RP2350_RISCV) ? MM_TRUE : MM_FALSE;
    }
    return (family_id == UF2_FAMILY_ID_ABSOLUTE ||
            family_id == UF2_FAMILY_ID_DATA ||
            family_id == UF2_FAMILY_ID_RP2350_ARM_S ||
            family_id == UF2_FAMILY_ID_RP2350_ARM_NS ||
            family_id == UF2_FAMILY_ID_RP2350_RISCV) ? MM_TRUE : MM_FALSE;
}

static mm_bool uf2_flash_addr_allowed(const struct rp2350_partition_table *pt, mm_u32 addr)
{
    mm_u32 count;
    mm_u32 i;
    if (pt == 0 || pt->loaded == 0u) {
        return MM_TRUE;
    }
    count = (pt->permission_partition_count != 0u) ? pt->permission_partition_count : pt->partition_count;
    if (count > RP2350_PARTITION_TABLE_MAX_PARTITIONS) {
        count = RP2350_PARTITION_TABLE_MAX_PARTITIONS;
    }
    for (i = 0; i < count; ++i) {
        mm_u32 loc = pt->partitions[i].permissions_and_location;
        mm_u32 first = (loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                       PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
        mm_u32 last = (loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                      PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
        mm_u32 start = 0x10000000u + first * 4096u;
        mm_u32 end = 0x10000000u + (last + 1u) * 4096u;
        if (addr >= start && addr < end) {
            return (pt->partitions[i].permissions_and_flags & PICOBIN_PARTITION_PERMISSION_NSBOOT_W_BITS) != 0u;
        }
    }
    return (pt->unpartitioned_space_permissions_and_flags & PICOBIN_PARTITION_PERMISSION_NSBOOT_W_BITS) != 0u;
}

static mm_bool uf2_should_ignore_block(const mm_u8 *block)
{
    mm_u32 flags = uf2_u32(block + 8);
    if ((flags & UF2_FLAG_EXTENSION_FLAGS_PRESENT) != 0u) {
        mm_u32 ext = uf2_u32(block + 32 + UF2_PAYLOAD_SIZE);
        if (ext == UF2_EXTENSION_RP2_IGNORE_BLOCK) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool uf2_block_valid(const mm_u8 *block, mm_bool rp2350)
{
    mm_u32 magic0 = uf2_u32(block + 0);
    mm_u32 magic1 = uf2_u32(block + 4);
    mm_u32 magic_end = uf2_u32(block + 508);
    mm_u32 flags = uf2_u32(block + 8);
    mm_u32 payload = uf2_u32(block + 16);
    if (magic0 != UF2_MAGIC_START0 || magic1 != UF2_MAGIC_START1 || magic_end != UF2_MAGIC_END) {
        return MM_FALSE;
    }
    if ((flags & UF2_FLAG_FILE_CONTAINER) != 0u) {
        return MM_FALSE;
    }
    if ((flags & UF2_FLAG_NOT_MAIN_FLASH) != 0u) {
        return MM_FALSE;
    }
    if (payload != UF2_PAYLOAD_SIZE) {
        return MM_FALSE;
    }
    if (rp2350 && (flags & UF2_FLAG_FAMILY_ID_PRESENT) == 0u) {
        return MM_FALSE;
    }
    if (uf2_should_ignore_block(block)) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool uf2_scan_for_ram_boot(const char *path,
                                     const struct mm_target_cfg *cfg,
                                     const char *cpu_name,
                                     mm_bool *ram_out)
{
    FILE *f;
    mm_u8 block[UF2_BLOCK_SIZE];
    size_t n;
    mm_bool rp2350 = (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) ? MM_TRUE : MM_FALSE;
    mm_bool found = MM_FALSE;
    if (ram_out) *ram_out = MM_FALSE;
    if (path == 0 || cfg == 0 || ram_out == 0) return MM_FALSE;
    if (detect_image_type(path) != MM_IMAGE_UF2) return MM_FALSE;
    f = fopen(path, "rb");
    if (f == NULL) return MM_FALSE;
    while ((n = fread(block, 1, UF2_BLOCK_SIZE, f)) == UF2_BLOCK_SIZE) {
        mm_u32 flags;
        mm_u32 target_addr;
        mm_u32 family_id;
        mm_bool ram_target;
        if (!uf2_block_valid(block, rp2350)) {
            continue;
        }
        flags = uf2_u32(block + 8);
        target_addr = uf2_u32(block + 12);
        family_id = uf2_u32(block + 28);
        ram_target = uf2_addr_in_ram(cfg, target_addr, UF2_PAYLOAD_SIZE);
        if (rp2350 && (flags & UF2_FLAG_FAMILY_ID_PRESENT) != 0u) {
            if (!uf2_family_allowed_rp2350(family_id, ram_target)) {
                continue;
            }
        }
        found = MM_TRUE;
        if (ram_target) {
            *ram_out = MM_TRUE;
            break;
        }
    }
    fclose(f);
    return found;
}

static int load_uf2(const char *path,
                    const struct mm_target_cfg *cfg,
                    const struct mm_load_targets *targets,
                    const char *cpu_name,
                    mm_u32 *start_out,
                    mm_u32 *end_out,
                    size_t *loaded_out)
{
    FILE *f;
    mm_u8 block[UF2_BLOCK_SIZE];
    size_t n;
    mm_u32 min_addr = 0xffffffffu;
    mm_u32 max_addr = 0u;
    size_t loaded = 0;
    mm_bool any = MM_FALSE;
    mm_bool rp2350 = (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) ? MM_TRUE : MM_FALSE;
    mm_u32 expected_family = 0u;
    mm_bool has_family = MM_FALSE;

    if (start_out) *start_out = 0u;
    if (end_out) *end_out = 0u;
    if (loaded_out) *loaded_out = 0u;
    if (path == 0 || cfg == 0 || targets == 0) return -1;

    f = fopen(path, "rb");
    if (f == NULL) return -1;

    while ((n = fread(block, 1, UF2_BLOCK_SIZE, f)) == UF2_BLOCK_SIZE) {
        mm_u32 flags;
        mm_u32 target_addr;
        mm_u32 payload;
        mm_u32 family_id;
        mm_u32 off;
        enum mm_load_target target;
        mm_u8 *dst;
        mm_bool ram_target;
        const struct rp2350_partition_table *pt;

        if (!uf2_block_valid(block, rp2350)) {
            continue;
        }
        flags = uf2_u32(block + 8);
        target_addr = uf2_u32(block + 12);
        payload = uf2_u32(block + 16);
        family_id = uf2_u32(block + 28);
        if ((flags & UF2_FLAG_FAMILY_ID_PRESENT) != 0u) {
            if (!has_family) {
                expected_family = family_id;
                has_family = MM_TRUE;
            } else if (family_id != expected_family) {
                fclose(f);
                return -1;
            }
        }
        ram_target = uf2_addr_in_ram(cfg, target_addr, payload);
        if (rp2350 && has_family) {
            if (!uf2_family_allowed_rp2350(expected_family, ram_target)) {
                fclose(f);
                return -1;
            }
            if (!ram_target) {
                pt = mm_rp2350_partition_table_get();
                if (!uf2_flash_addr_allowed(pt, target_addr)) {
                    fclose(f);
                    return -1;
                }
            }
        }
        if (!map_target_offset(cfg, targets, target_addr, &target, &off)) {
            fclose(f);
            return -1;
        }
        dst = load_target_ptr(targets, target, off, payload, 0);
        if (dst == 0) {
            fclose(f);
            return -1;
        }
        memcpy(dst, block + 32, payload);
        if (target_addr < min_addr) min_addr = target_addr;
        if (target_addr + payload > max_addr) max_addr = target_addr + payload;
        loaded += payload;
        any = MM_TRUE;
    }
    fclose(f);
    if (!any) {
        return -1;
    }
    if (start_out) *start_out = min_addr;
    if (end_out) *end_out = max_addr;
    if (loaded_out) *loaded_out = loaded;
    return 0;
}

static int load_image_autodetect(struct mm_image_spec *img,
                                 const struct mm_target_cfg *cfg,
                                 const struct mm_load_targets *targets,
                                 enum mm_boot_mode boot_mode,
                                 const char *cpu_name)
{
    mm_u32 start = 0;
    mm_u32 end = 0;
    size_t loaded = 0;
    mm_u32 base_addr = 0;
    mm_u32 off = 0;
    enum mm_load_target target = MM_LOAD_FLASH;
    if (img == 0 || img->path == 0) {
        return -1;
    }
    img->type = detect_image_type(img->path);
    img->loaded = 0;
    img->load_start = 0;
    img->load_end = 0;
    switch (img->type) {
    case MM_IMAGE_ELF:
        if (img->offset != 0u) {
            printf("warning: ignoring offset for ELF image %s\n", img->path);
        }
        if (load_elf_segments(img->path, cfg, targets, img->offset, &start, &end, &loaded) != 0) {
            fprintf(stderr, "failed to load ELF image %s (missing libelf?)\n", img->path);
            return -1;
        }
        img->loaded = loaded;
        img->load_start = start;
        img->load_end = end;
        break;
    case MM_IMAGE_IHEX:
        if (img->offset != 0u) {
            printf("warning: applying offset to IHEX image %s\n", img->path);
        }
        if (load_ihex(img->path, cfg, targets, img->offset, &start, &end, &loaded) != 0) {
            return -1;
        }
        img->loaded = loaded;
        img->load_start = start;
        img->load_end = end;
        break;
    case MM_IMAGE_UF2:
        if (img->offset != 0u) {
            printf("warning: ignoring offset for UF2 image %s\n", img->path);
        }
        if (load_uf2(img->path, cfg, targets, cpu_name, &start, &end, &loaded) != 0) {
            return -1;
        }
        img->loaded = loaded;
        img->load_start = start;
        img->load_end = end;
        break;
    case MM_IMAGE_BIN:
    default:
        if (boot_mode == MM_BOOT_RAM) {
            target = MM_LOAD_RAM;
        } else if (boot_mode == MM_BOOT_SPIFLASH) {
            target = MM_LOAD_SPIFLASH;
        } else {
            target = MM_LOAD_FLASH;
        }
        if (target == MM_LOAD_RAM) {
            base_addr = cfg->ram_base_s;
        } else if (target == MM_LOAD_SPIFLASH) {
            base_addr = targets->spiflash_base;
        } else {
            base_addr = cfg->flash_base_s;
        }
        off = img->offset;
        if (target == MM_LOAD_RAM && targets->ram == 0) return -1;
        if (target == MM_LOAD_SPIFLASH && targets->spiflash == 0) return -1;
        if (load_file_at(img->path,
                         (target == MM_LOAD_RAM) ? targets->ram : (target == MM_LOAD_SPIFLASH) ? targets->spiflash : targets->flash,
                         (target == MM_LOAD_RAM) ? targets->ram_size : (target == MM_LOAD_SPIFLASH) ? targets->spiflash_size : targets->flash_size,
                         off,
                         &loaded) != 0) {
            return -1;
        }
        img->type = MM_IMAGE_BIN;
        img->loaded = loaded;
        img->load_start = base_addr + img->offset;
        img->load_end = base_addr + img->offset + (mm_u32)loaded;
        break;
    }
    return 0;
}

static mm_bool parse_u32(const char *s, mm_u32 *out)
{
    unsigned long v;
    char *end;
    if (s == 0 || out == 0) {
        return MM_FALSE;
    }
    v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') {
        return MM_FALSE;
    }
    *out = (mm_u32)v;
    return MM_TRUE;
}

static mm_bool parse_usb_spec(const char *spec, char *udc_out, size_t udc_len)
{
    const char *value;
    size_t len;
    if (spec == 0 || udc_out == 0 || udc_len == 0u) return MM_FALSE;
    if (strncmp(spec, "udc=", 4) == 0) {
        value = spec + 4;
    } else if (strncmp(spec, "path=", 5) == 0) {
        value = spec + 5;
    } else {
        return MM_FALSE;
    }
    if (value[0] == '\0') {
        return MM_FALSE;
    }
    len = strlen(value);
    if (len + 1u > udc_len) {
        return MM_FALSE;
    }
    memcpy(udc_out, value, len + 1u);
    return MM_TRUE;
}

static struct mm_decoded decode_t32_fast(const struct mm_fetch_result *fetch,
                                         const struct mm_cpu *cpu,
                                         const struct mm_scs *scs)
{
    struct mm_decoded d;
    if (fetch == 0) {
        d.kind = MM_OP_UNDEFINED;
        d.cond = MM_COND_AL;
        d.rd = 0;
        d.rn = 0;
        d.rm = 0;
        d.ra = 0;
        d.imm = 0;
        d.len = 0;
        d.raw = 0;
        d.undefined = MM_TRUE;
        return d;
    }
    if (capstone_is_enabled()) {
        return mm_decode_t32(fetch);
    }
    if (fetch->len == 4u && cpu != 0 && scs != 0 &&
        !fpu_access_allowed(cpu, scs) && mm_is_vfp_insn_fast(fetch->insn)) {
        d.kind = MM_OP_UNDEFINED;
        d.cond = MM_COND_AL;
        d.rd = 0;
        d.rn = 0;
        d.rm = 0;
        d.ra = 0;
        d.imm = 0;
        d.len = fetch->len;
        d.raw = fetch->insn;
        d.undefined = MM_TRUE;
        return d;
    }
    return mm_decode_t32(fetch);
}

static volatile mm_bool g_system_reset_pending = MM_FALSE;
static volatile mm_bool g_boot_override_pending = MM_FALSE;
static volatile mm_u32 g_boot_override_mode = 0u;

void mm_system_request_reset(void)
{
    g_system_reset_pending = MM_TRUE;
}

void mm_system_request_reset_boot(int mode)
{
    g_boot_override_mode = (mm_u32)mode;
    g_boot_override_pending = MM_TRUE;
    g_system_reset_pending = MM_TRUE;
}

static mm_bool mm_system_reset_pending(void)
{
    return g_system_reset_pending;
}

static void mm_system_clear_reset(void)
{
    g_system_reset_pending = MM_FALSE;
}

static char *dup_range(const char *s, size_t n)
{
    char *p;
    size_t i;
    p = (char *)malloc(n + 1u);
    if (p == NULL) {
        return NULL;
    }
    for (i = 0; i < n; ++i) {
        p[i] = s[i];
    }
    p[n] = '\0';
    return p;
}

static mm_bool parse_image_spec(const char *spec, char **path_out, mm_u32 *offset_out)
{
    const char *colon;
    mm_u32 off;
    size_t path_len;
    char *path;

    if (spec == 0 || path_out == 0 || offset_out == 0) {
        return MM_FALSE;
    }

    *path_out = 0;
    *offset_out = 0;

    colon = strrchr(spec, ':');
    if (colon != 0) {
        const char *os = colon + 1;
        if (os[0] != '\0' && (os[0] == '0' || (os[0] >= '1' && os[0] <= '9'))) {
            if (parse_u32(os, &off)) {
                path_len = (size_t)(colon - spec);
                path = dup_range(spec, path_len);
                if (path == NULL) {
                    return MM_FALSE;
                }
                *path_out = path;
                *offset_out = off;
                return MM_TRUE;
            }
        }
    }

    path_len = strlen(spec);
    path = dup_range(spec, path_len);
    if (path == NULL) {
        return MM_FALSE;
    }
    *path_out = path;
    *offset_out = 0;
    return MM_TRUE;
}

static mm_bool g_fault_pending = MM_FALSE;
static int g_stack_trace = -1;
static struct mm_prot_ctx *g_active_prot_ctx = 0;

static mm_bool stack_trace_enabled(void)
{
    if (g_stack_trace < 0) {
        const char *v = getenv("M33MU_STACK_TRACE");
        g_stack_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_stack_trace ? MM_TRUE : MM_FALSE;
}

static int g_svc_stack_trace = -1;

static mm_bool svc_stack_trace_enabled(void)
{
    if (g_svc_stack_trace < 0) {
        const char *v = getenv("M33MU_SVC_STACK_TRACE");
        g_svc_stack_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_svc_stack_trace ? MM_TRUE : MM_FALSE;
}

static mm_bool primask_blocks_current(const struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    if (cpu->sec_state == MM_NONSECURE) {
        return cpu->primask_ns != 0u;
    }
    return cpu->primask_s != 0u;
}

static mm_bool faultmask_blocks_current(const struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    if (cpu->sec_state == MM_NONSECURE) {
        return cpu->faultmask_ns != 0u;
    }
    return cpu->faultmask_s != 0u;
}

static mm_bool primask_blocks_for_sec(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    return (sec == MM_NONSECURE) ? (cpu->primask_ns != 0u) : (cpu->primask_s != 0u);
}

static mm_bool faultmask_blocks_for_sec(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    return (sec == MM_NONSECURE) ? (cpu->faultmask_ns != 0u) : (cpu->faultmask_s != 0u);
}

static mm_u8 basepri_for_sec(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return 0u;
    }
    return (sec == MM_NONSECURE) ? (mm_u8)(cpu->basepri_ns & 0xFFu)
                                 : (mm_u8)(cpu->basepri_s & 0xFFu);
}

static mm_u8 system_exc_priority_for_sec(const struct mm_scs *scs,
                                         enum mm_sec_state sec,
                                         mm_u32 exc_num);
static mm_bool basepri_blocks_system_exc_for_sec(const struct mm_cpu *cpu,
                                                 const struct mm_scs *scs,
                                                 enum mm_sec_state sec,
                                                 mm_u32 exc_num);

static mm_u8 aircr_prigroup_for_sec(const struct mm_scs *scs, enum mm_sec_state sec)
{
    mm_u32 aircr;
    if (scs == 0) {
        return 0u;
    }
    aircr = (sec == MM_NONSECURE) ? scs->aircr_ns : scs->aircr_s;
    return (mm_u8)((aircr >> 8) & 0x7u);
}

static mm_u8 preempt_priority_value(mm_u8 prio, mm_u8 prigroup)
{
    mm_u8 sub_bits;
    if (prigroup > 7u) {
        prigroup = 7u;
    }
    sub_bits = (mm_u8)(prigroup + 1u);
    if (sub_bits >= 8u) {
        return 0u;
    }
    return (mm_u8)(prio & (mm_u8)(0xFFu << sub_bits));
}

static mm_u8 system_exc_priority(const struct mm_cpu *cpu,
                                 const struct mm_scs *scs,
                                 mm_u32 exc_num)
{
    enum mm_sec_state sec;
    if (cpu == 0) {
        return 0xFFu;
    }
    sec = cpu->sec_state;
    return system_exc_priority_for_sec(scs, sec, exc_num);
}

static mm_u8 system_exc_priority_for_sec(const struct mm_scs *scs,
                                         enum mm_sec_state sec,
                                         mm_u32 exc_num)
{
    mm_u32 shpr1;
    mm_u32 shpr2;
    mm_u32 shpr3;
    if (scs == 0) {
        return 0xFFu;
    }
    shpr1 = (sec == MM_NONSECURE) ? scs->shpr1_ns : scs->shpr1_s;
    shpr2 = (sec == MM_NONSECURE) ? scs->shpr2_ns : scs->shpr2_s;
    shpr3 = (sec == MM_NONSECURE) ? scs->shpr3_ns : scs->shpr3_s;
    switch (exc_num) {
    case MM_VECT_MEMMANAGE:
        return (mm_u8)(shpr1 & 0xFFu);
    case MM_VECT_BUSFAULT:
        return (mm_u8)((shpr1 >> 8) & 0xFFu);
    case MM_VECT_USAGEFAULT:
        return (mm_u8)((shpr1 >> 16) & 0xFFu);
    case MM_VECT_SECUREFAULT:
        return (mm_u8)((shpr1 >> 24) & 0xFFu);
    case MM_VECT_SVCALL:
        return (mm_u8)((shpr2 >> 24) & 0xFFu);
    case MM_VECT_PENDSV:
        return (mm_u8)((shpr3 >> 16) & 0xFFu);
    case MM_VECT_SYSTICK:
        return (mm_u8)((shpr3 >> 24) & 0xFFu);
    case MM_VECT_NMI:
    case MM_VECT_HARDFAULT:
        return 0x00u;
    default:
        return 0x00u;
    }
}

static mm_bool current_execution_priority(const struct mm_cpu *cpu,
                                          const struct mm_scs *scs,
                                          const struct mm_nvic *nvic,
                                          mm_u8 *preempt_out,
                                          mm_u8 *raw_out)
{
    mm_u32 exc_num;
    mm_u8 raw_prio;
    mm_u8 prigroup;
    if (cpu == 0 || scs == 0 || preempt_out == 0 || raw_out == 0) {
        return MM_FALSE;
    }
    exc_num = cpu->xpsr & 0x1FFu;
    if (cpu->mode != MM_HANDLER || exc_num == 0u) {
        return MM_FALSE;
    }
    if (exc_num >= 16u) {
        if (nvic == 0) {
            return MM_FALSE;
        }
        raw_prio = nvic->priority[exc_num - 16u];
    } else {
        raw_prio = system_exc_priority(cpu, scs, exc_num);
    }
    prigroup = aircr_prigroup_for_sec(scs, cpu->sec_state);
    *raw_out = raw_prio;
    *preempt_out = preempt_priority_value(raw_prio, prigroup);
    return MM_TRUE;
}

static mm_bool basepri_blocks_system_exc(const struct mm_cpu *cpu,
                                         const struct mm_scs *scs,
                                         mm_u32 exc_num)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    return basepri_blocks_system_exc_for_sec(cpu, scs, cpu->sec_state, exc_num);
}

static mm_bool basepri_blocks_system_exc_for_sec(const struct mm_cpu *cpu,
                                                 const struct mm_scs *scs,
                                                 enum mm_sec_state sec,
                                                 mm_u32 exc_num)
{
    mm_u8 basepri;
    mm_u8 prio;
    mm_u8 prigroup;
    if (cpu == 0 || scs == 0) {
        return MM_FALSE;
    }
    basepri = basepri_for_sec(cpu, sec);
    if (basepri == 0u) {
        return MM_FALSE;
    }
    prio = system_exc_priority_for_sec(scs, sec, exc_num);
    if (prio == 0u) {
        return MM_FALSE;
    }
    prigroup = aircr_prigroup_for_sec(scs, sec);
    return preempt_priority_value(prio, prigroup) >=
           preempt_priority_value(basepri, prigroup) ? MM_TRUE : MM_FALSE;
}

static mm_bool systick_pending_select(const struct mm_cpu *cpu,
                                      const struct mm_scs *scs,
                                      enum mm_sec_state *handler_sec_out)
{
    mm_u8 s_prio;
    mm_u8 ns_prio;
    mm_u8 s_preempt;
    mm_u8 ns_preempt;

    if (cpu == 0 || scs == 0 || handler_sec_out == 0) {
        return MM_FALSE;
    }
    if (scs->pend_st_s && !scs->pend_st_ns) {
        *handler_sec_out = MM_SECURE;
        return MM_TRUE;
    }
    if (!scs->pend_st_s && scs->pend_st_ns) {
        *handler_sec_out = MM_NONSECURE;
        return MM_TRUE;
    }
    if (!scs->pend_st_s && !scs->pend_st_ns) {
        return MM_FALSE;
    }

    /* When both banks are pending, select the handler security by the
     * configured exception priority for each bank rather than forcing
     * Secure-first delivery. */
    s_prio = system_exc_priority_for_sec(scs, MM_SECURE, MM_VECT_SYSTICK);
    ns_prio = system_exc_priority_for_sec(scs, MM_NONSECURE, MM_VECT_SYSTICK);
    s_preempt = preempt_priority_value(s_prio, aircr_prigroup_for_sec(scs, MM_SECURE));
    ns_preempt = preempt_priority_value(ns_prio, aircr_prigroup_for_sec(scs, MM_NONSECURE));
    if (s_preempt < ns_preempt || (s_preempt == ns_preempt && s_prio <= ns_prio)) {
        *handler_sec_out = MM_SECURE;
    } else {
        *handler_sec_out = MM_NONSECURE;
    }
    return MM_TRUE;
}

static mm_bool prot_mux_interceptor(void *opaque, enum mm_access_type type, enum mm_sec_state sec, mm_u32 addr, mm_u32 size_bytes)
{
    (void)opaque;
    if (g_active_prot_ctx == 0) {
        return MM_TRUE;
    }
    return mm_prot_interceptor(g_active_prot_ctx, type, sec, addr, size_bytes);
}

/* Security attribute a bus transaction to `addr` carries, for peripherals that
 * enforce TZ attribution themselves (the flash TZ filter). */
static enum mm_sec_state prot_mux_bus_attr(void *opaque, mm_u32 addr)
{
    (void)opaque;
    if (g_active_prot_ctx == 0) {
        return MM_SECURE;
    }
    return mm_prot_bus_attr_for_addr(g_active_prot_ctx, addr);
}

static mm_bool allow_system_reset(const struct mm_target_cfg *cfg, const char *cpu_name)
{
    if (cfg == 0 || cpu_name == 0) {
        return MM_TRUE;
    }
    if (cfg->core_count > 1u && strcmp(cpu_name, "rp2350") == 0) {
        return mm_rp2350_core1_can_reset();
    }
    return MM_TRUE;
}

#define FP_STACK_WORDS 18u
#define FP_STACK_BYTES (FP_STACK_WORDS * 4u)
#define EXC_ADDITIONAL_STATE_WORDS 9u
#define EXC_ADDITIONAL_STATE_BYTES (EXC_ADDITIONAL_STATE_WORDS * 4u)
#define CPACR_CP10_SHIFT 20u
#define CPACR_CP11_SHIFT 22u
#define AIRCR_BFHFNMINS (1u << 13)
#define FPCCR_LSPACT (1u << 0)
#define FPCCR_LSPEN  (1u << 30)
#define FPCCR_ASPEN  (1u << 31)

static mm_u32 cpacr_for_sec(const struct mm_scs *scs, enum mm_sec_state sec)
{
    if (scs == 0) {
        return 0u;
    }
    return (sec == MM_NONSECURE) ? scs->cpacr_ns : scs->cpacr_s;
}

static mm_bool fpu_access_allowed(const struct mm_cpu *cpu, const struct mm_scs *scs)
{
    mm_u32 cp10;
    mm_u32 cp11;
    if (cpu == 0 || scs == 0 || !scs->fpu_present) {
        return MM_FALSE;
    }
    if (cpu->sec_state == MM_NONSECURE) {
        if (((scs->nsacr >> 10) & 0x1u) == 0u || ((scs->nsacr >> 11) & 0x1u) == 0u) {
            return MM_FALSE;
        }
    }
    cp10 = (cpacr_for_sec(scs, cpu->sec_state) >> CPACR_CP10_SHIFT) & 0x3u;
    cp11 = (cpacr_for_sec(scs, cpu->sec_state) >> CPACR_CP11_SHIFT) & 0x3u;
    if (cp10 == 0u || cp11 == 0u) {
        return MM_FALSE;
    }
    if (!mm_cpu_get_privileged(cpu) && (cp10 != 3u || cp11 != 3u)) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool fpu_stack_active(const struct mm_cpu *cpu, const struct mm_scs *scs)
{
    mm_u32 ctrl;
    if (!fpu_access_allowed(cpu, scs)) {
        return MM_FALSE;
    }
    ctrl = (cpu->sec_state == MM_NONSECURE) ? cpu->control_ns : cpu->control_s;
    if ((ctrl & (1u << 2)) == 0u) {
        return MM_FALSE;
    }
    return cpu->fp_active ? MM_TRUE : MM_FALSE;
}

static mm_bool fpu_auto_preserve_enabled(const struct mm_cpu *cpu, const struct mm_scs *scs)
{
    if (!fpu_stack_active(cpu, scs)) {
        return MM_FALSE;
    }
    return (scs->fpccr & FPCCR_ASPEN) != 0u ? MM_TRUE : MM_FALSE;
}

static mm_bool fpu_lazy_preserve_enabled(const struct mm_cpu *cpu, const struct mm_scs *scs)
{
    if (!fpu_auto_preserve_enabled(cpu, scs)) {
        return MM_FALSE;
    }
    return (scs->fpccr & FPCCR_LSPEN) != 0u ? MM_TRUE : MM_FALSE;
}

static void save_and_clear_secure_callee_regs(struct mm_cpu *cpu, int depth)
{
    int i;

    if (cpu == 0 || depth < 0 || depth >= MM_EXC_STACK_MAX) {
        return;
    }
    for (i = 0; i < 8; ++i) {
        cpu->exc_callee_saved[depth][i] = cpu->r[4 + i];
        cpu->r[4 + i] = 0u;
    }
    cpu->exc_cross_domain[depth] = MM_TRUE;
}

static mm_bool cross_domain_additional_state_required(enum mm_sec_state pre_sec,
                                                      enum mm_sec_state handler_sec)
{
    return (pre_sec == MM_SECURE && handler_sec == MM_NONSECURE) ? MM_TRUE : MM_FALSE;
}

static mm_u32 shcsr_active_mask_for_exc(mm_u32 exc_num)
{
    switch (exc_num) {
        case MM_VECT_MEMMANAGE: return (1u << 0);
        case MM_VECT_BUSFAULT: return (1u << 1);
        case MM_VECT_USAGEFAULT: return (1u << 3);
        case MM_VECT_SECUREFAULT: return (1u << 4);
        case MM_VECT_SVCALL: return (1u << 7);
        case MM_VECT_PENDSV: return (1u << 10);
        case MM_VECT_SYSTICK: return (1u << 11);
        default: return 0u;
    }
}

static void shcsr_set_exception_active(struct mm_scs *scs, enum mm_sec_state sec, mm_u32 exc_num)
{
    mm_u32 mask = shcsr_active_mask_for_exc(exc_num);
    if (scs == 0 || mask == 0u) {
        return;
    }
    if (sec == MM_NONSECURE) {
        scs->shcsr_ns |= mask;
    } else {
        scs->shcsr_s |= mask;
    }
}

static void shcsr_clear_exception_active(struct mm_scs *scs, enum mm_sec_state sec, mm_u32 exc_num)
{
    mm_u32 mask = shcsr_active_mask_for_exc(exc_num);
    if (scs == 0 || mask == 0u) {
        return;
    }
    if (sec == MM_NONSECURE) {
        scs->shcsr_ns &= ~mask;
    } else {
        scs->shcsr_s &= ~mask;
    }
}

static void scs_set_vectactive(struct mm_scs *scs, enum mm_sec_state sec, mm_u32 exc_num)
{
    mm_u32 *icsr = 0;

    if (scs == 0) {
        return;
    }
    icsr = (sec == MM_NONSECURE) ? &scs->icsr_ns : &scs->icsr_s;
    *icsr = (*icsr & ~0x1FFu) | (exc_num & 0x1FFu);
}

static mm_u32 exc_return_encode(enum mm_sec_state stack_sec,
                                enum mm_sec_state exception_sec,
                                mm_bool use_psp,
                                mm_bool to_thread,
                                mm_bool basic_frame,
                                mm_bool default_callee_stacking)
{
    /* EXC_RETURN encodings (Armv8-M, DDI0553):
     *  bits[31:24] = 0xFF, bits[23:7] = RES1
     *  bit6 S      = stack security (1=Secure, 0=Non-secure)
     *  bit5 DCRS   = 1 (default callee stacking)
     *  bit4 FType  = 1 (no FP context)
     *  bit3 Mode   = 1 Thread, 0 Handler
     *  bit2 SPSEL  = 1 PSP, 0 MSP
     *  bit1 RES0
     *  bit0 ES     = 1 Secure exception, 0 Non-secure exception
     */
    mm_u32 base = 0xFFFFFF80u;
    if (stack_sec == MM_SECURE) {
        base |= (1u << 6); /* Secure stack */
    }
    if (exception_sec == MM_SECURE) {
        base |= 1u;        /* Secure exception */
    }
    if (default_callee_stacking) {
        base |= (1u << 5); /* DCRS */
    }
    if (basic_frame) {
        base |= (1u << 4); /* FType */
    }
    if (to_thread) {
        base |= (1u << 3);
        if (use_psp) {
            base |= (1u << 2);
        }
    }
    return base;
}

static mm_bool tailchain_select_pending(const struct mm_cpu *cpu,
                                        const struct mm_scs *scs,
                                        mm_u32 *exc_num_out,
                                        enum mm_sec_state *handler_sec_out,
                                        mm_bool *is_irq_out,
                                        mm_u32 *irq_num_out)
{
    struct mm_nvic *nvic;
    enum mm_sec_state irq_sec = MM_SECURE;
    enum mm_sec_state systick_sec = MM_SECURE;
    int pend_irq;
    mm_bool have_best = MM_FALSE;
    mm_u8 best_preempt = 0xffu;
    mm_u8 best_raw_prio = 0xffu;

    if (cpu == 0 || scs == 0 || exc_num_out == 0 || handler_sec_out == 0 ||
        is_irq_out == 0 || irq_num_out == 0) {
        return MM_FALSE;
    }

    *is_irq_out = MM_FALSE;
    *irq_num_out = 0u;
    *handler_sec_out = cpu->sec_state;

    if (scs->pend_sv &&
        !primask_blocks_current(cpu) &&
        !faultmask_blocks_current(cpu) &&
        !basepri_blocks_system_exc(cpu, scs, MM_VECT_PENDSV)) {
        mm_u8 prigroup = aircr_prigroup_for_sec(scs, cpu->sec_state);
        mm_u8 prio = system_exc_priority(cpu, scs, MM_VECT_PENDSV);
        best_preempt = preempt_priority_value(prio, prigroup);
        best_raw_prio = prio;
        *exc_num_out = MM_VECT_PENDSV;
        *handler_sec_out = cpu->sec_state;
        *is_irq_out = MM_FALSE;
        *irq_num_out = 0u;
        have_best = MM_TRUE;
    }
    if (systick_pending_select(cpu, scs, &systick_sec) &&
        !primask_blocks_for_sec(cpu, systick_sec) &&
        !faultmask_blocks_for_sec(cpu, systick_sec) &&
        !basepri_blocks_system_exc_for_sec(cpu, scs, systick_sec, MM_VECT_SYSTICK)) {
        mm_u8 prigroup = aircr_prigroup_for_sec(scs, systick_sec);
        mm_u8 prio = system_exc_priority_for_sec(scs, systick_sec, MM_VECT_SYSTICK);
        mm_u8 preempt = preempt_priority_value(prio, prigroup);
        if (!have_best ||
            preempt < best_preempt ||
            (preempt == best_preempt && prio < best_raw_prio)) {
            best_preempt = preempt;
            best_raw_prio = prio;
            *exc_num_out = MM_VECT_SYSTICK;
            *handler_sec_out = systick_sec;
            *is_irq_out = MM_FALSE;
            *irq_num_out = 0u;
            have_best = MM_TRUE;
        }
    }

    nvic = nvic_for_cpu(cpu);
    if (nvic != 0) {
        pend_irq = mm_nvic_select_routed_ex(nvic, cpu, scs, &irq_sec);
        if (pend_irq >= 0) {
            mm_u8 prigroup = aircr_prigroup_for_sec(scs, irq_sec);
            mm_u8 prio = nvic->priority[pend_irq];
            mm_u8 preempt = preempt_priority_value(prio, prigroup);
            if (!have_best ||
                preempt < best_preempt ||
                (preempt == best_preempt && prio < best_raw_prio)) {
                best_preempt = preempt;
                best_raw_prio = prio;
                *exc_num_out = 16u + (mm_u32)pend_irq;
                *handler_sec_out = irq_sec;
                *is_irq_out = MM_TRUE;
                *irq_num_out = (mm_u32)pend_irq;
                have_best = MM_TRUE;
            }
        }
    }
    return have_best;
}

static mm_bool exc_return_unstack(struct mm_cpu *cpu,
                                  struct mm_memmap *map,
                                  struct mm_scs *scs,
                                  mm_u32 exc_ret)
{
    struct mm_exc_return_info info;
    mm_u32 tail_exc_num = 0u;
    enum mm_sec_state tail_handler_sec = MM_SECURE;
    mm_bool tail_is_irq = MM_FALSE;
    mm_u32 tail_irq_num = 0u;
    mm_u32 tail_handler = 0u;
    mm_u32 tail_current_exc_num = 0u;
    mm_u32 sp;
    mm_u32 frame[8];
    mm_u32 msp_s_val;
    mm_u32 msp_ns_val;
    mm_u32 psp_s_val;
    mm_u32 psp_ns_val;
    mm_u32 control_s_val;
    mm_u32 control_ns_val;
    mm_u16 exc_num = 0u;
    mm_u32 basic_sp;
    mm_u32 fp_sp;
    mm_u32 callee_sp;
    mm_u32 fp_reserved;
    int i;
    enum mm_sec_state handler_sec;

    info = mm_exc_return_decode(exc_ret);
    if (!info.valid) {
        if (stack_trace_enabled()) {
            printf("[EXC_UNSTACK] invalid exc_return=0x%08lx\n", (unsigned long)exc_ret);
        }
        return MM_FALSE;
    }
    if (svc_stack_trace_enabled() && (cpu->xpsr & 0x1ffu) == MM_VECT_SVCALL) {
        dump_cpu_regs(cpu, "SVC_RETURN_PRE");
        dump_exc_stack_state(cpu, "SVC_RETURN_PRE");
    }
    if (stack_trace_enabled()) {
        dump_cpu_regs(cpu, "EXC_UNSTACK_PRE");
        dump_exc_stack_state(cpu, "EXC_UNSTACK_PRE");
    }

    if (stack_trace_enabled()) {
        printf("[EXC_UNSTACK] exc_ret=0x%08lx target_sec=%d to_thread=%d use_psp=%d mode=%d cur_sec=%d msp_s=0x%08lx msp_ns=0x%08lx psp_s=0x%08lx psp_ns=0x%08lx ctrl_s=0x%08lx ctrl_ns=0x%08lx\n",
               (unsigned long)exc_ret,
               (int)info.target_sec,
               (int)info.to_thread,
               (int)info.use_psp,
               (int)cpu->mode,
               (int)cpu->sec_state,
               (unsigned long)cpu->msp_s,
               (unsigned long)cpu->msp_ns,
               (unsigned long)cpu->psp_s,
               (unsigned long)cpu->psp_ns,
               (unsigned long)cpu->control_s,
               (unsigned long)cpu->control_ns);
    }

    if (cpu->exc_depth > 0 && cpu->exc_depth <= MM_EXC_STACK_MAX) {
        tail_current_exc_num = cpu->exc_num[cpu->exc_depth - 1];
    }
    if (MM_FALSE &&
        info.to_thread &&
        tail_current_exc_num >= 16u &&
        cpu->exc_depth > 0 &&
        tailchain_select_pending(cpu, scs, &tail_exc_num, &tail_handler_sec, &tail_is_irq, &tail_irq_num) &&
        tail_is_irq) {
        {
            struct mm_nvic *nvic = nvic_for_cpu(cpu);
            if (nvic != 0) {
                mm_nvic_set_active(nvic, tail_current_exc_num - 16u, MM_FALSE);
            }
        }
        if (tail_is_irq) {
            struct mm_nvic *nvic = nvic_for_cpu(cpu);
            if (nvic != 0) {
                mm_nvic_set_pending(nvic, tail_irq_num, MM_FALSE);
                mm_nvic_set_active(nvic, tail_irq_num, MM_TRUE);
            }
            {
                mm_u32 vtor = (tail_handler_sec == MM_NONSECURE) ? scs->vtor_ns : scs->vtor_s;
                (void)mm_vector_read(map, tail_handler_sec, vtor, tail_exc_num, &tail_handler);
            }
        } else {
            (void)mm_exception_read_handler(map, scs, tail_handler_sec, (enum mm_vector_index)tail_exc_num, &tail_handler);
            if (tail_exc_num == MM_VECT_PENDSV) {
                scs->pend_sv = MM_FALSE;
            } else if (tail_exc_num == MM_VECT_SYSTICK) {
                if (tail_handler_sec == MM_NONSECURE) scs->pend_st_ns = MM_FALSE;
                else scs->pend_st_s = MM_FALSE;
            }
        }
        if (cpu->exc_depth > 0 && cpu->exc_depth <= MM_EXC_STACK_MAX) {
            cpu->exc_num[cpu->exc_depth - 1] = (mm_u16)tail_exc_num;
        }
        cpu->xpsr = (cpu->xpsr & 0xF8000000u) | 0x01000000u | (tail_exc_num & 0x1FFu);
        cpu->mode = MM_HANDLER;
        if (tail_handler_sec == MM_NONSECURE) {
            cpu->control_ns &= ~0x2u;
            cpu->r[13] = cpu->msp_ns;
        } else {
            cpu->control_s &= ~0x2u;
            cpu->r[13] = cpu->msp_s;
        }
        cpu->sec_state = tail_handler_sec;
        scs_set_vectactive(scs, tail_handler_sec, tail_exc_num);
        cpu->r[15] = tail_handler | 1u;
        cpu->sleeping = MM_FALSE;
        cpu->event_reg = MM_FALSE;
        return MM_TRUE;
    }

    /* Unstack from live PSP on Thread+PSP returns (OS may switch PSP).
     * For Thread+MSP returns, prefer recorded MSP for this exception level. */
    if (stack_trace_enabled() ||
        (svc_stack_trace_enabled() && (cpu->xpsr & 0x1ffu) == MM_VECT_SVCALL)) {
        printf("[EXC_STACK_POP] exc_ret=0x%08lx depth_before=%u\n",
               (unsigned long)exc_ret,
               (unsigned)cpu->exc_depth);
        dump_exc_stack_state(cpu, "EXC_STACK_POP_PRE");
    }
    if (cpu->exc_depth > 0) {
        cpu->exc_depth--;
    }
    if (cpu->exc_depth < MM_EXC_STACK_MAX) {
        exc_num = cpu->exc_num[cpu->exc_depth];
    }
    if (exc_num >= 16u) {
        struct mm_nvic *nvic = nvic_for_cpu(cpu);
        if (nvic != 0) {
            mm_nvic_set_active(nvic, (mm_u32)(exc_num - 16u), MM_FALSE);
        }
    }
    shcsr_clear_exception_active(scs, cpu->sec_state, exc_num);
    if (stack_trace_enabled() ||
        (svc_stack_trace_enabled() && (cpu->xpsr & 0x1ffu) == MM_VECT_SVCALL)) {
        printf("[EXC_STACK_POP] exc_ret=0x%08lx depth_after=%u\n",
               (unsigned long)exc_ret,
               (unsigned)cpu->exc_depth);
        dump_exc_stack_state(cpu, "EXC_STACK_POP_POST");
    }
    if (info.use_psp) {
        sp = (info.target_sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s;
    } else {
        sp = (info.target_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    }
    if (stack_trace_enabled() || svc_stack_trace_enabled()) {
        printf("[EXC_UNSTACK_PRE] exc_ret=0x%08lx use_psp=%d sp_start=0x%08lx msp_ns=0x%08lx psp_ns=0x%08lx\n",
               (unsigned long)exc_ret,
               (int)info.use_psp,
               (unsigned long)sp,
               (unsigned long)cpu->msp_ns,
               (unsigned long)cpu->psp_ns);
    }
    if (stack_trace_enabled()) {
        mm_u32 rec_sp = 0;
        mm_u8 rec_use_psp = 0;
        mm_u8 rec_sec = 0;
        if (cpu->exc_depth < MM_EXC_STACK_MAX) {
            rec_sp = cpu->exc_sp[cpu->exc_depth];
            rec_use_psp = cpu->exc_use_psp[cpu->exc_depth];
            rec_sec = cpu->exc_sec[cpu->exc_depth];
        }
        printf("[EXC_UNSTACK] chosen sp=0x%08lx exc_depth=%u rec_sp=0x%08lx rec_use_psp=%u rec_sec=%u\n",
               (unsigned long)sp,
               (unsigned)cpu->exc_depth,
               (unsigned long)rec_sp,
               (unsigned)rec_use_psp,
               (unsigned)rec_sec);
    }

    msp_s_val = cpu->msp_s;
    msp_ns_val = cpu->msp_ns;
    psp_s_val = cpu->psp_s;
    psp_ns_val = cpu->psp_ns;
    control_s_val = cpu->control_s;
    control_ns_val = cpu->control_ns;
    (void)msp_s_val; (void)msp_ns_val; (void)psp_s_val; (void)psp_ns_val;
    (void)control_s_val; (void)control_ns_val;

    basic_sp = sp;
    fp_sp = sp + 32u;
    callee_sp = fp_sp + (info.basic_frame ? 0u : FP_STACK_BYTES);
    if (!info.basic_frame) {
        mm_bool fp_saved = MM_TRUE;
        if (cpu->exc_depth < MM_EXC_STACK_MAX &&
            cpu->exc_fp_reserved[cpu->exc_depth] &&
            !cpu->exc_fp_saved[cpu->exc_depth]) {
            fp_saved = MM_FALSE;
        }
        if (fp_saved) {
            for (i = 0; i < 16; ++i) {
                if (!mm_memmap_read(map, info.target_sec, fp_sp + (mm_u32)(i * 4u), 4u, &cpu->s[i])) {
                    record_bus_fault(scs, info.target_sec, fp_sp + (mm_u32)(i * 4u), BFSR_UNSTKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                    return raise_hard_fault(cpu, map, scs, cpu->r[15] & ~1u, cpu->xpsr);
                }
            }
            if (!mm_memmap_read(map, info.target_sec, fp_sp + (16u * 4u), 4u, &cpu->fpscr)) {
                record_bus_fault(scs, info.target_sec, fp_sp + (16u * 4u), BFSR_UNSTKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                return raise_hard_fault(cpu, map, scs, cpu->r[15] & ~1u, cpu->xpsr);
            }
            if (!mm_memmap_read(map, info.target_sec, fp_sp + (17u * 4u), 4u, &fp_reserved)) {
                record_bus_fault(scs, info.target_sec, fp_sp + (17u * 4u), BFSR_UNSTKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                return raise_hard_fault(cpu, map, scs, cpu->r[15] & ~1u, cpu->xpsr);
            }
            (void)fp_reserved;
            cpu->fp_active = MM_TRUE;
            if (info.target_sec == MM_NONSECURE) {
                cpu->control_ns |= (1u << 2);
            } else {
                cpu->control_s |= (1u << 2);
            }
        }
        scs->fpcar = fp_sp;
        scs->fpccr &= ~FPCCR_LSPACT;
    }
    if (!info.default_callee_stacking &&
        cpu->exc_depth < MM_EXC_STACK_MAX &&
        cpu->exc_cross_domain[cpu->exc_depth]) {
        for (i = 0; i < 8; ++i) {
            if (!mm_memmap_read(map, info.target_sec, callee_sp + (mm_u32)(i * 4u), 4u, &cpu->r[4 + i])) {
                record_bus_fault(scs, info.target_sec, callee_sp + (mm_u32)(i * 4u), BFSR_UNSTKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                return raise_hard_fault(cpu, map, scs, cpu->r[15] & ~1u, cpu->xpsr);
            }
            cpu->exc_callee_saved[cpu->exc_depth][i] = 0u;
        }
        cpu->exc_cross_domain[cpu->exc_depth] = MM_FALSE;
    }
    for (i = 0; i < 8; ++i) {
        if (!mm_memmap_read(map, info.target_sec, basic_sp + (mm_u32)(i * 4), 4u, &frame[i])) {
            record_bus_fault(scs, info.target_sec, basic_sp + (mm_u32)(i * 4), BFSR_UNSTKERR | BFSR_PRECISERR | BFSR_BFARVALID);
            return raise_hard_fault(cpu, map, scs, cpu->r[15] & ~1u, cpu->xpsr);
        }
    }
    if (stack_trace_enabled() || svc_stack_trace_enabled()) {
        printf("[EXC_UNSTACK_FRAME] r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx lr=%08lx pc=%08lx xpsr=%08lx\n",
               (unsigned long)frame[0],
               (unsigned long)frame[1],
               (unsigned long)frame[2],
               (unsigned long)frame[3],
               (unsigned long)frame[4],
               (unsigned long)frame[5],
               (unsigned long)frame[6],
               (unsigned long)frame[7]);
    }
    if (svc_stack_trace_enabled() &&
        info.use_psp &&
        info.to_thread &&
        (cpu->xpsr & 0x1ffu) == MM_VECT_SVCALL) {
        mm_u32 sp_after = sp + 32u;
        if ((frame[7] & (1u << 9)) != 0u) {
            sp_after += 4u;
        }
        printf("[SVC_STACK_RETURN] sp=0x%08lx sp_after=0x%08lx r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx r12=0x%08lx lr=0x%08lx pc=0x%08lx xpsr=0x%08lx psp_ns=0x%08lx ctrl_ns=0x%08lx handler_lr=0x%08lx ipsr=%lu msp_ns=0x%08lx\n",
               (unsigned long)sp,
               (unsigned long)sp_after,
               (unsigned long)frame[0],
               (unsigned long)frame[1],
               (unsigned long)frame[2],
               (unsigned long)frame[3],
               (unsigned long)frame[4],
               (unsigned long)frame[5],
               (unsigned long)frame[6],
               (unsigned long)frame[7],
               (unsigned long)cpu->psp_ns,
               (unsigned long)cpu->control_ns,
               (unsigned long)exc_ret,
               (unsigned long)(cpu->xpsr & 0x1ffu),
               (unsigned long)cpu->msp_ns);
    }
    {
        mm_u32 pc_raw = frame[6];
        mm_bool pc_suspect = MM_FALSE;
        if (pc_raw == 0u || pc_raw == 0xffffffffu || pc_raw >= 0xF0000000u) {
            pc_suspect = MM_TRUE;
        } else if (map->flash.buffer != 0) {
            if (pc_raw < map->flash.base || pc_raw >= (map->flash.base + map->flash.length)) {
                pc_suspect = MM_TRUE;
            }
        }
        if (pc_suspect && stack_trace_enabled()) {
            printf("[EXC_UNSTACK] sec=%d sp=0x%08lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx lr=%08lx pc=%08lx xpsr=%08lx\n",
                   (int)info.target_sec,
                   (unsigned long)sp,
                   (unsigned long)frame[0],
                   (unsigned long)frame[1],
                   (unsigned long)frame[2],
                   (unsigned long)frame[3],
                   (unsigned long)frame[4],
                   (unsigned long)frame[5],
                   (unsigned long)frame[6],
                   (unsigned long)frame[7]);
        }
    }

    /* Armv8-M EXC_RETURN unstack, basic frame (DDI0553, Exception return behavior). */
    cpu->r[0] = frame[0];
    cpu->r[1] = frame[1];
    cpu->r[2] = frame[2];
    cpu->r[3] = frame[3];
    cpu->r[12] = frame[4];
    cpu->r[14] = frame[5];
    cpu->r[15] = frame[6] | 1u;
    if (info.to_thread) {
        /* Thread mode must resume with IPSR=0. */
        cpu->xpsr = frame[7] & ~0x1FFu;
    } else {
        /* Handler return keeps stacked IPSR. */
        cpu->xpsr = frame[7];
    }

    sp = basic_sp + 32u;
    if (!info.basic_frame) {
        sp += FP_STACK_BYTES;
    }
    if (!info.default_callee_stacking) {
        sp += EXC_ADDITIONAL_STATE_BYTES;
    }
    if ((frame[7] & (1u << 9)) != 0u) {
        sp += 4u;
    }
    if (info.use_psp) {
        if (info.target_sec == MM_NONSECURE) cpu->psp_ns = sp;
        else cpu->psp_s = sp;
    } else {
        if (info.target_sec == MM_NONSECURE) cpu->msp_ns = sp;
        else cpu->msp_s = sp;
    }
    if (stack_trace_enabled() || svc_stack_trace_enabled()) {
        printf("[EXC_UNSTACK_SP] exc_ret=0x%08lx use_psp=%d sp_final=0x%08lx\n",
               (unsigned long)exc_ret,
               (int)info.use_psp,
               (unsigned long)sp);
    }
    /*
     * On exception return to Thread mode, EXC_RETURN.SPSEL selects the
     * stack used for unstacking. Some firmware (e.g., frosted) programs
     * CONTROL.SPSEL in the handler to switch to PSP after return; preserve
     * an already-set SPSEL bit rather than unconditionally clearing it.
     */
    if (info.to_thread) {
        if (info.return_sec == MM_NONSECURE) {
            if (info.use_psp) {
                cpu->control_ns |= 0x2u;
            } else {
                cpu->control_ns &= ~0x2u;
            }
        } else {
            if (info.use_psp) {
                cpu->control_s |= 0x2u;
            } else {
                cpu->control_s &= ~0x2u;
            }
        }
    }
    /* Mirror active SP into R13: handler always MSP; thread uses CONTROL.SPSEL. */
    if (!info.to_thread) {
        cpu->r[13] = (info.return_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    } else {
        mm_u32 ctrl = (info.return_sec == MM_NONSECURE) ? cpu->control_ns : cpu->control_s;
        if ((ctrl & 0x2u) != 0u) {
            cpu->r[13] = (info.return_sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s;
        } else {
            cpu->r[13] = (info.return_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
        }
    }

    if (stack_trace_enabled() && cpu->sec_state != info.return_sec) {
        printf("[SEC_STATE] exc_return sec=%d->%d to_thread=%d use_psp=%d\n",
               (int)cpu->sec_state,
               (int)info.return_sec,
               (int)info.to_thread,
               (int)info.use_psp);
    }
    handler_sec = cpu->sec_state;
    cpu->sec_state = info.return_sec;
    cpu->mode = info.to_thread ? MM_THREAD : MM_HANDLER;
    scs_set_vectactive(scs, info.return_sec, info.to_thread ? 0u : (cpu->xpsr & 0x1FFu));
    if (exc_num != MM_VECT_NMI) {
        if (handler_sec == MM_NONSECURE) cpu->faultmask_ns = 0u;
        else cpu->faultmask_s = 0u;
    }
    if (cpu->exc_depth < MM_EXC_STACK_MAX) {
        cpu->exc_fp_reserved[cpu->exc_depth] = MM_FALSE;
        cpu->exc_fp_saved[cpu->exc_depth] = MM_FALSE;
        cpu->exc_cross_domain[cpu->exc_depth] = MM_FALSE;
    }
    if (info.to_thread && exc_num >= 16u) {
        calltrace_log_irq_resume(cpu->r[15]);
    }
    if (stack_trace_enabled()) {
        printf("[EXC_UNSTACK] new pc=0x%08lx sp=0x%08lx r13=0x%08lx mode=%d sec=%d\n",
               (unsigned long)cpu->r[15],
               (unsigned long)((info.use_psp)
                   ? ((info.target_sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s)
                   : ((info.target_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s)),
               (unsigned long)cpu->r[13],
               (int)cpu->mode,
               (int)cpu->sec_state);
    }
    if (svc_stack_trace_enabled() && (cpu->xpsr & 0x1ffu) == 0u) {
        dump_cpu_regs(cpu, "SVC_RETURN_POST");
        dump_exc_stack_state(cpu, "SVC_RETURN_POST");
    }
    if (stack_trace_enabled()) {
        dump_cpu_regs(cpu, "EXC_UNSTACK_POST");
        dump_exc_stack_state(cpu, "EXC_UNSTACK_POST");
    }
    return MM_TRUE;
}

static mm_bool handle_pc_write(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               struct mm_scs *scs,
                               mm_u32 value,
                               mm_u8 *it_pattern,
                               mm_u8 *it_remaining,
                               mm_u8 *it_cond);
static mm_bool enter_exception(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               struct mm_scs *scs,
                               mm_u32 exc_num,
                               mm_u32 return_pc,
                               mm_u32 xpsr_in);
static mm_bool enter_exception_ex(struct mm_cpu *cpu,
                                  struct mm_memmap *map,
                                  struct mm_scs *scs,
                                  mm_u32 exc_num,
                                  mm_u32 return_pc,
                                  mm_u32 xpsr_in,
                                  enum mm_sec_state handler_sec);
static mm_bool raise_mem_fault(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               struct mm_scs *scs,
                               mm_u32 fault_pc,
                               mm_u32 fault_xpsr,
                               mm_u32 addr,
                               mm_bool is_exec);
static mm_bool raise_usage_fault(struct mm_cpu *cpu,
                                 struct mm_memmap *map,
                                 struct mm_scs *scs,
                                 mm_u32 fault_pc,
                                 mm_u32 fault_xpsr,
                                 mm_u32 ufsr_bits);

static mm_bool fault_clock_hit(mm_u64 cycle, const mm_u64 *clocks, mm_u8 count)
{
    mm_u8 i;
    if (cycle == 0u || clocks == 0 || count == 0u) return MM_FALSE;
    for (i = 0; i < count; ++i) {
        if (clocks[i] == cycle) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool fault_clock_add(mm_u64 value, mm_u64 *clocks, mm_u8 *count)
{
    mm_u8 i;
    mm_u8 cap;
    if (value == 0u || clocks == 0 || count == 0) return MM_FALSE;
    cap = 16u;
    if (*count >= cap) return MM_FALSE;
    for (i = 0; i < *count; ++i) {
        if (clocks[i] == value) {
            return MM_TRUE;
        }
        if (clocks[i] + 1u == value || value + 1u == clocks[i]) {
            return MM_FALSE;
        }
    }
    clocks[(*count)++] = value;
    return MM_TRUE;
}

static mm_bool step_core_simple(struct mm_cpu *cpu,
                                struct mm_scs *scs,
                                struct mm_nvic *nvic,
                                struct mm_memmap *map,
                                const struct mm_target_cfg *cfg,
                                const mm_u64 *fault_clocks,
                                mm_u8 fault_clock_count,
                                mm_u64 *cycle_total,
                                mm_u64 *vcycles,
                                mm_u64 *cycles_since_poll,
                                mm_u8 *it_pattern,
                                mm_u8 *it_remaining,
                                mm_u8 *it_cond,
                                mm_bool *done)
{
    mm_bool trace_started = MM_FALSE;
    mm_bool result = MM_TRUE;
    enum mm_sec_state systick_sec = MM_SECURE;
    if (cpu == 0 || scs == 0 || nvic == 0 || map == 0 || cfg == 0) {
        return MM_FALSE;
    }
    if (cpu->sleeping) {
        mm_bool wake = MM_FALSE;
        if (cpu->event_reg) {
            wake = MM_TRUE;
        } else if (systick_pending_select(cpu, scs, &systick_sec) || scs->pend_sv) {
            wake = MM_TRUE;
        } else if (mm_nvic_select_ex(nvic, cpu, scs) >= 0) {
            wake = MM_TRUE;
        } else if (cpu->sleep_wfe && mm_nvic_any_pending(nvic)) {
            wake = MM_TRUE;
        }
        if (!wake) {
            return MM_FALSE;
        }
        cpu->sleeping = MM_FALSE;
        cpu->sleep_wfe = MM_FALSE;
        cpu->event_reg = MM_FALSE;
    }


    if (!g_record_started && g_record_start_set &&
        ((cpu->r[15] & ~1u) == g_record_start_pc)) {
        mm_trace_reset();
        g_record_started = MM_TRUE;
        if (g_record_start_dump) {
            dump_record_start_context(cpu, map);
        }
        g_record_start_remaining = g_record_start_window;
    }
        if (g_record_stop_set && ((cpu->r[15] & ~1u) == g_record_stop_pc)) {
            fprintf(stderr, "[RECORD_STOP] pc=0x%08lx\n", (unsigned long)(cpu->r[15] & ~1u));
            dump_bytes(map, cpu->sec_state, g_record_dump_addr, 64u, "record_r_after (r0)");
            if (g_record_dump_count > 0u && mm_trace_enabled() && g_record_started) {
                dump_trace_tail(cpu, map, g_record_dump_count);
            }
            g_record_stop_set = MM_FALSE;
            g_record_started = MM_FALSE;
        }
    if (mm_trace_enabled() && g_record_started) {
        mm_trace_begin_step(cpu, cpu->r[15] & ~1u);
        trace_started = MM_TRUE;
    }

    if (scs->pend_sv) {
        if (primask_blocks_current(cpu) ||
            faultmask_blocks_current(cpu) ||
            basepri_blocks_system_exc(cpu, scs, MM_VECT_PENDSV)) {
            result = MM_TRUE;
            goto out;
        }
        {
            mm_u8 current_preempt = 0u;
            mm_u8 current_raw = 0u;
            mm_u8 pend_prio = system_exc_priority(cpu, scs, MM_VECT_PENDSV);
            mm_u8 pend_preempt = preempt_priority_value(pend_prio, aircr_prigroup_for_sec(scs, cpu->sec_state));
            if (current_execution_priority(cpu, scs, nvic, &current_preempt, &current_raw) &&
                pend_preempt >= current_preempt) {
                result = MM_TRUE;
                goto out;
            }
        }
        if (!enter_exception(cpu, map, scs, MM_VECT_PENDSV, cpu->r[15] & ~1u, cpu->xpsr)) {
            if (done) *done = MM_TRUE;
        } else {
            itstate_sync_from_xpsr(cpu->xpsr, it_pattern, it_remaining, it_cond);
        }
        result = MM_TRUE;
        goto out;
    }
    if (systick_pending_select(cpu, scs, &systick_sec)) {
        if (primask_blocks_for_sec(cpu, systick_sec) ||
            faultmask_blocks_for_sec(cpu, systick_sec) ||
            basepri_blocks_system_exc_for_sec(cpu, scs, systick_sec, MM_VECT_SYSTICK)) {
            result = MM_TRUE;
            goto out;
        }
        {
            mm_u8 current_preempt = 0u;
            mm_u8 current_raw = 0u;
            mm_u8 pend_prio = system_exc_priority_for_sec(scs, systick_sec, MM_VECT_SYSTICK);
            mm_u8 pend_preempt = preempt_priority_value(pend_prio, aircr_prigroup_for_sec(scs, systick_sec));
            if (current_execution_priority(cpu, scs, nvic, &current_preempt, &current_raw) &&
                pend_preempt >= current_preempt) {
                result = MM_TRUE;
                goto out;
            }
        }
        if (!enter_exception_ex(cpu, map, scs, MM_VECT_SYSTICK, cpu->r[15] & ~1u, cpu->xpsr, systick_sec)) {
            if (done) *done = MM_TRUE;
        } else {
            itstate_sync_from_xpsr(cpu->xpsr, it_pattern, it_remaining, it_cond);
        }
        result = MM_TRUE;
        goto out;
    }
    {
        enum mm_sec_state irq_sec = MM_SECURE;
        int pend_irq = mm_nvic_select_routed_ex(nvic, cpu, scs, &irq_sec);
        if (pend_irq >= 0) {
            mm_u32 exc_num = 16u + (mm_u32)pend_irq;
            mm_nvic_set_pending(nvic, (mm_u32)pend_irq, MM_FALSE);
            mm_nvic_set_active(nvic, (mm_u32)pend_irq, MM_TRUE);
            if (!enter_exception_ex(cpu, map, scs, exc_num, cpu->r[15] & ~1u, cpu->xpsr, irq_sec)) {
                if (done) *done = MM_TRUE;
            } else {
                itstate_sync_from_xpsr(cpu->xpsr, it_pattern, it_remaining, it_cond);
            }
            result = MM_TRUE;
            goto out;
        }
    }

    {
        struct mm_fetch_result f;
        struct mm_decoded d;
        mm_bool execute_it;
        mm_bool fault_skip = MM_FALSE;
        const mm_u32 insn_cycles = 1u;

        if (cycle_total) *cycle_total += insn_cycles;
        if (vcycles) *vcycles += insn_cycles;
        if (cycles_since_poll) *cycles_since_poll += insn_cycles;
        mm_scs_systick_advance(scs, insn_cycles);
        mm_timer_tick(cfg, insn_cycles);
        if (cycle_total && fault_clock_hit(*cycle_total, fault_clocks, fault_clock_count)) {
            fault_skip = MM_TRUE;
        }

        cpu->r[13] = mm_cpu_get_active_sp(cpu);

        if (mm_bootapi_handle(cpu, map)) {
            result = MM_TRUE;
            goto out;
        }

        f = mm_fetch_t32_memmap(cpu, map, cpu->sec_state);
        if (f.fault) {
            if (!raise_mem_fault(cpu, map, scs, cpu->r[15] & ~1u, cpu->xpsr, f.fault_addr, MM_TRUE)) {
                if (done) *done = MM_TRUE;
            }
            result = MM_TRUE;
            goto out;
        }
        d = decode_t32_fast(&f, cpu, scs);
        mm_memmap_set_last_pc(f.pc_fetch);
        if (d.undefined) {
            if (!raise_usage_fault(cpu, map, scs, f.pc_fetch, cpu->xpsr, (1u << 16))) {
                if (done) *done = MM_TRUE;
            }
            result = MM_TRUE;
            goto out;
        }

        if (*it_remaining > 0u && itstate_get(cpu->xpsr) == 0u) {
            *it_pattern = 0;
            *it_remaining = 0;
            *it_cond = 0;
        }
        execute_it = MM_TRUE;
        if (*it_remaining > 0u && d.kind != MM_OP_IT) {
            mm_bool cond_true = MM_FALSE;
            mm_bool take = MM_FALSE;
            mm_bool n = (cpu->xpsr & (1u << 31)) != 0u;
            mm_bool z = (cpu->xpsr & (1u << 30)) != 0u;
            mm_bool c = (cpu->xpsr & (1u << 29)) != 0u;
            mm_bool v = (cpu->xpsr & (1u << 28)) != 0u;
            mm_u8 cond = *it_cond;
            switch (cond) {
            case MM_COND_EQ: cond_true = z; break;
            case MM_COND_NE: cond_true = !z; break;
            case MM_COND_CS: cond_true = c; break;
            case MM_COND_CC: cond_true = !c; break;
            case MM_COND_MI: cond_true = n; break;
            case MM_COND_PL: cond_true = !n; break;
            case MM_COND_VS: cond_true = v; break;
            case MM_COND_VC: cond_true = !v; break;
            case MM_COND_HI: cond_true = c && !z; break;
            case MM_COND_LS: cond_true = !c || z; break;
            case MM_COND_GE: cond_true = (n == v); break;
            case MM_COND_LT: cond_true = (n != v); break;
            case MM_COND_GT: cond_true = !z && (n == v); break;
            case MM_COND_LE: cond_true = z || (n != v); break;
            case MM_COND_AL: cond_true = MM_TRUE; break;
            default: cond_true = MM_FALSE; break;
            }
            take = ((*it_pattern & 0x1u) != 0u) ? cond_true : !cond_true;
            execute_it = take;
        }

        if (!execute_it && d.kind != MM_OP_IT) {
            if (*it_remaining > 0u) {
                mm_u8 raw = itstate_get(cpu->xpsr);
                *it_pattern >>= 1;
                (*it_remaining)--;
                raw = itstate_advance(raw);
                cpu->xpsr = itstate_set(cpu->xpsr, raw);
            }
            result = MM_TRUE;
            goto out;
        }

        if (fault_skip) {
            printf("[FAULT-CLOCK] skip pc=0x%08lx cycle=%llu\n",
                   (unsigned long)(f.pc_fetch | 1u),
                   (unsigned long long)(cycle_total ? *cycle_total : 0u));
            if (*it_remaining > 0u && d.kind != MM_OP_IT) {
                mm_u8 raw = itstate_get(cpu->xpsr);
                *it_pattern >>= 1;
                (*it_remaining)--;
                raw = itstate_advance(raw);
                cpu->xpsr = itstate_set(cpu->xpsr, raw);
            }
            result = MM_TRUE;
            goto out;
        }

        {
            struct mm_execute_ctx exec_ctx;
            calltrace_handle_decoded(cpu, &f, &d);
            exec_ctx.cpu = cpu;
            exec_ctx.map = map;
            exec_ctx.scs = scs;
            exec_ctx.nvic = nvic_for_cpu(cpu);
            exec_ctx.gdb = 0;
            exec_ctx.fetch = &f;
            exec_ctx.dec = &d;
            exec_ctx.opt_dump = MM_FALSE;
            exec_ctx.opt_gdb = MM_FALSE;
            exec_ctx.opt_expect_bkpt = MM_FALSE;
            exec_ctx.expect_bkpt = 0u;
            exec_ctx.it_pattern = it_pattern;
            exec_ctx.it_remaining = it_remaining;
            exec_ctx.it_cond = it_cond;
            exec_ctx.done = done;
            exec_ctx.bkpt_hit = 0;
            exec_ctx.bkpt_imm = 0;
            exec_ctx.handle_pc_write = handle_pc_write;
            exec_ctx.raise_mem_fault = raise_mem_fault;
            exec_ctx.raise_usage_fault = raise_usage_fault;
            exec_ctx.exc_return_unstack = exc_return_unstack;
            exec_ctx.enter_exception = enter_exception;
            (void)mm_execute_decoded(&exec_ctx);
        }

        if (*it_remaining > 0u && d.kind != MM_OP_IT) {
            mm_u8 raw = itstate_get(cpu->xpsr);
            *it_pattern >>= 1;
            (*it_remaining)--;
            raw = itstate_advance(raw);
            cpu->xpsr = itstate_set(cpu->xpsr, raw);
        }
    }
    result = MM_TRUE;
out:
    if (trace_started) {
        mmio_bus_end_step(&map->mmio, mm_trace_get_undo_sink());
        mm_trace_end_step(cpu);
    }
    return result;
}

/* Handle writes to PC; detect EXC_RETURN magic values and perform unstack. */
static mm_bool handle_pc_write(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               struct mm_scs *scs,
                               mm_u32 value,
                               mm_u8 *it_pattern,
                               mm_u8 *it_remaining,
                               mm_u8 *it_cond)
{
    if ((value & 1u) == 0u && (value & 0xffffff00u) != 0xffffff00u) {
        if (getenv("M33MU_UNDEF_TRACE")) {
            printf("[PC_WRITE_FAULT] pc=0x%08lx value=0x%08lx lr=0x%08lx\n",
                   (unsigned long)(cpu->r[15] & ~1u),
                   (unsigned long)value,
                   (unsigned long)cpu->r[14]);
        }
        if (!raise_usage_fault(cpu, map, scs, cpu->r[15] & ~1u, cpu->xpsr, (1u << 16))) {
            return MM_FALSE;
        }
        return MM_TRUE;
    }
    if ((value & 0xffffff00u) == 0xffffff00u) {
        if (svc_stack_trace_enabled() && (cpu->xpsr & 0x1ffu) == MM_VECT_SVCALL) {
            printf("[EXC_RETURN_WRITE] value=0x%08lx pc=0x%08lx lr=0x%08lx mode=%d sec=%d\n",
                   (unsigned long)value,
                   (unsigned long)cpu->r[15],
                   (unsigned long)cpu->r[14],
                   (int)cpu->mode,
                   (int)cpu->sec_state);
            dump_cpu_regs(cpu, "EXC_RETURN_WRITE");
            dump_exc_stack_state(cpu, "EXC_RETURN_WRITE");
        }
        if (!exc_return_unstack(cpu, map, scs, value)) {
            printf("EXC_RETURN unstack failed\n");
            return MM_FALSE;
        }
        itstate_sync_from_xpsr(cpu->xpsr, it_pattern, it_remaining, it_cond);
        return MM_TRUE;
    }
    if (svc_stack_trace_enabled() && (cpu->xpsr & 0x1ffu) == MM_VECT_SVCALL) {
        printf("[PC_WRITE] value=0x%08lx pc=0x%08lx lr=0x%08lx mode=%d sec=%d\n",
               (unsigned long)value,
               (unsigned long)cpu->r[15],
               (unsigned long)cpu->r[14],
               (int)cpu->mode,
               (int)cpu->sec_state);
        dump_cpu_regs(cpu, "PC_WRITE");
        dump_exc_stack_state(cpu, "PC_WRITE");
    }
    cpu->r[15] = value | 1u;
    cpu->xpsr = itstate_set(cpu->xpsr, 0u);
    if (it_pattern) *it_pattern = 0;
    if (it_remaining) *it_remaining = 0;
    if (it_cond) *it_cond = 0;
    return MM_TRUE;
}

static mm_bool raise_hard_fault(struct mm_cpu *cpu, struct mm_memmap *map, struct mm_scs *scs, mm_u32 fault_pc, mm_u32 fault_xpsr)
{
    mm_u32 handler = 0;
    mm_u32 sp;
    mm_u32 exc_ret_val;
    mm_bool use_psp_entry;
    mm_bool fp_stack;
    mm_bool fp_lazy;
    mm_bool addl_state;
    enum mm_mode pre_mode;
    mm_u32 frame[8];
    enum mm_sec_state sec;
    enum mm_sec_state handler_sec;
    enum mm_sec_state stack_sec;
    int i;

    if (cpu == 0 || map == 0 || scs == 0) {
        return MM_FALSE;
    }

    sec = cpu->sec_state;
    handler_sec = sec;
    if (sec == MM_SECURE && (scs->aircr_s & AIRCR_BFHFNMINS) != 0u) {
        handler_sec = MM_NONSECURE;
    }
    stack_sec = sec;
    scs->hfsr |= (1u << 30); /* FORCED */
    (void)mm_exception_read_handler(map, scs, handler_sec, MM_VECT_HARDFAULT, &handler);

    {
        mm_u32 cfsr_dbg = 0;
        (void)mm_memmap_read(map, sec, 0xE000ED28u, 4u, &cfsr_dbg);
        printf("[HARDFLT] CFSR=0x%08lx fault_pc=0x%08lx handler=0x%08lx\n",
               (unsigned long)cfsr_dbg,
               (unsigned long)fault_pc,
               (unsigned long)handler);
        {
        mm_u32 mmfar_dbg = 0;
        if (mm_memmap_read(map, sec, 0xE000ED34u, 4u, &mmfar_dbg)) {
            printf("[HARDFLT] MMFAR=0x%08lx\n", (unsigned long)mmfar_dbg);
        }
        }
        /* Best-effort debug; do not halt even if fetches fail. */
        {
            mm_u32 hw = 0;
            if (mm_memmap_read(map, sec, fault_pc & ~1u, 2u, &hw)) {
                printf("[HARDFLT] mem16[0x%08lx]=0x%04lx\n",
                       (unsigned long)(fault_pc & ~1u),
                       (unsigned long)(hw & 0xffffu));
            }
            (void)mm_memmap_read(map, sec, handler & ~1u, 2u, &hw);
        }
    }
    if (g_quit_on_faults) {
        g_fault_pending = MM_TRUE;
    }

    frame[0] = cpu->r[0];
    frame[1] = cpu->r[1];
    frame[2] = cpu->r[2];
    frame[3] = cpu->r[3];
    frame[4] = cpu->r[12];
    frame[5] = cpu->r[14];
    frame[6] = fault_pc & ~1u;
    frame[7] = fault_xpsr | 0x01000000u; /* Preserve full xPSR/IT/flags/IPSR; ensure T */

    pre_mode = cpu->mode;
    use_psp_entry = (pre_mode == MM_THREAD) && (((stack_sec == MM_NONSECURE) ? cpu->control_ns : cpu->control_s) & 0x2u);
    fp_stack = fpu_auto_preserve_enabled(cpu, scs);
    fp_lazy = fp_stack && fpu_lazy_preserve_enabled(cpu, scs);
    addl_state = cross_domain_additional_state_required(sec, handler_sec);
    exc_ret_val = exc_return_encode(stack_sec,
                                    handler_sec,
                                    use_psp_entry,
                                    pre_mode == MM_THREAD,
                                    fp_stack ? MM_FALSE : MM_TRUE,
                                    addl_state ? MM_FALSE : MM_TRUE);
    {
        mm_u32 ctrl = (stack_sec == MM_NONSECURE) ? cpu->control_ns : cpu->control_s;
        mm_u32 cpacr = cpacr_for_sec(scs, stack_sec);
        mm_bool fpu_allowed = fpu_access_allowed(cpu, scs);
        printf("[MEMFAULT_FPU] sec=%d fp_active=%d fpu_allowed=%d fp_stack=%d\n",
               (int)stack_sec,
               (int)cpu->fp_active,
               (int)fpu_allowed,
               (int)fp_stack);
        printf("[MEMFAULT_FPU] ctrl=0x%08lx cpacr=0x%08lx nsacr=0x%08lx\n",
               (unsigned long)ctrl,
               (unsigned long)cpacr,
               (unsigned long)scs->nsacr);
        printf("[MEMFAULT_FPU] exc_ret=0x%08lx\n",
               (unsigned long)exc_ret_val);
    }

    sp = use_psp_entry ? ((stack_sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s)
                       : ((stack_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s);
    if ((scs->ccr & CCR_STKALIGN) != 0u && (sp & 7u) != 0u) {
        sp -= 4u; /* Align stack to 8-byte boundary. */
        frame[7] |= (1u << 9); /* Stack alignment padding flag. */
    } else {
        frame[7] &= ~(1u << 9);
    }
    {
        mm_u32 sp_total = sp - 32u - (fp_stack ? FP_STACK_BYTES : 0u) - (addl_state ? EXC_ADDITIONAL_STATE_BYTES : 0u);
        mm_u32 sp_frame = sp_total;
        mm_u32 sp_fp = sp_total + 32u;
        mm_u32 sp_addl = sp_fp + (fp_stack ? FP_STACK_BYTES : 0u);
        for (i = 0; i < 8; ++i) {
            if (!mm_memmap_write(map, stack_sec, sp_frame + (mm_u32)(i * 4u), 4u, frame[i])) {
                printf("HardFault: stacking failed at 0x%08lx\n", (unsigned long)(sp_frame + (mm_u32)(i * 4u)));
                record_bus_fault(scs, sec, sp_frame + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                return MM_FALSE;
            }
        }
        if (fp_stack) {
            if (!fp_lazy) {
                for (i = 0; i < 16; ++i) {
                    if (!mm_memmap_write(map, stack_sec, sp_fp + (mm_u32)(i * 4u), 4u, cpu->s[i])) {
                        printf("HardFault: FP stacking failed at 0x%08lx\n",
                               (unsigned long)(sp_fp + (mm_u32)(i * 4u)));
                        record_bus_fault(scs, sec, sp_fp + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                        return MM_FALSE;
                    }
                }
                if (!mm_memmap_write(map, stack_sec, sp_fp + (16u * 4u), 4u, cpu->fpscr)) {
                    record_bus_fault(scs, sec, sp_fp + (16u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                    return MM_FALSE;
                }
                if (!mm_memmap_write(map, stack_sec, sp_fp + (17u * 4u), 4u, 0u)) {
                    record_bus_fault(scs, sec, sp_fp + (17u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                    return MM_FALSE;
                }
            } else {
                scs->fpccr |= FPCCR_LSPACT;
            }
            scs->fpcar = sp_fp;
        }
        if (addl_state) {
            for (i = 0; i < 8; ++i) {
                if (!mm_memmap_write(map, stack_sec, sp_addl + (mm_u32)(i * 4u), 4u, cpu->r[4 + i])) {
                    record_bus_fault(scs, sec, sp_addl + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                    return MM_FALSE;
                }
            }
            if (!mm_memmap_write(map, stack_sec, sp_addl + (8u * 4u), 4u, 0u)) {
                record_bus_fault(scs, sec, sp_addl + (8u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                return MM_FALSE;
            }
        }
        sp = sp_total;
    }
    if (use_psp_entry) {
        if (stack_sec == MM_NONSECURE) cpu->psp_ns = sp; else cpu->psp_s = sp;
    } else {
        if (stack_sec == MM_NONSECURE) cpu->msp_ns = sp; else cpu->msp_s = sp;
    }
    if (cpu->exc_depth < MM_EXC_STACK_MAX) {
        cpu->exc_sp[cpu->exc_depth] = sp;
        cpu->exc_use_psp[cpu->exc_depth] = use_psp_entry;
        cpu->exc_sec[cpu->exc_depth] = stack_sec;
        cpu->exc_num[cpu->exc_depth] = (mm_u16)MM_VECT_HARDFAULT;
        cpu->exc_fp_reserved[cpu->exc_depth] = fp_stack;
        cpu->exc_fp_saved[cpu->exc_depth] = fp_stack;
        cpu->exc_cross_domain[cpu->exc_depth] = MM_FALSE;
        if (addl_state) {
            save_and_clear_secure_callee_regs(cpu, cpu->exc_depth);
        }
        cpu->exc_depth++;
    }
    if (stack_trace_enabled()) {
        printf("[EXC_STACK_PUSH] exc=%lu pushed_sp=0x%08lx use_psp=%u sec=%u new_depth=%u\n",
               (unsigned long)MM_VECT_HARDFAULT,
               (unsigned long)sp,
               (unsigned)use_psp_entry,
               (unsigned)sec,
               (unsigned)cpu->exc_depth);
        dump_exc_stack_state(cpu, "EXC_STACK_PUSH");
    }
    /* Handler mode always uses MSP (ARM ARM DDI0553). Set R13 accordingly. */
    cpu->r[13] = (handler_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    cpu->xpsr = (fault_xpsr & 0xF8000000u) | 0x01000003u;
    cpu->r[14] = exc_ret_val;
    cpu->mode = MM_HANDLER;
    if (handler_sec == MM_NONSECURE) {
        cpu->control_ns &= ~0x2u;
    } else {
        cpu->control_s &= ~0x2u;
    }
    cpu->sec_state = handler_sec;
    scs_set_vectactive(scs, handler_sec, MM_VECT_HARDFAULT);
    cpu->r[15] = handler | 1u;
    return MM_TRUE;
}

static void record_bus_fault(struct mm_scs *scs, enum mm_sec_state sec, mm_u32 addr, mm_u32 bfsr_bits)
{
    mm_u32 *cfsr;
    mm_u32 *bfar;

    if (scs == 0) {
        return;
    }
    cfsr = mm_scs_cfsr_ptr(scs, sec);
    bfar = mm_scs_bfar_ptr(scs, sec);
    *cfsr |= bfsr_bits;
    if ((bfsr_bits & BFSR_BFARVALID) != 0u) {
        *bfar = addr;
    }
}

/* Generic exception entry for synchronous SVC and asynchronous PendSV/SysTick/faults. */
static mm_bool enter_exception(struct mm_cpu *cpu, struct mm_memmap *map, struct mm_scs *scs, mm_u32 exc_num, mm_u32 return_pc, mm_u32 xpsr_in);
static mm_bool enter_exception_ex(struct mm_cpu *cpu,
                                  struct mm_memmap *map,
                                  struct mm_scs *scs,
                                  mm_u32 exc_num,
                                  mm_u32 return_pc,
                                  mm_u32 xpsr_in,
                                  enum mm_sec_state handler_sec);

static mm_bool raise_mem_fault(struct mm_cpu *cpu, struct mm_memmap *map, struct mm_scs *scs, mm_u32 fault_pc, mm_u32 fault_xpsr, mm_u32 addr, mm_bool is_exec)
{
    enum mm_sec_state sec;
    mm_u32 bits = is_exec ? 0x1u : 0x2u; /* IACCVIOL / DACCVIOL */
    mm_u32 *cfsr;
    mm_u32 *mmfar;
    sec = cpu->sec_state;
    printf("[MEMFAULT] pc=0x%08lx addr=0x%08lx\n",
           (unsigned long)fault_pc,
           (unsigned long)addr);
    printf("[MEMFAULT] sp=0x%08lx lr=0x%08lx xpsr=0x%08lx\n",
           (unsigned long)mm_cpu_get_active_sp(cpu),
           (unsigned long)cpu->r[14],
           (unsigned long)cpu->xpsr);
    printf("[MEMFAULT] r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
           (unsigned long)cpu->r[0],
           (unsigned long)cpu->r[1],
           (unsigned long)cpu->r[2],
           (unsigned long)cpu->r[3]);
    printf("[MEMFAULT] r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
           (unsigned long)cpu->r[4],
           (unsigned long)cpu->r[5],
           (unsigned long)cpu->r[6],
           (unsigned long)cpu->r[7]);
    printf("[MEMFAULT] r12=%08lx\n",
           (unsigned long)cpu->r[12]);
    if (g_quit_on_faults) {
        g_fault_pending = MM_TRUE;
    }

    /* TrustZone: if a SecureFault is pending (e.g. NS access to Secure attribution),
     * route to SecureFault in Secure state. */
    if (scs->securefault_pending) {
        scs->securefault_pending = MM_FALSE;
        return enter_exception_ex(cpu, map, scs, MM_VECT_SECUREFAULT, fault_pc, fault_xpsr, MM_SECURE);
    }

    bits |= (1u << 7); /* MMARVALID */
    cfsr = mm_scs_cfsr_ptr(scs, sec);
    mmfar = mm_scs_mmfar_ptr(scs, sec);
    *cfsr |= bits;
    *mmfar = addr;
    if ((*cfsr & 0x3u) == 0x3u) {
        /* Dump SAU state when both IACCVIOL and DACCVIOL are set. */
#define SAU_CTRL_ENABLE 0x1u
#define SAU_CTRL_ALLNS  0x2u
#define SAU_RLAR_ENABLE 0x1u
#define SAU_RLAR_NSC    0x2u
        int i;
        printf("[SAU] CTRL=0x%08lx TYPE=0x%08lx SFSR=0x%08lx SFAR=0x%08lx\n",
               (unsigned long)scs->sau_ctrl,
               (unsigned long)scs->sau_type,
               (unsigned long)scs->sau_sfsr,
               (unsigned long)scs->sau_sfar);
        printf("[SAU] CTRL.EN=%lu CTRL.ALLNS=%lu\n",
               (unsigned long)((scs->sau_ctrl & SAU_CTRL_ENABLE) != 0u),
               (unsigned long)((scs->sau_ctrl & SAU_CTRL_ALLNS) != 0u));
        for (i = 0; i < 8; ++i) {
            mm_u32 rbar = scs->sau_rbar[i];
            mm_u32 rlar = scs->sau_rlar[i];
            if ((rlar & SAU_RLAR_ENABLE) == 0u) {
                continue;
            }
            printf("[SAU] R%u RBAR=0x%08lx RLAR=0x%08lx BASE=0x%08lx LIMIT=0x%08lx NSC=%lu\n",
                   (unsigned)i,
                   (unsigned long)rbar,
                   (unsigned long)rlar,
                   (unsigned long)(rbar & 0xFFFFFFE0u),
                   (unsigned long)((rlar & 0xFFFFFFE0u) | 0x1Fu),
                   (unsigned long)((rlar & SAU_RLAR_NSC) != 0u));
        }
#undef SAU_CTRL_ENABLE
#undef SAU_CTRL_ALLNS
#undef SAU_RLAR_ENABLE
#undef SAU_RLAR_NSC
    }
    /* Deliver MemManage if enabled, otherwise escalate to HardFault. */
    if (sec == MM_NONSECURE) {
        if ((scs->shcsr_ns & (1u << 16)) != 0u) {
            shcsr_set_exception_active(scs, MM_NONSECURE, MM_VECT_MEMMANAGE);
            return enter_exception(cpu, map, scs, MM_VECT_MEMMANAGE, fault_pc, fault_xpsr);
        }
    } else {
        if ((scs->shcsr_s & (1u << 16)) != 0u) {
            shcsr_set_exception_active(scs, MM_SECURE, MM_VECT_MEMMANAGE);
            return enter_exception(cpu, map, scs, MM_VECT_MEMMANAGE, fault_pc, fault_xpsr);
        }
    }
    return raise_hard_fault(cpu, map, scs, fault_pc, fault_xpsr);
}

static mm_bool raise_usage_fault(struct mm_cpu *cpu, struct mm_memmap *map, struct mm_scs *scs, mm_u32 fault_pc, mm_u32 fault_xpsr, mm_u32 ufsr_bits)
{
    mm_u32 handler = 0;
    mm_u32 sp;
    mm_u32 msp_s_val;
    mm_u32 msp_ns_val;
    mm_u32 psp_s_val;
    mm_u32 psp_ns_val;
    mm_u32 control_s_val;
    mm_u32 control_ns_val;
    mm_u32 frame[8];
    mm_bool use_psp_entry;
    mm_u32 exc_ret_val;
    mm_bool fp_stack;
    mm_bool fp_lazy;
    int i;
    enum mm_sec_state sec;
    mm_u32 *cfsr;

    if (cpu == 0 || map == 0 || scs == 0) {
        return MM_FALSE;
    }

    if (ufsr_bits == 0u) {
        ufsr_bits = UFSR_UNDEFINSTR;
    }
    if ((ufsr_bits & UFSR_UNDEFINSTR) != 0u && getenv("M33MU_UNDEF_TRACE")) {
        printf("[UNDEF_RAISE] fault_pc=0x%08lx xpsr=0x%08lx\n",
               (unsigned long)fault_pc,
               (unsigned long)fault_xpsr);
    }
    sec = cpu->sec_state;
    cfsr = mm_scs_cfsr_ptr(scs, sec);
    *cfsr |= ufsr_bits;
    if (sec == MM_NONSECURE) {
        if ((scs->shcsr_ns & (1u << 18)) == 0u) {
            return raise_hard_fault(cpu, map, scs, fault_pc, fault_xpsr);
        }
    } else {
        if ((scs->shcsr_s & (1u << 18)) == 0u) {
            return raise_hard_fault(cpu, map, scs, fault_pc, fault_xpsr);
        }
    }
    if (sec == MM_NONSECURE) {
        shcsr_set_exception_active(scs, MM_NONSECURE, MM_VECT_USAGEFAULT);
    } else {
        shcsr_set_exception_active(scs, MM_SECURE, MM_VECT_USAGEFAULT);
    }
    (void)mm_exception_read_handler(map, scs, sec, MM_VECT_USAGEFAULT, &handler);

    frame[0] = cpu->r[0];
    frame[1] = cpu->r[1];
    frame[2] = cpu->r[2];
    frame[3] = cpu->r[3];
    frame[4] = cpu->r[12];
    frame[5] = cpu->r[14];
    frame[6] = fault_pc & ~1u;
    frame[7] = fault_xpsr | (1u << 24); /* ensure Thumb; keep IPSR from preempted ctx */

    /* Select stack based on pre-fault thread CONTROL.SPSEL (Handler always MSP). */
    use_psp_entry = (cpu->mode == MM_THREAD) && ((sec == MM_NONSECURE ? cpu->control_ns : cpu->control_s) & 0x2u);
    sp = use_psp_entry ? ((sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s)
                       : ((sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s);
    if ((scs->ccr & CCR_STKALIGN) != 0u && (sp & 7u) != 0u) {
        sp -= 4u; /* Align stack to 8-byte boundary. */
        frame[7] |= (1u << 9); /* Stack alignment padding flag. */
    } else {
        frame[7] &= ~(1u << 9);
    }
    msp_s_val = cpu->msp_s;
    msp_ns_val = cpu->msp_ns;
    psp_s_val = cpu->psp_s;
    psp_ns_val = cpu->psp_ns;
    control_s_val = cpu->control_s;
    control_ns_val = cpu->control_ns;
    (void)msp_s_val;
    (void)msp_ns_val;
    (void)psp_s_val;
    (void)psp_ns_val;
    (void)control_s_val;
    (void)control_ns_val;
    fp_stack = fpu_auto_preserve_enabled(cpu, scs);
    fp_lazy = fp_stack && fpu_lazy_preserve_enabled(cpu, scs);
    exc_ret_val = exc_return_encode(sec,
                                    sec,
                                    use_psp_entry,
                                    cpu->mode == MM_THREAD,
                                    fp_stack ? MM_FALSE : MM_TRUE,
                                    MM_TRUE);
    printf("[USGFLT] enter sec=%d mode=%d use_psp=%d active_sp=0x%08lx\n",
           (int)sec,
           (int)cpu->mode,
           (int)use_psp_entry,
           (unsigned long)sp);
    printf("[USGFLT] msp_s=0x%08lx msp_ns=0x%08lx\n",
           (unsigned long)msp_s_val,
           (unsigned long)msp_ns_val);
    printf("[USGFLT] psp_s=0x%08lx psp_ns=0x%08lx\n",
           (unsigned long)psp_s_val,
           (unsigned long)psp_ns_val);
    printf("[USGFLT] ctrl_s=0x%08lx ctrl_ns=0x%08lx\n",
           (unsigned long)control_s_val,
           (unsigned long)control_ns_val);
    printf("[USGFLT] fault_pc=0x%08lx xpsr=0x%08lx\n",
           (unsigned long)fault_pc,
           (unsigned long)fault_xpsr);
    printf("[USGFLT] handler=0x%08lx exc_ret=0x%08lx\n",
           (unsigned long)handler,
           (unsigned long)exc_ret_val);
    printf("[USGFLT] msplim_s=0x%08lx msplim_ns=0x%08lx\n",
           (unsigned long)cpu->msplim_s,
           (unsigned long)cpu->msplim_ns);
    printf("[USGFLT] psplim_s=0x%08lx psplim_ns=0x%08lx\n",
           (unsigned long)cpu->psplim_s,
           (unsigned long)cpu->psplim_ns);
        {
            mm_u32 cfsr_dbg = 0;
            (void)mm_memmap_read(map, sec, 0xE000ED28u, 4u, &cfsr_dbg); /* SCB->CFSR */
            printf("[USGFLT] CFSR=0x%08lx\n", (unsigned long)cfsr_dbg);
            /* Dump the halfword at the reported fault PC for debugging decode vs fetch. */
            {
                mm_u32 hw = 0;
                if (mm_memmap_read(map, sec, fault_pc & ~1u, 2u, &hw)) {
                    printf("[USGFLT] mem16[0x%08lx]=0x%04lx\n",
                           (unsigned long)(fault_pc & ~1u),
                           (unsigned long)(hw & 0xffffu));
                    {
                        mm_u32 w = 0;
                        if (mm_memmap_read(map, sec, fault_pc & ~3u, 4u, &w)) {
                            printf("[USGFLT] mem32[0x%08lx]=0x%08lx\n",
                                   (unsigned long)(fault_pc & ~3u),
                                   (unsigned long)w);
                        } else {
                            printf("[USGFLT] mem32[0x%08lx] faulted\n",
                                   (unsigned long)(fault_pc & ~3u));
                        }
                    }
                } else {
                    printf("[USGFLT] mem16[0x%08lx] faulted\n", (unsigned long)(fault_pc & ~1u));
                }
            }
        }
    if (g_quit_on_faults) {
        return MM_FALSE;
    }
    {
        mm_u32 sp_total = sp - 32u - (fp_stack ? FP_STACK_BYTES : 0u);
        mm_u32 sp_frame = sp_total;
        mm_u32 sp_fp = sp_total + 32u;
        for (i = 0; i < 8; ++i) {
            mm_bool ok = mm_memmap_write(map, sec, sp_frame + (mm_u32)(i * 4u), 4u, frame[i]);
            if (!ok) {
                /* Stack write failed: escalate to HardFault (per ARMv8-M, stacking fault escalates). */
                printf("HardFault: stacking failed at 0x%08lx\n",
                       (unsigned long)(sp_frame + (mm_u32)(i * 4u)));
                return MM_FALSE;
            }
        }
        if (fp_stack) {
            if (!fp_lazy) {
                for (i = 0; i < 16; ++i) {
                    if (!mm_memmap_write(map, sec, sp_fp + (mm_u32)(i * 4u), 4u, cpu->s[i])) {
                        printf("HardFault: FP stacking failed at 0x%08lx\n",
                               (unsigned long)(sp_fp + (mm_u32)(i * 4u)));
                        return MM_FALSE;
                    }
                }
                if (!mm_memmap_write(map, sec, sp_fp + (16u * 4u), 4u, cpu->fpscr)) {
                    return MM_FALSE;
                }
                if (!mm_memmap_write(map, sec, sp_fp + (17u * 4u), 4u, 0u)) {
                    return MM_FALSE;
                }
            } else {
                scs->fpccr |= FPCCR_LSPACT;
            }
            scs->fpcar = sp_fp;
            sp = sp_total;
        } else {
            sp = sp_total;
        }
    }
    if (use_psp_entry) {
        if (sec == MM_NONSECURE) cpu->psp_ns = sp; else cpu->psp_s = sp;
    } else {
        if (sec == MM_NONSECURE) cpu->msp_ns = sp; else cpu->msp_s = sp;
    }
    if (cpu->exc_depth < MM_EXC_STACK_MAX) {
        cpu->exc_sp[cpu->exc_depth] = sp;
        cpu->exc_use_psp[cpu->exc_depth] = use_psp_entry;
        cpu->exc_sec[cpu->exc_depth] = sec;
        cpu->exc_num[cpu->exc_depth] = (mm_u16)MM_VECT_USAGEFAULT;
        cpu->exc_fp_reserved[cpu->exc_depth] = fp_stack;
        cpu->exc_fp_saved[cpu->exc_depth] = fp_stack && !fp_lazy;
        cpu->exc_cross_domain[cpu->exc_depth] = MM_FALSE;
        cpu->exc_depth++;
    }
    /* Handler mode uses MSP; reflect that in R13. */
    cpu->r[13] = (sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    /* On exception entry, IPSR should carry the exception number (UsageFault=6)
     * and the T-bit must be set. Keep condition flags from the faulting context.
     */
    cpu->xpsr = (fault_xpsr & 0xF8000000u) | 0x01000006u;
    cpu->r[14] = exc_ret_val;
    cpu->mode = MM_HANDLER;
    if (sec == MM_NONSECURE) {
        cpu->control_ns &= ~0x2u;
    } else {
        cpu->control_s &= ~0x2u;
    }
    scs_set_vectactive(scs, sec, MM_VECT_USAGEFAULT);
    cpu->r[15] = handler | 1u;
    return MM_TRUE;
}

/* Generic exception entry for synchronous SVC and asynchronous PendSV/SysTick. */
static mm_bool enter_exception(struct mm_cpu *cpu, struct mm_memmap *map, struct mm_scs *scs, mm_u32 exc_num, mm_u32 return_pc, mm_u32 xpsr_in)
{
    enum mm_sec_state handler_sec;
    if (cpu == 0) {
        return MM_FALSE;
    }
    handler_sec = cpu->sec_state;
    return enter_exception_ex(cpu, map, scs, exc_num, return_pc, xpsr_in, handler_sec);
}

static mm_bool enter_exception_ex(struct mm_cpu *cpu,
                                  struct mm_memmap *map,
                                  struct mm_scs *scs,
                                  mm_u32 exc_num,
                                  mm_u32 return_pc,
                                  mm_u32 xpsr_in,
                                  enum mm_sec_state handler_sec)
{
    mm_u32 handler = 0;
    mm_u32 vtor;
    mm_u32 sp;
    mm_u32 frame[8];
    enum mm_sec_state sec;
    mm_bool use_psp_entry;
    mm_bool fp_stack;
    mm_bool fp_lazy;
    mm_bool addl_state;
    enum mm_mode pre_mode;
    mm_u32 exc_ret_val;
    mm_bool tail_chain = MM_FALSE;
    int i;

    if (cpu == 0 || map == 0 || scs == 0) {
        return MM_FALSE;
    }

    if (exc_num >= 16u) {
        calltrace_log_interrupt(return_pc, exc_num - 16u);
    }

    sec = cpu->sec_state;
    pre_mode = cpu->mode;
    tail_chain = MM_FALSE;

    if (exc_num >= 16u) {
        vtor = (handler_sec == MM_NONSECURE) ? scs->vtor_ns : scs->vtor_s;
        (void)mm_vector_read(map, handler_sec, vtor, exc_num, &handler);
        if (handler == 0u) {
            mm_u32 fallback_vtor = (handler_sec == MM_NONSECURE) ? cpu->vtor_ns : cpu->vtor_s;
            if (fallback_vtor != vtor) {
                (void)mm_vector_read(map, handler_sec, fallback_vtor, exc_num, &handler);
            }
        }
    } else {
        (void)mm_exception_read_handler(map, scs, handler_sec, (enum mm_vector_index)exc_num, &handler);
    }

    switch (exc_num) {
    case MM_VECT_SVCALL:
        shcsr_set_exception_active(scs, sec, MM_VECT_SVCALL);
        break;
    case MM_VECT_SECUREFAULT:
        shcsr_set_exception_active(scs, MM_SECURE, MM_VECT_SECUREFAULT);
        break;
    case MM_VECT_PENDSV:
        scs->pend_sv = MM_FALSE;
        shcsr_set_exception_active(scs, handler_sec, MM_VECT_PENDSV);
        break;
    case MM_VECT_SYSTICK:
        if (handler_sec == MM_NONSECURE) scs->pend_st_ns = MM_FALSE;
        else scs->pend_st_s = MM_FALSE;
        shcsr_set_exception_active(scs, handler_sec, MM_VECT_SYSTICK);
        break;
    default:
        break;
    }

    frame[0] = cpu->r[0];
    frame[1] = cpu->r[1];
    frame[2] = cpu->r[2];
    frame[3] = cpu->r[3];
    frame[4] = cpu->r[12];
    frame[5] = cpu->r[14];
    frame[6] = return_pc & ~1u;
    frame[7] = xpsr_in | 0x01000000u; /* preserve full xPSR/IT/flags/IPSR; ensure T */

    fp_stack = (!tail_chain) && fpu_auto_preserve_enabled(cpu, scs);
    fp_lazy = fp_stack && fpu_lazy_preserve_enabled(cpu, scs);
    addl_state = cross_domain_additional_state_required(sec, handler_sec);
    if (pre_mode == MM_HANDLER) {
        use_psp_entry = MM_FALSE;
        exc_ret_val = exc_return_encode(sec, handler_sec, MM_FALSE, MM_FALSE, fp_stack ? MM_FALSE : MM_TRUE, addl_state ? MM_FALSE : MM_TRUE);
    } else {
        use_psp_entry = (((sec == MM_NONSECURE) ? cpu->control_ns : cpu->control_s) & 0x2u) != 0u;
        exc_ret_val = tail_chain ? cpu->r[14] :
            exc_return_encode(sec,
                              handler_sec,
                              use_psp_entry,
                              MM_TRUE,
                              fp_stack ? MM_FALSE : MM_TRUE,
                              addl_state ? MM_FALSE : MM_TRUE);
    }

    sp = use_psp_entry ? ((sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s)
                       : ((sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s);
    if (!tail_chain) {
        if ((scs->ccr & CCR_STKALIGN) != 0u && (sp & 7u) != 0u) {
            sp -= 4u; /* Align stack to 8-byte boundary. */
            frame[7] |= (1u << 9); /* Stack alignment padding flag. */
        } else {
            frame[7] &= ~(1u << 9);
        }
    }
    if (stack_trace_enabled()) {
        printf("[EXC_ENTER] exc=%lu pre_mode=%d sec=%d handler_sec=%d use_psp=%d sp=0x%08lx ret_pc=0x%08lx xpsr=0x%08lx handler=0x%08lx msp_s=0x%08lx msp_ns=0x%08lx psp_s=0x%08lx psp_ns=0x%08lx ctrl_s=0x%08lx ctrl_ns=0x%08lx\n",
               (unsigned long)exc_num,
               (int)pre_mode,
               (int)sec,
               (int)handler_sec,
               (int)use_psp_entry,
               (unsigned long)sp,
               (unsigned long)return_pc,
               (unsigned long)xpsr_in,
               (unsigned long)handler,
               (unsigned long)cpu->msp_s,
               (unsigned long)cpu->msp_ns,
               (unsigned long)cpu->psp_s,
               (unsigned long)cpu->psp_ns,
               (unsigned long)cpu->control_s,
               (unsigned long)cpu->control_ns);
    }
    if (svc_stack_trace_enabled() &&
        exc_num == MM_VECT_SVCALL &&
        pre_mode == MM_THREAD) {
        dump_cpu_regs(cpu, "SVC_ENTER_PRE_STACK");
        dump_exc_stack_state(cpu, "SVC_ENTER_PRE_STACK");
    }
    if (stack_trace_enabled()) {
        dump_cpu_regs(cpu, "EXC_ENTER_PRE_STACK");
        dump_exc_stack_state(cpu, "EXC_ENTER_PRE_STACK");
    }
    if (svc_stack_trace_enabled() &&
        exc_num == MM_VECT_SVCALL &&
        pre_mode == MM_THREAD &&
        use_psp_entry) {
        printf("[SVC_STACK_ENTER] svc_pc=0x%08lx ret_pc=0x%08lx sp=0x%08lx r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx r12=0x%08lx lr=0x%08lx pc=0x%08lx xpsr=0x%08lx psp_ns=0x%08lx ctrl_ns=0x%08lx handler_lr=0x%08lx ipsr=%lu msp_ns=0x%08lx\n",
               (unsigned long)cpu->r[15],
               (unsigned long)return_pc,
               (unsigned long)sp,
               (unsigned long)frame[0],
               (unsigned long)frame[1],
               (unsigned long)frame[2],
               (unsigned long)frame[3],
               (unsigned long)frame[4],
               (unsigned long)frame[5],
               (unsigned long)frame[6],
               (unsigned long)frame[7],
               (unsigned long)cpu->psp_ns,
               (unsigned long)cpu->control_ns,
               (unsigned long)exc_ret_val,
               (unsigned long)exc_num,
               (unsigned long)cpu->msp_ns);
    }
    if (!tail_chain) {
        {
            mm_u32 sp_total = sp - 32u - (fp_stack ? FP_STACK_BYTES : 0u) - (addl_state ? EXC_ADDITIONAL_STATE_BYTES : 0u);
            mm_u32 sp_frame = sp_total;
            mm_u32 sp_fp = sp_total + 32u;
            mm_u32 sp_addl = sp_fp + (fp_stack ? FP_STACK_BYTES : 0u);
            mm_u32 splim = use_psp_entry ? ((sec == MM_NONSECURE) ? cpu->psplim_ns : cpu->psplim_s)
                                         : ((sec == MM_NONSECURE) ? cpu->msplim_ns : cpu->msplim_s);
            if (splim != 0u && sp_total < splim) {
                return raise_usage_fault(cpu, map, scs, return_pc, xpsr_in, UFSR_STKOF);
            }
            for (i = 0; i < 8; ++i) {
                if (!mm_memmap_write(map, sec, sp_frame + (mm_u32)(i * 4u), 4u, frame[i])) {
                    printf("HardFault: stacking failed at 0x%08lx\n",
                           (unsigned long)(sp_frame + (mm_u32)(i * 4u)));
                    record_bus_fault(scs, sec, sp_frame + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                    return raise_hard_fault(cpu, map, scs, return_pc, xpsr_in);
                }
            }
            if (fp_stack) {
                if (!fp_lazy) {
                    for (i = 0; i < 16; ++i) {
                        if (!mm_memmap_write(map, sec, sp_fp + (mm_u32)(i * 4u), 4u, cpu->s[i])) {
                            printf("HardFault: FP stacking failed at 0x%08lx\n",
                                   (unsigned long)(sp_fp + (mm_u32)(i * 4u)));
                            record_bus_fault(scs, sec, sp_fp + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                            return raise_hard_fault(cpu, map, scs, return_pc, xpsr_in);
                        }
                    }
                    if (!mm_memmap_write(map, sec, sp_fp + (16u * 4u), 4u, cpu->fpscr)) {
                        record_bus_fault(scs, sec, sp_fp + (16u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                        return raise_hard_fault(cpu, map, scs, return_pc, xpsr_in);
                    }
                    if (!mm_memmap_write(map, sec, sp_fp + (17u * 4u), 4u, 0u)) {
                        record_bus_fault(scs, sec, sp_fp + (17u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                        return raise_hard_fault(cpu, map, scs, return_pc, xpsr_in);
                    }
                } else {
                    scs->fpccr |= FPCCR_LSPACT;
                }
                scs->fpcar = sp_fp;
            }
            if (addl_state) {
                for (i = 0; i < 8; ++i) {
                    if (!mm_memmap_write(map, sec, sp_addl + (mm_u32)(i * 4u), 4u, cpu->r[4 + i])) {
                        record_bus_fault(scs, sec, sp_addl + (mm_u32)(i * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                        return raise_hard_fault(cpu, map, scs, return_pc, xpsr_in);
                    }
                }
                if (!mm_memmap_write(map, sec, sp_addl + (8u * 4u), 4u, 0u)) {
                    record_bus_fault(scs, sec, sp_addl + (8u * 4u), BFSR_STKERR | BFSR_PRECISERR | BFSR_BFARVALID);
                    return raise_hard_fault(cpu, map, scs, return_pc, xpsr_in);
                }
            }
            sp = sp_total;
        }
        if (use_psp_entry) {
            if (sec == MM_NONSECURE) cpu->psp_ns = sp; else cpu->psp_s = sp;
        } else {
            if (sec == MM_NONSECURE) cpu->msp_ns = sp; else cpu->msp_s = sp;
        }
    if (cpu->exc_depth < MM_EXC_STACK_MAX) {
        cpu->exc_sp[cpu->exc_depth] = sp;
        cpu->exc_use_psp[cpu->exc_depth] = use_psp_entry;
        cpu->exc_sec[cpu->exc_depth] = sec;
        cpu->exc_num[cpu->exc_depth] = (mm_u16)exc_num;
        cpu->exc_fp_reserved[cpu->exc_depth] = fp_stack;
        cpu->exc_fp_saved[cpu->exc_depth] = fp_stack && !fp_lazy;
        cpu->exc_cross_domain[cpu->exc_depth] = MM_FALSE;
        if (addl_state) {
            save_and_clear_secure_callee_regs(cpu, cpu->exc_depth);
        }
        cpu->exc_depth++;
    }
    }
    /* Exception handlers always use the handler security state's MSP. */
    cpu->r[13] = (handler_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    if (svc_stack_trace_enabled() &&
        exc_num == MM_VECT_SVCALL &&
        pre_mode == MM_THREAD) {
        dump_cpu_regs(cpu, "SVC_ENTER_POST_STACK");
        dump_exc_stack_state(cpu, "SVC_ENTER_POST_STACK");
    }
    if (stack_trace_enabled()) {
        dump_cpu_regs(cpu, "EXC_ENTER_POST_STACK");
        dump_exc_stack_state(cpu, "EXC_ENTER_POST_STACK");
    }
    cpu->xpsr = (xpsr_in & 0xF8000000u) | 0x01000000u | (exc_num & 0x1FFu);
    cpu->r[14] = exc_ret_val;
    cpu->mode = MM_HANDLER;
    /* On exception entry, CONTROL.SPSEL becomes 0 for the handler security state. */
    if (handler_sec == MM_NONSECURE) {
        cpu->control_ns &= ~0x2u;
    } else {
        cpu->control_s &= ~0x2u;
    }
    if (stack_trace_enabled()) {
        printf("[EXC_ENTER_SPSEL] sec=%d control_s=0x%08lx control_ns=0x%08lx\n",
               (int)handler_sec,
               (unsigned long)cpu->control_s,
               (unsigned long)cpu->control_ns);
    }
    if (stack_trace_enabled() && cpu->sec_state != handler_sec) {
        printf("[SEC_STATE] enter exc=%lu sec=%d->%d mode=%d\n",
               (unsigned long)exc_num,
               (int)cpu->sec_state,
               (int)handler_sec,
               (int)cpu->mode);
    }
    cpu->sec_state = handler_sec;
    scs_set_vectactive(scs, handler_sec, exc_num);
    cpu->r[15] = handler | 1u;
    cpu->sleeping = MM_FALSE;
    cpu->event_reg = MM_FALSE;
    return MM_TRUE;
}

int main(int argc, char **argv)
{
    struct mm_image_spec images[16];
    char tui_command_line[512];
    int image_count = 0;
    size_t loaded_total = 0;
    size_t loaded_max_end = 0;
    mm_bool opt_gdb = MM_FALSE;
    mm_bool opt_dump = MM_FALSE;
    mm_u64 covdump_cycles = 0;
    const char *opt_covdump = 0;
    const char *opt_covdump_elf = 0;
    mm_bool opt_covdump_success_only = MM_FALSE;
    mm_bool opt_tui = MM_FALSE;
    mm_bool opt_persist = MM_FALSE;
    mm_bool opt_quit_on_faults = MM_FALSE;
    mm_bool opt_capstone = MM_FALSE;
    mm_bool opt_capstone_verbose = MM_FALSE;
    mm_bool opt_uart_stdout = MM_FALSE;
    mm_bool opt_meminfo = MM_FALSE;
    mm_bool opt_record = MM_FALSE;
    mm_bool opt_record_start_set = MM_FALSE;
    mm_u32 opt_record_start_pc = 0;
    mm_u32 opt_record_dump = 0;
    mm_bool opt_record_start_dump = MM_FALSE;
    mm_bool opt_record_start_dump_ram = MM_FALSE;
    mm_bool opt_record_end_dump_ram = MM_FALSE;
    mm_u32 opt_record_window = 0;
    mm_bool opt_call_trace = MM_FALSE;
    mm_bool opt_dualbank = MM_FALSE;
    const char *gdb_symbols = 0;
    const char *gdb_symbols_list[32];
    size_t gdb_symbols_count = 0;
    int gdb_port = 1234;
    const char *cpu_name = 0;
    int i;
    struct mm_target_cfg cfg;
    struct mm_memmap map;
            struct mmio_region regions[256];
    struct mm_cpu cpu;
    struct mm_cpu cpu1;
    struct mm_scs scs;
    struct mm_scs scs1;
    struct mm_prot_ctx prot;
    struct mm_prot_ctx prot1;
    struct mm_nvic nvic;
    struct mm_nvic nvic1;
    struct mm_scs_mux scs_mux;
    mm_u32 active_core = 0;
    mm_u8 *flash;
    mm_u8 *ram;
    struct mm_gdb_stub gdb;
    static struct mm_tui tui;
    struct mm_flash_persist persist;
    mm_bool tui_active = MM_FALSE;
    int rc = 0;
    struct mm_load_targets targets;
    /* IT block tracking: pattern encodes THEN(1)/ELSE(0) for remaining instructions. */
    mm_u8 it_pattern = 0;
    mm_u8 it_remaining = 0;
    mm_u8 it_cond = 0;
    mm_u8 it_pattern1 = 0;
    mm_u8 it_remaining1 = 0;
    mm_u8 it_cond1 = 0;
    mm_bool tui_paused = MM_FALSE;
    mm_bool tui_step = MM_FALSE;
    mm_bool reload_pending = MM_FALSE;
    mm_u64 tui_steps_offset = 0;
    mm_u64 tui_steps_latched = 0;
    mm_bool last_running = MM_TRUE;
    mm_bool opt_usb = MM_FALSE;
    char usb_udc[128];
    enum mm_eth_backend_type eth_backend = MM_ETH_BACKEND_NONE;
    const char *eth_spec = 0;
    struct mm_spiflash_cfg spiflash_cfgs[8];
    int spiflash_count = 0;
#ifdef M33MU_HAS_LIBTPMS
    struct mm_tpm_tis_cfg tpm_cfgs[4];
    int tpm_count = 0;
#endif
    struct mm_ta100_cfg ta100_cfgs[4];
    int ta100_count = 0;
    struct mm_iotsafe_uart_cfg iotsafe_uart_cfgs[4];
    int iotsafe_uart_count = 0;
#ifdef M33MU_HAS_RUST_PLUGINS
    struct mm_se050_cfg se050_cfgs[4];
    int se050_count = 0;
    struct mm_atecc608_cfg atecc608_cfgs[4];
    int atecc608_count = 0;
    struct mm_stsafe_cfg stsafe_cfgs[4];
    int stsafe_count = 0;
    struct mm_tropic01_cfg tropic01_cfgs[4];
    int tropic01_count = 0;
#endif
    mm_bool opt_no_tz = MM_FALSE;
    mm_u32 opt_secwm_strt[2] = { 0u, 0u };
    mm_u32 opt_secwm_end[2] = { 0u, 0u };
    mm_bool opt_secwm_set[2] = { MM_FALSE, MM_FALSE };
    const char *memwatch_env = getenv("M33MU_MEMWATCH");
    const char *capstone_pc_env = getenv("CAPSTONE_PC");
    const char *disable_tb_env = getenv("M33MU_DISABLE_TB");
    mm_u32 memwatch_addr = 0;
    mm_u32 memwatch_size = 0;
    mm_u32 capstone_pc = 0;
    mm_bool opt_capstone_pc = MM_FALSE;
    mm_bool opt_boot_offset = MM_FALSE;
    mm_u32 boot_offset = 0;
    enum mm_boot_mode boot_mode = MM_BOOT_FLASH;
    mm_bool opt_boot_mode = MM_FALSE;
    mm_bool opt_expect_bkpt = MM_FALSE;
    mm_u32 expect_bkpt = 0;
    mm_bool expect_bkpt_hit = MM_FALSE;
    mm_u32 opt_timeout = 0;
    mm_bool opt_puf_seed_set = MM_FALSE;
    mm_u64 opt_puf_seed = 0;
    mm_u64 opt_puf_cold_boot_count = 0;
    mm_u32 opt_puf_noise = 0;
    mm_u8 opt_sbkload_key[32];
    mm_u32 opt_sbkload_key_len = 0u;
    mm_u64 opt_fault_clocks[16];
    mm_u8 opt_fault_clock_count = 0;
    mm_u8 *spiflash_boot_data = 0;
    size_t spiflash_boot_size = 0;
    mm_u32 spiflash_boot_base = 0;
    mm_bool opt_disable_tb = (disable_tb_env != 0);

    snprintf(usb_udc, sizeof(usb_udc), "dummy_udc.0");

    if (memwatch_env != 0 && memwatch_env[0] != '\0') {
        if (parse_addr_size(memwatch_env, &memwatch_addr, &memwatch_size)) {
            mm_memmap_set_watch(memwatch_addr, memwatch_size);
        }
    }
    if (capstone_pc_env != 0 && capstone_pc_env[0] != '\0') {
        if (parse_hex_u32(capstone_pc_env, &capstone_pc)) {
            opt_capstone_pc = MM_TRUE;
        }
    }

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--gdb") == 0) {
            opt_gdb = MM_TRUE;
        } else if (strcmp(argv[i], "--dump") == 0) {
            opt_dump = MM_TRUE;
        } else if (strcmp(argv[i], "--covdump") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing covdump prefix\n");
                return 1;
            }
            opt_covdump = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--covdump-elf") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing covdump-elf path\n");
                return 1;
            }
            opt_covdump_elf = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--covdump-when") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing covdump-when value\n");
                return 1;
            }
            if (strcmp(argv[i + 1], "success") == 0) {
                opt_covdump_success_only = MM_TRUE;
            } else if (strcmp(argv[i + 1], "always") == 0) {
                opt_covdump_success_only = MM_FALSE;
            } else {
                fprintf(stderr, "invalid covdump-when value: %s "
                                "(expected success or always)\n", argv[i + 1]);
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "--tui") == 0) {
#ifdef M33MU_HAS_NCURSES
            opt_tui = MM_TRUE;
#else
            fprintf(stderr, "TUI support is not available (ncurses not found at build time)\n");
            return 1;
#endif
        } else if (strcmp(argv[i], "--gdb-symbols") == 0 && i + 1 < argc) {
            gdb_symbols = argv[i + 1];
            if (gdb_symbols_count < (sizeof(gdb_symbols_list) / sizeof(gdb_symbols_list[0]))) {
                gdb_symbols_list[gdb_symbols_count++] = gdb_symbols;
            } else {
                fprintf(stderr, "too many --gdb-symbols entries\n");
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            gdb_port = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "?") == 0 || strcmp(argv[i + 1], "list") == 0) {
                size_t ci;
                fprintf(stderr, "valid cpus:");
                for (ci = 0; ci < mm_cpu_count(); ++ci) {
                    const char *name = mm_cpu_name_at(ci);
                    if (name != 0) {
                        fprintf(stderr, "\n%s", name);
                    }
                }
                fprintf(stderr, "\n");
                return 0;
            }
            cpu_name = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--persist") == 0) {
            opt_persist = MM_TRUE;
        } else if (strcmp(argv[i], "--puf-seed") == 0 && i + 1 < argc) {
            if (!parse_hex_u64(argv[i + 1], &opt_puf_seed)) {
                fprintf(stderr, "invalid puf seed value: %s\n", argv[i + 1]);
                return 1;
            }
            opt_puf_seed_set = MM_TRUE;
            i++;
        } else if (strncmp(argv[i], "--puf-seed=", 11) == 0) {
            if (!parse_hex_u64(argv[i] + 11, &opt_puf_seed)) {
                fprintf(stderr, "invalid puf seed value: %s\n", argv[i]);
                return 1;
            }
            opt_puf_seed_set = MM_TRUE;
        } else if (strcmp(argv[i], "--puf-cold-boot") == 0 && i + 1 < argc) {
            if (!parse_hex_u64(argv[i + 1], &opt_puf_cold_boot_count)) {
                fprintf(stderr, "invalid puf cold boot value: %s\n", argv[i + 1]);
                return 1;
            }
            i++;
        } else if (strncmp(argv[i], "--puf-cold-boot=", 16) == 0) {
            if (!parse_hex_u64(argv[i] + 16, &opt_puf_cold_boot_count)) {
                fprintf(stderr, "invalid puf cold boot value: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--puf-noise") == 0 && i + 1 < argc) {
            if (!parse_hex_u32(argv[i + 1], &opt_puf_noise) || opt_puf_noise > 127u) {
                fprintf(stderr, "invalid puf noise value: %s\n", argv[i + 1]);
                return 1;
            }
            i++;
        } else if (strncmp(argv[i], "--puf-noise=", 12) == 0) {
            if (!parse_hex_u32(argv[i] + 12, &opt_puf_noise) || opt_puf_noise > 127u) {
                fprintf(stderr, "invalid puf noise value: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--sbkload-key") == 0 && i + 1 < argc) {
            opt_sbkload_key_len = 0u;
            if (!parse_hex_blob(argv[i + 1], opt_sbkload_key, sizeof(opt_sbkload_key),
                                &opt_sbkload_key_len) ||
                (opt_sbkload_key_len != 16u && opt_sbkload_key_len != 32u)) {
                fprintf(stderr, "invalid sbkload key value: %s\n", argv[i + 1]);
                return 1;
            }
            i++;
        } else if (strncmp(argv[i], "--sbkload-key=", 14) == 0) {
            opt_sbkload_key_len = 0u;
            if (!parse_hex_blob(argv[i] + 14, opt_sbkload_key, sizeof(opt_sbkload_key),
                                &opt_sbkload_key_len) ||
                (opt_sbkload_key_len != 16u && opt_sbkload_key_len != 32u)) {
                fprintf(stderr, "invalid sbkload key value: %s\n", argv[i]);
                return 1;
            }
#ifdef M33MU_USE_LIBCAPSTONE
        } else if (strcmp(argv[i], "--capstone") == 0) {
            opt_capstone = MM_TRUE;
        } else if (strcmp(argv[i], "--capstone-verbose") == 0) {
            opt_capstone = MM_TRUE;
            opt_capstone_verbose = MM_TRUE;
#endif
        } else if (strcmp(argv[i], "--uart-stdout") == 0) {
            opt_uart_stdout = MM_TRUE;
        } else if (strcmp(argv[i], "--quit-on-faults") == 0) {
            opt_quit_on_faults = MM_TRUE;
        } else if (strcmp(argv[i], "--dualbank") == 0) {
            opt_dualbank = MM_TRUE;
        } else if (strcmp(argv[i], "--meminfo") == 0) {
            opt_meminfo = MM_TRUE;
        } else if (strcmp(argv[i], "--record") == 0) {
            opt_record = MM_TRUE;
        } else if (strcmp(argv[i], "--record-start") == 0) {
            mm_u32 v;
            if (i + 1 >= argc || !parse_hex_u32(argv[i + 1], &v)) {
                fprintf(stderr, "invalid record-start value: %s\n", (i + 1 < argc) ? argv[i + 1] : "");
                return 1;
            }
            opt_record_start_set = MM_TRUE;
            opt_record_start_pc = v & ~1u;
            opt_record = MM_TRUE;
            i++;
        } else if (strncmp(argv[i], "--record-start=", 15) == 0) {
            mm_u32 v;
            if (!parse_hex_u32(argv[i] + 15, &v)) {
                fprintf(stderr, "invalid record-start value: %s\n", argv[i]);
                return 1;
            }
            opt_record_start_set = MM_TRUE;
            opt_record_start_pc = v & ~1u;
            opt_record = MM_TRUE;
        } else if (strcmp(argv[i], "--record-dump") == 0) {
            unsigned long v;
            if (i + 1 >= argc) {
                fprintf(stderr, "missing record-dump value\n");
                return 1;
            }
            v = strtoul(argv[i + 1], 0, 10);
            if (v == 0u) {
                fprintf(stderr, "invalid record-dump value: %s\n", argv[i + 1]);
                return 1;
            }
            opt_record_dump = (mm_u32)v;
            opt_record = MM_TRUE;
            i++;
        } else if (strncmp(argv[i], "--record-dump=", 14) == 0) {
            unsigned long v = strtoul(argv[i] + 14, 0, 10);
            if (v == 0u) {
                fprintf(stderr, "invalid record-dump value: %s\n", argv[i]);
                return 1;
            }
            opt_record_dump = (mm_u32)v;
            opt_record = MM_TRUE;
        } else if (strcmp(argv[i], "--record-start-dump") == 0) {
            opt_record_start_dump = MM_TRUE;
            opt_record = MM_TRUE;
        } else if (strcmp(argv[i], "--record-start-dump-ram") == 0) {
            opt_record_start_dump_ram = MM_TRUE;
            opt_record_start_dump = MM_TRUE;
            opt_record = MM_TRUE;
        } else if (strcmp(argv[i], "--record-trace") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing record-trace path\n");
                return 1;
            }
            g_record_trace_fp = fopen(argv[i + 1], "w");
            if (g_record_trace_fp == NULL) {
                fprintf(stderr, "failed to open record-trace file: %s\n", argv[i + 1]);
                return 1;
            }
            opt_record = MM_TRUE;
            i++;
        } else if (strcmp(argv[i], "--record-quiet") == 0) {
            opt_record = MM_TRUE;
            g_record_trace_live = MM_FALSE;
        } else if (strcmp(argv[i], "--record-end-dump-ram") == 0) {
            opt_record_end_dump_ram = MM_TRUE;
            opt_record = MM_TRUE;
        } else if (strcmp(argv[i], "--record-window") == 0) {
            mm_u32 v;
            if (i + 1 >= argc || !parse_hex_u32(argv[i + 1], &v)) {
                fprintf(stderr, "invalid record-window value: %s\n", (i + 1 < argc) ? argv[i + 1] : "");
                return 1;
            }
            opt_record_window = v;
            opt_record = MM_TRUE;
            i++;
        } else if (strncmp(argv[i], "--record-window=", 16) == 0) {
            mm_u32 v = 0;
            if (!parse_hex_u32(argv[i] + 16, &v)) {
                fprintf(stderr, "invalid record-window value: %s\n", argv[i]);
                return 1;
            }
            opt_record_window = v;
            opt_record = MM_TRUE;
        } else if (strcmp(argv[i], "--call-trace") == 0) {
            opt_call_trace = MM_TRUE;
        } else if (strcmp(argv[i], "--fault-clock") == 0) {
            unsigned long long t;
            if (i + 1 >= argc) {
                fprintf(stderr, "missing fault-clock value\n");
                return 1;
            }
            t = strtoull(argv[i + 1], 0, 10);
            if (t == 0u) {
                fprintf(stderr, "invalid fault-clock: %s\n", argv[i + 1]);
                return 1;
            }
            if (!fault_clock_add((mm_u64)t, opt_fault_clocks, &opt_fault_clock_count)) {
                fprintf(stderr, "invalid or contiguous fault-clock: %s\n", argv[i + 1]);
                return 1;
            }
            i++;
        } else if (strncmp(argv[i], "--fault-clock=", 13) == 0) {
            unsigned long long t = strtoull(argv[i] + 13, 0, 10);
            if (t == 0u) {
                fprintf(stderr, "invalid fault-clock: %s\n", argv[i]);
                return 1;
            }
            if (!fault_clock_add((mm_u64)t, opt_fault_clocks, &opt_fault_clock_count)) {
                fprintf(stderr, "invalid or contiguous fault-clock: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--expect-bkpt") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing expect-bkpt value\n");
                return 1;
            }
            if (!parse_hex_u32(argv[i + 1], &expect_bkpt)) {
                fprintf(stderr, "invalid expect-bkpt value: %s\n", argv[i + 1]);
                return 1;
            }
            opt_expect_bkpt = MM_TRUE;
            i++;
        } else if (strncmp(argv[i], "--expect-bkpt=", 14) == 0) {
            if (!parse_hex_u32(argv[i] + 14, &expect_bkpt)) {
                fprintf(stderr, "invalid expect-bkpt value: %s\n", argv[i]);
                return 1;
            }
            opt_expect_bkpt = MM_TRUE;
        } else if (strcmp(argv[i], "--timeout") == 0) {
            unsigned long t;
            if (i + 1 >= argc) {
                fprintf(stderr, "missing timeout value\n");
                return 1;
            }
            t = strtoul(argv[i + 1], 0, 10);
            if (t == 0u) {
                fprintf(stderr, "invalid timeout: %s\n", argv[i + 1]);
                return 1;
            }
            opt_timeout = (mm_u32)t;
            i++;
        } else if (strncmp(argv[i], "--timeout=", 10) == 0) {
            unsigned long t = strtoul(argv[i] + 10, 0, 10);
            if (t == 0u) {
                fprintf(stderr, "invalid timeout: %s\n", argv[i]);
                return 1;
            }
            opt_timeout = (mm_u32)t;
        } else if (strcmp(argv[i], "--no-tz") == 0) {
            opt_no_tz = MM_TRUE;
        } else if (strncmp(argv[i], "--secwm1=", 9) == 0 ||
                   strncmp(argv[i], "--secwm2=", 9) == 0) {
            /* Provision a flash secure watermark, as the option bytes of a
             * real board would carry it. Format: --secwm1=<strt>:<end>. */
            int bank = (argv[i][7] == '2') ? 1 : 0;
            const char *spec = argv[i] + 9;
            char *endp = 0;
            unsigned long strt;
            unsigned long end;
            strt = strtoul(spec, &endp, 0);
            if (endp == spec || endp == 0 || *endp != ':') {
                fprintf(stderr, "invalid watermark: %s (expected <strt>:<end>)\n", argv[i]);
                return 1;
            }
            spec = endp + 1;
            end = strtoul(spec, &endp, 0);
            if (endp == spec || endp == 0 || *endp != '\0') {
                fprintf(stderr, "invalid watermark: %s (expected <strt>:<end>)\n", argv[i]);
                return 1;
            }
            if (strt > 0xFFFFu || end > 0xFFFFu) {
                fprintf(stderr, "watermark sector out of range: %s\n", argv[i]);
                return 1;
            }
            opt_secwm_strt[bank] = (mm_u32)strt;
            opt_secwm_end[bank] = (mm_u32)end;
            opt_secwm_set[bank] = MM_TRUE;
        } else if (strcmp(argv[i], "--boot") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing boot mode\n");
                return 1;
            }
            if (!parse_boot_mode(argv[i + 1], &boot_mode)) {
                fprintf(stderr, "invalid boot mode: %s\n", argv[i + 1]);
                return 1;
            }
            opt_boot_mode = MM_TRUE;
            i++;
        } else if (strncmp(argv[i], "--boot=", 7) == 0) {
            if (!parse_boot_mode(argv[i] + 7, &boot_mode)) {
                fprintf(stderr, "invalid boot mode: %s\n", argv[i] + 7);
                return 1;
            }
            opt_boot_mode = MM_TRUE;
        } else if (strncmp(argv[i], "--boot-offset=", 14) == 0) {
            if (!parse_hex_u32(argv[i] + 14, &boot_offset)) {
                fprintf(stderr, "invalid boot offset: %s\n", argv[i]);
                return 1;
            }
            opt_boot_offset = MM_TRUE;
        } else if (strncmp(argv[i], "--spiflash:", 11) == 0) {
            if (spiflash_count >= (int)(sizeof(spiflash_cfgs) / sizeof(spiflash_cfgs[0]))) {
                fprintf(stderr, "too many spiflash configs\n");
                return 1;
            }
            if (!mm_spiflash_parse_spec(argv[i] + 11, &spiflash_cfgs[spiflash_count])) {
                fprintf(stderr, "invalid spiflash spec: %s\n", argv[i]);
                return 1;
            }
            spiflash_count++;
        } else if (strcmp(argv[i], "--usb") == 0) {
            opt_usb = MM_TRUE;
        } else if (strncmp(argv[i], "--usb:", 6) == 0) {
            opt_usb = MM_TRUE;
            if (!parse_usb_spec(argv[i] + 6, usb_udc, sizeof(usb_udc))) {
                fprintf(stderr, "invalid usb spec: %s\n", argv[i]);
                return 1;
            }
#ifdef M33MU_HAS_VDE
        } else if (strcmp(argv[i], "--vde") == 0) {
            if (eth_backend != MM_ETH_BACKEND_NONE) {
                fprintf(stderr, "only one ethernet backend can be selected\n");
                return 1;
            }
            eth_backend = MM_ETH_BACKEND_VDE;
            eth_spec = "/var/run/vde.ctl";
        } else if (strncmp(argv[i], "--vde:", 6) == 0) {
            if (eth_backend != MM_ETH_BACKEND_NONE) {
                fprintf(stderr, "only one ethernet backend can be selected\n");
                return 1;
            }
            eth_backend = MM_ETH_BACKEND_VDE;
            eth_spec = argv[i] + 6;
#else
        } else if (strcmp(argv[i], "--vde") == 0 || strncmp(argv[i], "--vde:", 6) == 0) {
            fprintf(stderr, "VDE backend requested but vde-2 not available at build time\n");
            return 1;
#endif
        } else if (strcmp(argv[i], "--tap") == 0) {
            if (eth_backend != MM_ETH_BACKEND_NONE) {
                fprintf(stderr, "only one ethernet backend can be selected\n");
                return 1;
            }
            eth_backend = MM_ETH_BACKEND_TAP;
            eth_spec = "tap0";
        } else if (strncmp(argv[i], "--tap:", 6) == 0) {
            if (eth_backend != MM_ETH_BACKEND_NONE) {
                fprintf(stderr, "only one ethernet backend can be selected\n");
                return 1;
            }
            eth_backend = MM_ETH_BACKEND_TAP;
            eth_spec = argv[i] + 6;
#ifdef M33MU_HAS_LIBTPMS
        } else if (strncmp(argv[i], "--tpm:", 6) == 0) {
            if (tpm_count >= (int)(sizeof(tpm_cfgs) / sizeof(tpm_cfgs[0]))) {
                fprintf(stderr, "too many tpm configs\n");
                return 1;
            }
            if (!mm_tpm_tis_parse_spec(argv[i] + 6, &tpm_cfgs[tpm_count])) {
                fprintf(stderr, "invalid tpm spec: %s\n", argv[i]);
                return 1;
            }
            tpm_count++;
#endif
        } else if (strncmp(argv[i], "--ta100:", 8) == 0) {
            if (ta100_count >= (int)(sizeof(ta100_cfgs) / sizeof(ta100_cfgs[0]))) {
                fprintf(stderr, "too many ta100 configs\n");
                return 1;
            }
            if (!mm_ta100_parse_spec(argv[i] + 8, &ta100_cfgs[ta100_count])) {
                fprintf(stderr, "invalid ta100 spec: %s\n", argv[i]);
                return 1;
            }
            ta100_count++;
        } else if (strncmp(argv[i], "--iotsafe-uart:", 15) == 0) {
            if (iotsafe_uart_count >= (int)(sizeof(iotsafe_uart_cfgs) /
                                            sizeof(iotsafe_uart_cfgs[0]))) {
                fprintf(stderr, "too many iotsafe-uart configs\n");
                return 1;
            }
            if (!mm_iotsafe_uart_parse_spec(argv[i] + 15,
                                            &iotsafe_uart_cfgs[iotsafe_uart_count])) {
                fprintf(stderr, "invalid iotsafe-uart spec: %s\n", argv[i]);
                return 1;
            }
            iotsafe_uart_count++;
#ifdef M33MU_HAS_RUST_PLUGINS
        } else if (strncmp(argv[i], "--se050:", 8) == 0) {
            if (se050_count >= (int)(sizeof(se050_cfgs) / sizeof(se050_cfgs[0]))) {
                fprintf(stderr, "too many se050 configs\n");
                return 1;
            }
            if (!mm_se050_parse_spec(argv[i] + 8, &se050_cfgs[se050_count])) {
                fprintf(stderr, "invalid se050 spec: %s\n", argv[i]);
                return 1;
            }
            se050_count++;
        } else if (strncmp(argv[i], "--atecc608:", 11) == 0) {
            if (atecc608_count >= (int)(sizeof(atecc608_cfgs) / sizeof(atecc608_cfgs[0]))) {
                fprintf(stderr, "too many atecc608 configs\n");
                return 1;
            }
            if (!mm_atecc608_parse_spec(argv[i] + 11, &atecc608_cfgs[atecc608_count])) {
                fprintf(stderr, "invalid atecc608 spec: %s\n", argv[i]);
                return 1;
            }
            atecc608_count++;
        } else if (strncmp(argv[i], "--stsafe:", 9) == 0) {
            if (stsafe_count >= (int)(sizeof(stsafe_cfgs) / sizeof(stsafe_cfgs[0]))) {
                fprintf(stderr, "too many stsafe configs\n");
                return 1;
            }
            if (!mm_stsafe_parse_spec(argv[i] + 9, &stsafe_cfgs[stsafe_count])) {
                fprintf(stderr, "invalid stsafe spec: %s\n", argv[i]);
                return 1;
            }
            stsafe_count++;
        } else if (strncmp(argv[i], "--tropic01:", 11) == 0) {
            if (tropic01_count >= (int)(sizeof(tropic01_cfgs) / sizeof(tropic01_cfgs[0]))) {
                fprintf(stderr, "too many tropic01 configs\n");
                return 1;
            }
            if (!mm_tropic01_parse_spec(argv[i] + 11, &tropic01_cfgs[tropic01_count])) {
                fprintf(stderr, "invalid tropic01 spec: %s\n", argv[i]);
                return 1;
            }
            tropic01_count++;
#endif
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        } else {
            if (image_count >= (int)(sizeof(images) / sizeof(images[0]))) {
                fprintf(stderr, "too many images\n");
                return 1;
            }
            images[image_count].path = 0;
            images[image_count].offset = 0;
            images[image_count].loaded = 0;
            images[image_count].load_start = 0;
            images[image_count].load_end = 0;
            images[image_count].type = MM_IMAGE_UNKNOWN;
            if (!parse_image_spec(argv[i], &images[image_count].path, &images[image_count].offset)) {
                fprintf(stderr, "invalid image spec: %s\n", argv[i]);
                return 1;
            }
            image_count++;
        }
    }

    if (image_count == 0) {
        fprintf(stderr, "usage: %s [--cpu cpu] [--gdb] [--port <n>] [--dump] [--record] "
#ifdef M33MU_HAS_NCURSES
                        "[--tui] "
#endif
                        "[--persist] "
                        "[--puf-seed <value>] [--puf-cold-boot <n>] [--puf-noise <n>] "
                        "[--sbkload-key <hex>] "
#ifdef M33MU_USE_LIBCAPSTONE
                        "[--capstone] [--capstone-verbose] "
#endif
                        "[--uart-stdout] [--quit-on-faults] [--meminfo] [--record] [--record-start <pc>] [--record-start-dump] [--record-start-dump-ram] [--record-end-dump-ram] [--record-trace <path>] [--record-quiet] [--record-dump <n>] [--call-trace] [--dualbank] [--fault-clock N] [--no-tz] [--secwm1=<strt>:<end>] [--secwm2=<strt>:<end>] [--gdb-symbols <elf>] "
                        "[--expect-bkpt 0xNN] [--timeout seconds] "
                        "[--boot flash|ram|spiflash] "
                        "[--boot-offset=0xN] "
                        "[--spiflash:SPIx:file=<path>:size=<n>[:mmap=0xaddr][:cs=GPIONAME]] "
                        "[--usb[:udc=<name>|path=/dev/gadget/<name>]] "
                        "[--tap[:name]] [--vde[:/path/to/vde.ctl]] "
#ifdef M33MU_HAS_LIBTPMS
                        "[--tpm:SPIx:cs=GPIONAME[:file=<path>]] "
#endif
                        "[--ta100:SPIx:cs=GPIONAME[:file=<path>][:profile=<name>][:serial=<hex>]] "
                        "[--se050:I2Cx[:addr=<hex>][:file=<path>]] "
                        "[--iotsafe-uart:<uart-base-hex>[:file=<path>]] "
#ifdef M33MU_HAS_RUST_PLUGINS
                        "[--atecc608:SPIx:cs=GPIONAME[:file=<path>]] "
                        "[--stsafe:I2Cx[:addr=<hex>][:file=<path>]] "
                        "[--tropic01:SPIx:cs=GPIONAME[:file=<path>]] "
#endif
                        "<image.bin[:offset]|image.elf|image.hex|image.uf2> [more images...]\n",
                argv[0]);
        fprintf(stderr, "supported CPUs:");
        for (size_t i = 0; i < mm_cpu_count(); ++i) {
            fprintf(stderr, " %s", mm_cpu_name_at(i));
        }
        fprintf(stderr, "\n");
        return 1;
    }

    g_quit_on_faults = opt_quit_on_faults;
    g_call_trace = opt_call_trace;
    g_record_start_set = opt_record_start_set;
    g_record_start_pc = opt_record_start_pc;
    g_record_started = opt_record_start_set ? MM_FALSE : MM_TRUE;
    g_record_start_dump = opt_record_start_dump;
    g_record_start_dump_ram = opt_record_start_dump_ram;
    g_record_end_dump_ram = opt_record_end_dump_ram;
    g_record_dump_count = opt_record_dump;
    g_record_start_window = opt_record_window;
    g_record_start_remaining = opt_record_window;
    if (opt_tui && opt_uart_stdout) {
        fprintf(stderr, "warning: --uart-stdout disabled while TUI is active\n");
        opt_uart_stdout = MM_FALSE;
    }
    mm_uart_io_set_stdout(opt_uart_stdout);
    if (opt_meminfo) {
        mm_scs_set_meminfo(MM_TRUE);
    }
    if (opt_timeout > 0u) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handle_timeout_alarm;
        sigemptyset(&sa.sa_mask);
        (void)sigaction(SIGALRM, &sa, 0);
        alarm(opt_timeout);
    }
    if (opt_puf_noise > 0u && !opt_puf_seed_set) {
        fprintf(stderr, "--puf-noise requires --puf-seed\n");
        return 1;
    }

    if (cpu_name == 0) {
        cpu_name = mm_cpu_default_name();
    }
    mm_stm32_saes_set_key_material(opt_puf_seed_set, opt_puf_seed,
                                   (opt_sbkload_key_len != 0u) ? opt_sbkload_key : 0,
                                   opt_sbkload_key_len);
    if (!mm_cpu_lookup(cpu_name, &cfg)) {
        size_t ci;
        fprintf(stderr, "unknown cpu: %s\n", cpu_name);
        fprintf(stderr, "valid cpus:");
        for (ci = 0; ci < mm_cpu_count(); ++ci) {
            const char *name = mm_cpu_name_at(ci);
            if (name != 0) {
                fprintf(stderr, " %s", name);
            }
        }
        fprintf(stderr, "\n");
        return 1;
    }
    if (opt_dualbank) {
        cfg.flags |= MM_TARGET_FLAG_DUALBANK;
    }
    if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
        mm_rp2350_otp_init(cpu_name);
    } else if (cpu_name != 0 && strcmp(cpu_name, "stm32h563") == 0) {
        mm_stm32h563_otp_init(cpu_name);
    } else if (cpu_name != 0 && strcmp(cpu_name, "stm32h533") == 0) {
        mm_stm32h533_otp_init(cpu_name);
    } else if (cpu_name != 0 && strcmp(cpu_name, "stm32h5f4") == 0) {
        mm_stm32h5f4_otp_init(cpu_name);
    }

    if (image_count > 0 && boot_mode != MM_BOOT_RAM) {
        int ii;
        for (ii = 0; ii < image_count; ++ii) {
            mm_bool ram_boot = MM_FALSE;
            if (detect_image_type(images[ii].path) != MM_IMAGE_UF2) {
                continue;
            }
            if (uf2_scan_for_ram_boot(images[ii].path, &cfg, cpu_name, &ram_boot) && ram_boot) {
                if (opt_boot_mode) {
                    printf("warning: UF2 requests RAM boot; overriding --boot to ram\n");
                }
                boot_mode = MM_BOOT_RAM;
                break;
            }
        }
    }

    if (opt_capstone) {
        if (!capstone_available() || !capstone_init()) {
            fprintf(stderr, "failed to initialize capstone\n");
            return 1;
        }
        (void)capstone_set_enabled(MM_TRUE);
        printf("[CAPSTONE] Cross-checker activated\n");
    }

    for (i = 0; i < spiflash_count; ++i) {
        if (!mm_spiflash_register_cfg(&spiflash_cfgs[i])) {
            fprintf(stderr, "failed to register spiflash for %s\n", spiflash_cfgs[i].path);
            return 1;
        }
    }
#ifdef M33MU_HAS_LIBTPMS
    for (i = 0; i < tpm_count; ++i) {
        if (!mm_tpm_tis_register_cfg(&tpm_cfgs[i])) {
            fprintf(stderr, "failed to register tpm\n");
            return 1;
        }
    }
#endif

    for (i = 0; i < ta100_count; ++i) {
        if (!mm_ta100_register_cfg(&ta100_cfgs[i])) {
            fprintf(stderr, "failed to register ta100\n");
            return 1;
        }
    }
    for (i = 0; i < iotsafe_uart_count; ++i) {
        if (!mm_iotsafe_uart_register_cfg(&iotsafe_uart_cfgs[i])) {
            fprintf(stderr, "failed to register iotsafe-uart\n");
            return 1;
        }
    }
#ifdef M33MU_HAS_RUST_PLUGINS
    for (i = 0; i < se050_count; ++i) {
        if (!mm_se050_register_cfg(&se050_cfgs[i])) {
            fprintf(stderr, "failed to register se050\n");
            return 1;
        }
    }
    for (i = 0; i < atecc608_count; ++i) {
        if (!mm_atecc608_register_cfg(&atecc608_cfgs[i])) {
            fprintf(stderr, "failed to register atecc608\n");
            return 1;
        }
    }
    for (i = 0; i < stsafe_count; ++i) {
        if (!mm_stsafe_register_cfg(&stsafe_cfgs[i])) {
            fprintf(stderr, "failed to register stsafe\n");
            return 1;
        }
    }
    for (i = 0; i < tropic01_count; ++i) {
        if (!mm_tropic01_register_cfg(&tropic01_cfgs[i])) {
            fprintf(stderr, "failed to register tropic01\n");
            return 1;
        }
    }
#endif

    {
        mm_u8 *spiflash_data = 0;
        size_t spiflash_size = 0;
        mm_u32 spiflash_base = 0;
        mm_bool spiflash_ready = MM_FALSE;
        size_t si;
        struct mm_spiflash_info info;

        if (boot_mode == MM_BOOT_SPIFLASH) {
            for (si = 0; si < mm_spiflash_count(); ++si) {
                if (!mm_spiflash_get_info(si, &info)) continue;
                if (!info.mmap) continue;
                if (!mm_spiflash_get_storage(si, &spiflash_data, &spiflash_size)) continue;
                spiflash_base = info.mmap_base;
                spiflash_ready = MM_TRUE;
                break;
            }
            if (!spiflash_ready) {
                fprintf(stderr, "boot mode spiflash requested but no mmap spiflash configured\n");
                return 1;
            }
        }

        if (opt_boot_offset) {
            size_t limit = cfg.flash_size_s;
            const char *kind = "flash";
            if (boot_mode == MM_BOOT_RAM) {
                limit = cfg_total_ram(&cfg);
                kind = "ram";
            } else if (boot_mode == MM_BOOT_SPIFLASH) {
                limit = spiflash_size;
                kind = "spiflash";
            }
            if ((size_t)boot_offset + 8u > limit) {
                fprintf(stderr, "boot offset 0x%08lx out of bounds (%s size 0x%08lx)\n",
                        (unsigned long)boot_offset,
                        kind,
                        (unsigned long)limit);
                return 1;
            }
        }

        if (boot_mode == MM_BOOT_SPIFLASH) {
            printf("[BOOT] mode=%s base=0x%08lx size=0x%08lx\n",
                   boot_mode_name(boot_mode),
                   (unsigned long)spiflash_base,
                   (unsigned long)spiflash_size);
        }

        spiflash_boot_data = spiflash_data;
        spiflash_boot_size = spiflash_size;
        spiflash_boot_base = spiflash_base;
    }

    memset(&tui, 0, sizeof(tui));
    tui_command_line[0] = '\0';
    for (i = 0; i < argc; ++i) {
        size_t len = strlen(tui_command_line);
        size_t arg_len = strlen(argv[i]);
        if (len + arg_len + ((len > 0u) ? 1u : 0u) >= sizeof(tui_command_line)) {
            break;
        }
        if (len > 0u) {
            tui_command_line[len++] = ' ';
            tui_command_line[len] = '\0';
        }
        memcpy(tui_command_line + len, argv[i], arg_len);
        tui_command_line[len + arg_len] = '\0';
    }
    if (opt_tui) {
        if (!mm_tui_init(&tui) || !mm_tui_redirect_stdio(&tui)) {
            fprintf(stderr, "failed to initialize TUI\n");
            return 1;
        }
        mm_tui_set_command_line(&tui, tui_command_line);
        set_tui_image0(&tui, images, image_count);
        tui_active = MM_TRUE;
        mm_tui_register(&tui);
        if (!mm_tui_start_thread(&tui)) {
            fprintf(stderr, "failed to start TUI thread\n");
        }
    }

    flash = (mm_u8 *)malloc(cfg.flash_size_s);
    ram = (mm_u8 *)malloc(cfg_total_ram(&cfg));
    if (flash == NULL || ram == NULL) {
        fprintf(stderr, "out of memory\n");
        rc = 1;
        goto cleanup;
    }
    {
        size_t i;
        mm_u64 ram_seed_state = opt_puf_seed;
        for (i = 0; i < cfg.flash_size_s; ++i) {
            flash[i] = 0xFFu;
        }
        for (i = 0; i < cfg_total_ram(&cfg); ++i) {
            if (opt_puf_seed_set) {
                if ((i & 7u) == 0u) {
                    ram_seed_state = splitmix64_next(&ram_seed_state);
                }
                ram[i] = (mm_u8)(ram_seed_state >> ((i & 7u) * 8u));
            } else {
                ram[i] = (mm_u8)(rand() & 0xFF);
            }
        }
        if (opt_puf_seed_set && opt_puf_noise > 0u) {
            apply_puf_noise(ram, cfg_total_ram(&cfg), opt_puf_seed,
                            opt_puf_cold_boot_count, opt_puf_noise);
        }
    }

    targets.flash = flash;
    targets.flash_size = cfg.flash_size_s;
    targets.ram = ram;
    targets.ram_size = cfg_total_ram(&cfg);
    targets.spiflash = spiflash_boot_data;
    targets.spiflash_size = spiflash_boot_size;
    targets.spiflash_base = spiflash_boot_base;

    for (i = 0; i < image_count; ++i) {
        int j;
        mm_u32 b0;
        mm_u32 b1;
        if (load_image_autodetect(&images[i], &cfg, &targets, boot_mode, cpu_name) != 0) {
            fprintf(stderr, "failed to load image %s\n", images[i].path);
            rc = 1;
            goto cleanup;
        }
        b0 = images[i].load_start;
        b1 = images[i].load_end;
        loaded_total += images[i].loaded;
        if ((size_t)b1 > loaded_max_end) {
            loaded_max_end = (size_t)b1;
        }
        for (j = 0; j < i; ++j) {
            mm_u32 a0 = images[j].load_start;
            mm_u32 a1 = images[j].load_end;
            if (!(b1 <= a0 || b0 >= a1)) {
                fprintf(stderr, "warning: image %s overlaps %s\n", images[i].path, images[j].path);
            }
        }
        printf("Loaded %s image %s: %zu bytes [0x%08lx..0x%08lx]\n",
               image_type_name(images[i].type),
               images[i].path,
               images[i].loaded,
               (unsigned long)images[i].load_start,
               (unsigned long)images[i].load_end);
    }

    build_symbol_db(images, image_count, gdb_symbols_list, gdb_symbols_count);

    memset(&persist, 0, sizeof(persist));
    if (opt_persist) {
        const char *paths[16];
        mm_u32 offsets[16];
        int k;
        int persist_count = 0;
        if (boot_mode != MM_BOOT_FLASH) {
            printf("warning: --persist is only supported for flash boot; skipping\n");
        } else {
        for (k = 0; k < image_count; ++k) {
            if (images[k].type != MM_IMAGE_BIN) {
                printf("warning: persist skipped for non-BIN image %s\n", images[k].path);
                continue;
            }
            paths[persist_count] = images[k].path;
            offsets[persist_count] = images[k].offset;
            persist_count++;
        }
        if (persist_count > 0) {
            mm_flash_persist_build(&persist, flash, cfg.flash_size_s, paths, offsets, persist_count);
        }
        }
    }

    mm_gdb_stub_init(&gdb);
    if (opt_fault_clock_count > 0u) {
        mm_u8 i;
        if (opt_fault_clock_count > (mm_u8)(sizeof(gdb.fault_clocks) / sizeof(gdb.fault_clocks[0]))) {
            opt_fault_clock_count = (mm_u8)(sizeof(gdb.fault_clocks) / sizeof(gdb.fault_clocks[0]));
        }
        for (i = 0; i < opt_fault_clock_count; ++i) {
            gdb.fault_clocks[i] = opt_fault_clocks[i];
        }
        gdb.fault_clock_count = opt_fault_clock_count;
    }
    if (opt_gdb) {
        if (gdb_port <= 0 || gdb_port > 65535) {
            fprintf(stderr, "invalid gdb port: %d\n", gdb_port);
            rc = 1;
            goto cleanup;
        }
        mm_gdb_stub_set_cpu_name(&gdb, cpu_name);
        printf("Starting GDB server on port %d...\n", gdb_port);
        if (!mm_gdb_stub_start(&gdb, gdb_port)) {
            fprintf(stderr, "Failed to start GDB server\n");
            rc = 1;
            goto cleanup;
        }
        printf("Waiting for GDB connection...\n");
        if (!mm_gdb_stub_wait_client_blocking(&gdb)) {
            fprintf(stderr, "Failed to accept GDB connection\n");
            rc = 1;
            goto cleanup;
        }
        mm_gdb_stub_set_exec_path(&gdb, (gdb_symbols != 0) ? gdb_symbols : images[0].path);
    }

    {
        printf("Loaded total %zu bytes (max_end=0x%08lx)\n",
               loaded_total,
               (unsigned long)loaded_max_end);
    }

    {
        mm_bool first_start = MM_TRUE;
        if (opt_record) {
            mm_trace_init();
        }
        for (;;) {
            mm_u64 cycle_total = 0;
            mm_bool done = MM_FALSE;
            mm_bool reset_again = MM_FALSE;
            mm_bool force_ns_boot = MM_FALSE;
            mm_u32 initial_sp = 0;
            mm_u64 vcycles = 0;
            mm_u64 vcycles_last_sync = 0;
            mm_u64 cycles_since_poll = 0;
            struct mm_core_sys core_sys;
            struct mm_code_cache code_cache;
            mm_u64 fault_clocks[16];
            mm_u8 fault_clock_count = opt_fault_clock_count;
            mm_u32 boot_offset_local = 0;
            mm_u32 boot_base_s = 0;
            mm_u32 boot_base_ns = 0;
            enum mm_boot_mode boot_mode_local = boot_mode;
            const mm_u64 poll_granularity = DEFAULT_BATCH_CYCLES;
            mm_u64 sync_granularity = DEFAULT_SYNC_GRANULARITY;
            mm_u64 host0_ns = host_now_ns();
            mm_u64 cpu_hz = MM_CPU_HZ;
            mm_u64 hz_now = 0;
            mm_u64 last_hz = 0;
            tui_steps_offset = 0;
            if (fault_clock_count > 0u) {
                mm_u8 i;
                if (fault_clock_count > (mm_u8)(sizeof(fault_clocks) / sizeof(fault_clocks[0]))) {
                    fault_clock_count = (mm_u8)(sizeof(fault_clocks) / sizeof(fault_clocks[0]));
                }
                for (i = 0; i < fault_clock_count; ++i) {
                    fault_clocks[i] = opt_fault_clocks[i];
                }
            }

            mm_system_clear_reset();
            if (g_boot_override_pending) {
                boot_mode_local = (enum mm_boot_mode)g_boot_override_mode;
                g_boot_override_pending = MM_FALSE;
            }
            mm_memmap_init(&map, regions, sizeof(regions) / sizeof(regions[0]));
            mm_target_soc_reset(&cfg);
            mm_timer_reset(&cfg);
            mm_spiflash_reset_all();
#ifdef M33MU_HAS_LIBTPMS
            mm_tpm_tis_reset_all();
#endif
            mm_ta100_reset_all();
            mm_iotsafe_uart_reset_all();
#ifdef M33MU_HAS_RUST_PLUGINS
            mm_se050_reset_all();
            mm_atecc608_reset_all();
            mm_stsafe_reset_all();
            mm_tropic01_reset_all();
#endif
            mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE);
            mm_memmap_configure_flash(&map, &cfg, flash, MM_FALSE);
            mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE);
            mm_memmap_configure_ram(&map, &cfg, ram, MM_FALSE);
            memset(&core_sys, 0, sizeof(core_sys));
            core_sys.dwt.vcycles = &vcycles;
            core_sys.dwt.ctrl = 0u;
            core_sys.dwt.cyccnt_base = 0u;
            mm_code_cache_init(&code_cache, &map);
            mm_memmap_set_code_cache(&map, &code_cache);
            {
                if (opt_boot_offset) {
                    boot_offset_local = boot_offset;
                } else if (boot_mode_local == MM_BOOT_FLASH) {
                    boot_offset_local = default_rp2350_boot_offset(cpu_name, &cfg, images, image_count, flash, cfg.flash_size_s);
                } else {
                    boot_offset_local = 0u;
                }
                if (boot_mode_local == MM_BOOT_RAM) {
                    boot_base_s = cfg.ram_base_s + boot_offset_local;
                    boot_base_ns = cfg.ram_base_ns + boot_offset_local;
                } else if (boot_mode_local == MM_BOOT_SPIFLASH) {
                    boot_base_s = spiflash_boot_base + boot_offset_local;
                    boot_base_ns = spiflash_boot_base + boot_offset_local;
                } else {
                    boot_base_s = cfg.flash_base_s + boot_offset_local;
                    boot_base_ns = cfg.flash_base_ns + boot_offset_local;
                }
                if (opt_no_tz) {
                    force_ns_boot = MM_TRUE;
                    cfg.mpcbb_block_secure = 0;
                    cfg.mpcbb_block_size = 0;
                    /* No TrustZone: the target IDAU attribution must not
                     * fault accesses either. */
                    cfg.tz_attr_for_addr = 0;
                    printf("[TZ] TrustZone disabled via --no-tz\n");
                } else if (cfg.ram_base_s != cfg.ram_base_ns && boot_mode_local != MM_BOOT_SPIFLASH) {
                    if (mm_vector_read(&map, MM_SECURE, boot_base_s, 0u, &initial_sp)) {
                        if ((initial_sp & 0xF0000000u) == 0x20000000u) {
                            force_ns_boot = MM_TRUE;
                            cfg.mpcbb_block_secure = 0;
                            cfg.mpcbb_block_size = 0;
                            /* No TrustZone: the target IDAU attribution must
                             * not fault accesses either. */
                            cfg.tz_attr_for_addr = 0;
                            printf("[TZ] Non-secure boot detected (SP=0x%08lx); TrustZone disabled\n",
                                   (unsigned long)initial_sp);
                        }
                    }
                }
                if (force_ns_boot) {
                    map.flash.base = cfg.flash_base_ns;
                    map.flash.length = cfg.flash_size_ns;
                    map.ram.base = cfg.ram_base_ns;
                    if (boot_mode_local == MM_BOOT_FLASH) {
                        boot_base_s = cfg.flash_base_ns + boot_offset_local;
                        boot_base_ns = cfg.flash_base_ns + boot_offset_local;
                    }
                } else {
                    map.flash.base = cfg.flash_base_s;
                    map.flash.length = cfg.flash_size_s;
                    map.ram.base = cfg.ram_base_s;
                }
                map.ram.length = cfg_total_ram(&cfg);
            }
            if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                const struct rp2350_partition_table *pt = mm_rp2350_partition_table_get();
                mm_u32 diag = BOOT_DIAGNOSTIC_CONSIDERED | BOOT_DIAGNOSTIC_CHOSEN | BOOT_DIAGNOSTIC_IMAGE_LAUNCHED;
                mm_u8 boot_type = BOOT_TYPE_NORMAL;
                if (boot_mode_local == MM_BOOT_RAM) {
                    boot_type = BOOT_TYPE_RAM_IMAGE;
                }
                if (pt != 0 && pt->loaded && pt->partition_count > 0u) {
                    diag |= BOOT_DIAGNOSTIC_HAS_PARTITION_TABLE;
                }
                mm_rp2350_set_boot_info(BOOT_PARTITION_NONE,
                                        boot_type,
                                        BOOT_PARTITION_NONE,
                                        0u,
                                        diag,
                                        0u,
                                        0u);
            }
            if (opt_secwm_set[0] || opt_secwm_set[1]) {
                if (cpu_name == 0 || strncmp(cpu_name, "stm32h5", 7) != 0) {
                    fprintf(stderr, "--secwm1/--secwm2 are only supported on stm32h5 targets\n");
                    rc = 1;
                    goto cleanup;
                }
                if (opt_secwm_set[0]) {
                    stm32h5_flash_set_secwm(0, opt_secwm_strt[0], opt_secwm_end[0]);
                }
                if (opt_secwm_set[1]) {
                    stm32h5_flash_set_secwm(1, opt_secwm_strt[1], opt_secwm_end[1]);
                }
            }
            if (!mm_target_register_mmio(&cfg, &map.mmio)) {
                fprintf(stderr, "Failed to register MMIO for CPU %s\n", (cpu_name != 0) ? cpu_name : "unknown");
                rc = 1;
                goto cleanup;
            }
            mm_spiflash_register_mmap_regions(&map.mmio);
            mm_target_flash_bind(&cfg, &map, flash, cfg.flash_size_s, opt_persist ? &persist : 0);
            mm_target_usart_reset(&cfg);
            mm_target_usart_init(&cfg, &map.mmio, &nvic);
            mm_target_spi_reset(&cfg);
            mm_target_spi_init(&cfg, &map.mmio, &nvic);
            mm_target_eth_reset(&cfg);
            mm_target_eth_init(&cfg, &map.mmio, &nvic);
            mm_timer_init(&cfg, &map.mmio, &nvic);

            mm_scs_init(&scs, 0x410fc241u);
            mm_scs_set_fpu_present(&scs, (cfg.flags & MM_TARGET_FLAG_FPU) != 0u);
            if (cfg.core_count > 1u) {
                mm_scs_init(&scs1, 0x410fc241u);
                mm_scs_set_fpu_present(&scs1, (cfg.flags & MM_TARGET_FLAG_FPU) != 0u);
                scs_mux.scs[0] = &scs;
                scs_mux.scs[1] = &scs1;
                scs_mux.nvic[0] = &nvic;
                scs_mux.nvic[1] = &nvic1;
                scs_mux.core_count = cfg.core_count;
                scs_mux.active_core = &active_core;
                mm_scs_register_regions_multi(&scs_mux, &map.mmio, 0xE000ED00u, 0xE002ED00u);
            } else {
                mm_scs_register_regions(&scs, &map.mmio, 0xE000ED00u, 0xE002ED00u, &nvic);
            }
            if ((cfg.flags & MM_TARGET_FLAG_FPU) != 0u) {
                scs.cpacr_s |= 0x00F00000u;
                scs.cpacr_ns |= 0x00F00000u;
                scs.nsacr |= (1u << 10) | (1u << 11);
                if (cfg.core_count > 1u) {
                    scs1.cpacr_s |= 0x00F00000u;
                    scs1.cpacr_ns |= 0x00F00000u;
                    scs1.nsacr |= (1u << 10) | (1u << 11);
                }
            }
            mm_core_sys_register(&map.mmio, &core_sys);
            mm_prot_init(&prot, &scs, &cfg, &cpu);
            if (cfg.core_count > 1u) {
                mm_prot_init(&prot1, &scs1, &cfg, &cpu1);
            }
            g_active_prot_ctx = &prot;
            mm_memmap_set_interceptor(&map, prot_mux_interceptor, 0);
            mm_memmap_set_bus_attr(&map, prot_mux_bus_attr, 0);
            mm_prot_add_region(&prot, cfg.flash_base_s, cfg.flash_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
            mm_prot_add_region(&prot, cfg.flash_base_ns, cfg.flash_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
            /* Secure code may address the non-secure flash alias: on ARMv8-M a
             * secure master reaching an address the SAU/IDAU attributes NS just
             * issues a non-secure transaction, it is not denied.  Whether the
             * read returns data is then up to the flash TZ filter, which
             * mm_memmap_read applies.  Without this region the access never got
             * that far and read back as zero even for NS-attributed sectors. */
            mm_prot_add_region(&prot, cfg.flash_base_ns, cfg.flash_size_ns,
                               MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
            if (cpu_name != 0 && strcmp(cpu_name, "mcxn947") == 0) {
                mm_prot_add_region(&prot, 0x1303fc00u, 0x00000200u, MM_PROT_PERM_READ, MM_SECURE);
                mm_prot_add_region(&prot, 0x1303fc00u, 0x00000200u, MM_PROT_PERM_READ, MM_NONSECURE);
            }
            if (cpu_name != 0 && strcmp(cpu_name, "lpc55s69") == 0) {
                /* LPC55S69 ROM is at 0x03000000 (NS alias) / 0x13000000 (S alias).
                 * The hardware IDAU hardwires this range as NS-accessible.
                 * Register both S and NS attributions so S and NS code can access
                 * the ROM API bootloader tree and stubs. */
                mm_prot_add_region(&prot, 0x03000000u, 0x00020000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot, 0x03000000u, 0x00020000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
                mm_prot_add_region(&prot, 0x13000000u, 0x00020000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot, 0x13000000u, 0x00020000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
            }
            if (cpu_name != 0 && strcmp(cpu_name, "rw612") == 0) {
                /* RW612 ROM stub region (S + NS aliases) and ROM API tree. */
                mm_prot_add_region(&prot, 0x03001000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot, 0x03001000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
                mm_prot_add_region(&prot, 0x13001000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot, 0x13001000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
                mm_prot_add_region(&prot, 0x0301F000u, 0x00000100u, MM_PROT_PERM_READ, MM_SECURE);
                mm_prot_add_region(&prot, 0x0301F000u, 0x00000100u, MM_PROT_PERM_READ, MM_NONSECURE);
                mm_prot_add_region(&prot, 0x1301F000u, 0x00000100u, MM_PROT_PERM_READ, MM_SECURE);
                mm_prot_add_region(&prot, 0x1301F000u, 0x00000100u, MM_PROT_PERM_READ, MM_NONSECURE);
                /* PKA scratch RAM at 0x22040000 (also 0x32040000 secure alias). */
                mm_prot_add_region(&prot, 0x22040000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                mm_prot_add_region(&prot, 0x22040000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
                mm_prot_add_region(&prot, 0x32040000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                mm_prot_add_region(&prot, 0x32040000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
            }
            if (cpu_name != 0 && strcmp(cpu_name, "pic32ck") == 0) {
                /* PIC32CK Boot ROM: 0x08000000, 128 KB */
                mm_prot_add_region(&prot, 0x08000000u, 0x00020000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot, 0x08000000u, 0x00020000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
                /* PIC32CK CFM (Configuration Flash Memory): 0x0A000000, 64 KB */
                mm_prot_add_region(&prot, 0x0A000000u, 0x00010000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                mm_prot_add_region(&prot, 0x0A000000u, 0x00010000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
            }
            if (cpu_name != 0 &&
                (strcmp(cpu_name, "stm32h563") == 0 || strcmp(cpu_name, "stm32h533") == 0 ||
                 strcmp(cpu_name, "stm32h5f4") == 0)) {
                mm_prot_add_region(&prot, 0x0CFFF000u, 0x800u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                mm_prot_add_region(&prot, 0x08FFF000u, 0x800u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
                mm_prot_add_region(&prot, 0x0CFFF800u, 0x10u, MM_PROT_PERM_READ, MM_SECURE);
                mm_prot_add_region(&prot, 0x08FFF800u, 0x10u, MM_PROT_PERM_READ, MM_NONSECURE);
            }
            if (cpu_name != 0 &&
                (strcmp(cpu_name, "stm32u585") == 0 || strcmp(cpu_name, "stm32l552") == 0)) {
                /* STM32U5/L5 expose vendor PPB blocks such as DBGMCU at 0xE004xxxx. */
                mm_prot_add_region(&prot, 0xE0040000u, 0x00010000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                mm_prot_add_region(&prot, 0xE0040000u, 0x00010000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
            }
            if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                mm_prot_add_region(&prot, 0x00000000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot, 0x00000000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
            }
            if (cpu_name != 0 && strcmp(cpu_name, "nrf5340") == 0) {
                mm_prot_add_region(&prot, 0x00000000u, cfg.flash_size_ns,
                                   MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot, 0x00000000u, cfg.flash_size_ns,
                                   MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
            }
            if (cfg.ram_regions != 0 && cfg.ram_region_count > 0u) {
                mm_u32 ri;
                for (ri = 0; ri < cfg.ram_region_count; ++ri) {
                    const struct mm_ram_region *r = &cfg.ram_regions[ri];
                    mm_prot_add_region(&prot, r->base_s, r->size, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
                    mm_prot_add_region(&prot, r->base_ns, r->size, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
                }
            } else {
                mm_prot_add_region(&prot, cfg.ram_base_s, cfg.ram_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot, cfg.ram_base_ns, cfg.ram_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
            }
            /* Permit peripheral space (AHB/APB) for now; SAU still controls Secure vs Non-secure visibility. */
            mm_prot_add_region(&prot, 0x40000000u, 0x20000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
            mm_prot_add_region(&prot, 0x40000000u, 0x20000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
            /* Permit SIO/system bus window (e.g. RP2350 SIO at 0xD0000000). */
            mm_prot_add_region(&prot, 0xD0000000u, 0x10000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
            mm_prot_add_region(&prot, 0xD0000000u, 0x10000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
            if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                mm_prot_add_region(&prot, 0x18000000u, 0x04000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                mm_prot_add_region(&prot, 0x18000000u, 0x04000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
            }
            if (cfg.core_count > 1u) {
                mm_prot_add_region(&prot1, cfg.flash_base_s, cfg.flash_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
                mm_prot_add_region(&prot1, cfg.flash_base_ns, cfg.flash_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
                if (cpu_name != 0 && strcmp(cpu_name, "mcxn947") == 0) {
                    mm_prot_add_region(&prot1, cfg.flash_base_ns, cfg.flash_size_ns,
                                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
                    mm_prot_add_region(&prot1, 0x1303fc00u, 0x00000200u, MM_PROT_PERM_READ, MM_SECURE);
                    mm_prot_add_region(&prot1, 0x1303fc00u, 0x00000200u, MM_PROT_PERM_READ, MM_NONSECURE);
                }
                if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                    mm_prot_add_region(&prot1, 0x00000000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_SECURE);
                    mm_prot_add_region(&prot1, 0x00000000u, 0x00001000u, MM_PROT_PERM_READ | MM_PROT_PERM_EXEC, MM_NONSECURE);
                }
                if (cpu_name != 0 &&
                    (strcmp(cpu_name, "stm32u585") == 0 || strcmp(cpu_name, "stm32l552") == 0)) {
                    mm_prot_add_region(&prot1, 0xE0040000u, 0x00010000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                    mm_prot_add_region(&prot1, 0xE0040000u, 0x00010000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
                }
                if (cpu_name != 0 && strcmp(cpu_name, "nrf5340") == 0) {
                    mm_prot_add_region(&prot1, 0x00000000u, cfg.flash_size_ns,
                                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
                    mm_prot_add_region(&prot1, 0x00000000u, cfg.flash_size_ns,
                                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
                }
                if (cfg.ram_regions != 0 && cfg.ram_region_count > 0u) {
                    mm_u32 ri;
                    for (ri = 0; ri < cfg.ram_region_count; ++ri) {
                        const struct mm_ram_region *r = &cfg.ram_regions[ri];
                        mm_prot_add_region(&prot1, r->base_s, r->size, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
                        mm_prot_add_region(&prot1, r->base_ns, r->size, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
                    }
                } else {
                    mm_prot_add_region(&prot1, cfg.ram_base_s, cfg.ram_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
                    mm_prot_add_region(&prot1, cfg.ram_base_ns, cfg.ram_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
                }
                mm_prot_add_region(&prot1, 0x40000000u, 0x20000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                mm_prot_add_region(&prot1, 0x40000000u, 0x20000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
                mm_prot_add_region(&prot1, 0xD0000000u, 0x10000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                mm_prot_add_region(&prot1, 0xD0000000u, 0x10000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
                if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                    mm_prot_add_region(&prot1, 0x18000000u, 0x04000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
                    mm_prot_add_region(&prot1, 0x18000000u, 0x04000000u, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
                }
            }
            mm_spiflash_register_prot_regions(&prot);

            mm_nvic_init(&nvic);
            if (cfg.core_count > 1u) {
                mm_nvic_init(&nvic1);
            }
            g_cpu0 = &cpu;
            g_nvic0 = &nvic;
            if (cfg.core_count > 1u) {
                g_cpu1 = &cpu1;
                g_nvic1 = &nvic1;
            }
            if (force_ns_boot) {
                mm_u32 irq;
                for (irq = 0; irq < MM_MAX_IRQ; ++irq) {
                    mm_nvic_set_itns(&nvic, irq, MM_TRUE);
                }
                if (cfg.core_count > 1u) {
                    for (irq = 0; irq < MM_MAX_IRQ; ++irq) {
                        mm_nvic_set_itns(&nvic1, irq, MM_TRUE);
                    }
                }
            }
            if (cfg.core_count > 1u && cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                mm_rp2350_bind_multicore(&cpu, &cpu1, &nvic, &nvic1, &active_core);
            }

            /* Reset CPU state */
            {
                int i;
                for (i = 0; i < 16; ++i) cpu.r[i] = 0;
                for (i = 0; i < 32; ++i) cpu.s[i] = 0;
                cpu.xpsr = 0;
                cpu.fpscr = 0;
                cpu.fp_active = MM_FALSE;
                cpu.sec_state = MM_SECURE;
                cpu.mode = MM_THREAD;
                cpu.priv_s = MM_FALSE;
                cpu.priv_ns = MM_FALSE;
                cpu.control_s = cpu.control_ns = 0;
                cpu.primask_s = cpu.primask_ns = 0;
                cpu.basepri_s = cpu.basepri_ns = 0;
                cpu.faultmask_s = cpu.faultmask_ns = 0;
                cpu.msp_s = cpu.msp_ns = 0;
                cpu.psp_s = cpu.psp_ns = 0;
                cpu.vtor_s = boot_base_s;
                cpu.vtor_ns = boot_base_ns;
                cpu.exc_depth = 0;
                cpu.tz_depth = 0;
                cpu.sleeping = MM_FALSE;
                cpu.sleep_wfe = MM_FALSE;
                cpu.event_reg = MM_FALSE;
                apply_target_boot_seed_regs(&cpu, cpu_name);
                if (cfg.core_count > 1u) {
                    for (i = 0; i < 16; ++i) cpu1.r[i] = 0;
                    for (i = 0; i < 32; ++i) cpu1.s[i] = 0;
                    cpu1.xpsr = 0;
                    cpu1.fpscr = 0;
                    cpu1.fp_active = MM_FALSE;
                    cpu1.sec_state = MM_SECURE;
                    cpu1.mode = MM_THREAD;
                    cpu1.priv_s = MM_FALSE;
                    cpu1.priv_ns = MM_FALSE;
                    cpu1.control_s = cpu1.control_ns = 0;
                    cpu1.primask_s = cpu1.primask_ns = 0;
                    cpu1.basepri_s = cpu1.basepri_ns = 0;
                    cpu1.faultmask_s = cpu1.faultmask_ns = 0;
                    cpu1.msp_s = cpu1.msp_ns = 0;
                    cpu1.psp_s = cpu1.psp_ns = 0;
                    cpu1.vtor_s = boot_base_s;
                    cpu1.vtor_ns = boot_base_ns;
                    cpu1.exc_depth = 0;
                    cpu1.tz_depth = 0;
                    cpu1.sleeping = MM_TRUE;
                    cpu1.sleep_wfe = MM_TRUE;
                    cpu1.event_reg = MM_FALSE;
                    apply_target_boot_seed_regs(&cpu1, cpu_name);
                    it_pattern1 = 0;
                    it_remaining1 = 0;
                    it_cond1 = 0;
                }
            }

        {
            mm_u32 cpacr_s = scs.cpacr_s;
            mm_u32 cpacr_ns = scs.cpacr_ns;
            mm_u32 nsacr = scs.nsacr;
            mm_bool s_en = ((cpacr_s >> 20) & 0x3u) != 0u && ((cpacr_s >> 22) & 0x3u) != 0u;
            mm_bool ns_en = ((cpacr_ns >> 20) & 0x3u) != 0u && ((cpacr_ns >> 22) & 0x3u) != 0u;
            mm_bool nsacr_en = ((nsacr >> 10) & 0x1u) != 0u && ((nsacr >> 11) & 0x1u) != 0u;
            fprintf(stderr, "[FPU] CPACR_S=%s CPACR_NS=%s NSACR=%s\n",
                    s_en ? "Enabled" : "Disabled",
                    ns_en ? "Enabled" : "Disabled",
                    nsacr_en ? "Enabled" : "Disabled");
            }

        {
            enum mm_sec_state boot_sec = force_ns_boot ? MM_NONSECURE : MM_SECURE;
            active_core = 0;
                g_active_prot_ctx = &prot;
                if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                    mm_rp2350_set_active_core(0);
                }
                if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                    scs.sau_ctrl = 0x1u | 0x2u;
                } else if (force_ns_boot) {
                    scs.sau_ctrl = 0x1u | 0x2u;
                }
                if (!mm_vector_apply_reset(&cpu, &map, boot_sec)) {
                    fprintf(stderr, "failed to apply reset\n");
                    rc = 1;
                    goto cleanup;
                }
            }
            /* Keep SCS VTOR banks in sync with CPU reset values so exception dispatch uses VTOR_S/VTOR_NS. */
            scs.vtor_s = cpu.vtor_s;
            scs.vtor_ns = cpu.vtor_ns;
            if (cfg.core_count > 1u) {
                if (cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                    scs1.sau_ctrl = 0x1u | 0x2u;
                }
                scs1.vtor_s = cpu1.vtor_s;
                scs1.vtor_ns = cpu1.vtor_ns;
            }

            if (opt_gdb) {
                mm_gdb_stub_notify_stop(&gdb, 5);
            }

            if (first_start) {
                if (eth_backend != MM_ETH_BACKEND_NONE) {
                    if (!mm_eth_backend_config(eth_backend, eth_spec)) {
                        fprintf(stderr, "invalid ethernet backend spec\n");
                        rc = 1;
                        goto cleanup;
                    }
                    if (!mm_eth_backend_start()) {
                        fprintf(stderr, "failed to start ethernet backend\n");
                        rc = 1;
                        goto cleanup;
                    }
                }
                if (opt_usb) {
                    if (!mm_usbdev_start(usb_udc)) {
                        fprintf(stderr, "failed to start USB gadget backend\n");
                        rc = 1;
                        goto cleanup;
                    }
                }
                printf("Initial SP=0x%08lx PC=0x%08lx\n", (unsigned long)mm_cpu_get_active_sp(&cpu), (unsigned long)cpu.r[15]);
                printf("VTOR_S=0x%08lx VTOR_NS=0x%08lx\n", (unsigned long)cpu.vtor_s, (unsigned long)cpu.vtor_ns);
                first_start = MM_FALSE;
            } else {
                printf("[RESET] System reset requested, reinitialising core\n");
            }

            cpu.r[14] = 0xFFFFFFFFu; /* Initial LR */
            last_running = target_should_run(opt_gdb, &gdb, tui_paused, tui_step);

            /* Main loop */
            while (!done) {
                int pend_irq;
                mm_bool running_now;
                hz_now = mm_target_cpu_hz(&cfg);
                if (hz_now != 0 && hz_now != last_hz) {
                    cpu_hz = hz_now;
                    last_hz = hz_now;
                    sync_granularity = cpu_hz / 100000u;
                    if (sync_granularity == 0) sync_granularity = 1u;
                    printf("[CLOCK] CPU %llu Hz\n", (unsigned long long)cpu_hz);
                }
                if (g_fault_pending) {
                    done = MM_TRUE;
                    break;
                }
                if (mm_system_reset_pending() && allow_system_reset(&cfg, cpu_name)) {
                    reset_again = MM_TRUE;
                    mm_system_clear_reset();
                    break;
                }
                if (cfg.core_count > 1u && cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0) {
                    mm_u32 launch_vtor = 0;
                    mm_u32 launch_sp = 0;
                    mm_u32 launch_entry = 0;
                    if (mm_rp2350_core1_take_launch(&launch_vtor, &launch_sp, &launch_entry)) {
                        int r;
                        for (r = 0; r < 16; ++r) cpu1.r[r] = 0;
                        cpu1.xpsr = 0x01000000u;
                        cpu1.sec_state = MM_SECURE;
                        cpu1.mode = MM_THREAD;
                        cpu1.priv_s = MM_FALSE;
                        cpu1.priv_ns = MM_FALSE;
                        cpu1.control_s = cpu1.control_ns = 0;
                        cpu1.primask_s = cpu1.primask_ns = 0;
                        cpu1.basepri_s = cpu1.basepri_ns = 0;
                        cpu1.faultmask_s = cpu1.faultmask_ns = 0;
                        cpu1.msp_s = launch_sp;
                        cpu1.msp_ns = 0;
                        cpu1.psp_s = 0;
                        cpu1.psp_ns = 0;
                        cpu1.vtor_s = launch_vtor;
                        cpu1.vtor_ns = launch_vtor;
                        cpu1.r[13] = launch_sp;
                        cpu1.r[14] = 0xFFFFFFFFu;
                        cpu1.r[15] = launch_entry | 1u;
                        cpu1.exc_depth = 0;
                        cpu1.tz_depth = 0;
                        cpu1.sleeping = MM_FALSE;
                        cpu1.sleep_wfe = MM_FALSE;
                        cpu1.event_reg = MM_FALSE;
                        scs1.vtor_s = launch_vtor;
                        scs1.vtor_ns = launch_vtor;
                        it_pattern1 = 0;
                        it_remaining1 = 0;
                        it_cond1 = 0;
                    }
                }

                if (opt_gdb) {
                    if (mm_gdb_stub_poll(&gdb, 0)) {
                        mm_gdb_stub_handle(&gdb, &cpu, &map);
                    }
                    if (!gdb.connected && gdb.listen_fd >= 0) {
                        if (mm_gdb_stub_wait_client(&gdb)) {
                            const char *exec_path = (gdb_symbols != 0) ? gdb_symbols : images[0].path;
                            mm_gdb_stub_set_exec_path(&gdb, exec_path);
                        }
                    }
                    if (mm_gdb_stub_take_reset(&gdb)) {
                        apply_reset_view(opt_tui ? &tui : 0, &cpu, &map, cycle_total,
                                         opt_tui ? &tui_steps_offset : 0,
                                         opt_tui ? &tui_steps_latched : 0);
                    }
                    if (mm_gdb_stub_take_quit(&gdb)) {
                        done = MM_TRUE;
                        continue;
                    }
                    if (!gdb.alive) {
                        gdb.alive = MM_TRUE;
                    }
                    if (gdb.to_interrupt) {
                        mm_gdb_stub_notify_stop(&gdb, 2);
                        gdb.to_interrupt = MM_FALSE;
                        printf("[GDB] Interrupt handled\n");
                    }
                }
                if (mm_uart_break_on_macro_take()) {
                    printf("[UART] macro error breakpoint hit\n");
                    if (opt_gdb) {
                        gdb.running = MM_FALSE;
                        mm_gdb_stub_notify_stop(&gdb, 5);
                    }
                }

                if (opt_tui) {
                    update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                             &tui_steps_offset, &tui_steps_latched);
                    if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, &cpu, &scs, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                        done = MM_TRUE;
                        continue;
                    }
                }

                running_now = target_should_run(opt_gdb, &gdb, tui_paused, tui_step);
                if (running_now != last_running) {
                    mm_u64 steps_now = 0;
                    if (cycle_total >= tui_steps_offset) {
                        steps_now = cycle_total - tui_steps_offset;
                    }
                    printf("[EMULATION] %s steps=%llu\n",
                           running_now ? "Start" : "Stop",
                           (unsigned long long)steps_now);
                    if (running_now) {
                        mm_u64 now_ns = host_now_ns();
                        if (cpu_hz != 0) {
                            long double vns = ((long double)vcycles * (long double)NS_PER_SEC) / (long double)cpu_hz;
                            mm_u64 vns_u = (vns < 0.0L) ? 0u : (mm_u64)vns;
                            host0_ns = (now_ns > vns_u) ? (now_ns - vns_u) : 0u;
                        } else {
                            host0_ns = now_ns;
                        }
                        vcycles_last_sync = vcycles;
                    }
                    last_running = running_now;
                }

                if (running_now && cfg.core_count > 1u && cpu_name != 0 && strcmp(cpu_name, "rp2350") == 0 &&
                    mm_rp2350_core1_running()) {
                    const mm_u32 core1_epoch_steps = 64u;
                    mm_u32 core1_step;
                    active_core = 1u;
                    g_active_prot_ctx = &prot1;
                    mm_rp2350_set_active_core(1u);
                    for (core1_step = 0; core1_step < core1_epoch_steps; ++core1_step) {
                        if (!mm_rp2350_core1_running()) {
                            break;
                        }
                        if (opt_gdb) {
                            mm_u8 i;
                            fault_clock_count = gdb.fault_clock_count;
                            if (fault_clock_count > (mm_u8)(sizeof(fault_clocks) / sizeof(fault_clocks[0]))) {
                                fault_clock_count = (mm_u8)(sizeof(fault_clocks) / sizeof(fault_clocks[0]));
                            }
                            for (i = 0; i < fault_clock_count; ++i) {
                                fault_clocks[i] = gdb.fault_clocks[i];
                            }
                        }
                        if (!step_core_simple(&cpu1, &scs1, &nvic1, &map, &cfg, fault_clocks, fault_clock_count,
                                              &cycle_total, &vcycles, &cycles_since_poll,
                                              &it_pattern1, &it_remaining1, &it_cond1, &done)) {
                            break;
                        }
                        if (done) {
                            break;
                        }
                    }
                    active_core = 0u;
                    g_active_prot_ctx = &prot;
                    mm_rp2350_set_active_core(0u);
                    if (done) {
                        continue;
                    }
                }

                if (!running_now) {
                    host_sync_if_needed(vcycles, &vcycles_last_sync, host0_ns, sync_granularity, cpu_hz);
                    mm_target_usart_poll(&cfg);
                    mm_target_spi_poll(&cfg);
                    mm_target_eth_poll(&cfg);
                    rp2350_usb_sync_vector_ready(&scs, &map);
                    mm_usbdev_set_paused(cpu.primask_ns != 0u ? MM_TRUE : MM_FALSE);
                    mm_usbdev_poll();
                    update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                             &tui_steps_offset, &tui_steps_latched);
                    if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, &cpu, &scs, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                        done = MM_TRUE;
                        continue;
                    }
                    {
                        struct timespec req;
                        req.tv_sec = 0;
                        req.tv_nsec = (long)IDLE_SLEEP_NS;
                        nanosleep(&req, 0);
                    }
                    if (mm_system_reset_pending() && allow_system_reset(&cfg, cpu_name)) {
                        reset_again = MM_TRUE;
                        mm_system_clear_reset();
                        break;
                    }
                    continue;
                }

                if (opt_gdb) {
                    if (mm_gdb_stub_breakpoint_hit(&gdb, cpu.r[15] | 1u)) {
                        mm_gdb_stub_notify_stop(&gdb, 5);
                        continue;
                    }
                }

                if (!opt_gdb && reload_pending && tui_paused) {
                    if (reload_images(images, image_count, &targets, &cfg, boot_mode, cpu_name, &loaded_total, &loaded_max_end)) {
                        if (opt_persist) {
                            const char *paths[16];
                            mm_u32 offsets[16];
                            int k;
                            int persist_count = 0;
                            for (k = 0; k < image_count; ++k) {
                                if (images[k].type != MM_IMAGE_BIN) {
                                    continue;
                                }
                                paths[persist_count] = images[k].path;
                                offsets[persist_count] = images[k].offset;
                                persist_count++;
                            }
                            if (persist_count > 0) {
                                mm_flash_persist_build(&persist, flash, cfg.flash_size_s, paths, offsets, persist_count);
                            }
                        }
                        build_symbol_db(images, image_count, gdb_symbols_list, gdb_symbols_count);
                        mm_system_request_reset();
                    }
                    reload_pending = MM_FALSE;
                    continue;
                }

                {
                    enum mm_sec_state systick_sec = MM_SECURE;

                /* If CPU is sleeping (WFI/WFE), stay in low-power loop until an event or pending exception arrives. */
                if (cpu.sleeping) {
                    mm_bool wake = MM_FALSE;
                    mm_bool stopped = MM_FALSE;
                    stopped = !target_should_run(opt_gdb, &gdb, tui_paused, tui_step);
                    if (stopped) {
                        struct timespec req;
                        req.tv_sec = 0;
                        req.tv_nsec = (long)IDLE_SLEEP_NS;
                        nanosleep(&req, 0);
                        continue;
                    }
                    /* Flush accumulated cycles into virtual time before idling. */
                    host_sync_if_needed(vcycles, &vcycles_last_sync, host0_ns, sync_granularity, cpu_hz);
                    /* Wake on event register or any pending enabled exception. */
                    if (cpu.event_reg) {
                        wake = MM_TRUE;
                    } else if (systick_pending_select(&cpu, &scs, &systick_sec) || scs.pend_sv) {
                        wake = MM_TRUE;
                    } else if (mm_nvic_select_ex(&nvic, &cpu, &scs) >= 0) {
                        wake = MM_TRUE;
                    } else if (cpu.sleep_wfe && mm_nvic_any_pending(&nvic)) {
                        if (getenv("M33MU_SLEEP_TRACE")) {
                            printf("[SLEEP_WAKE] wfe pending irq (primask_s=%lu primask_ns=%lu)\n",
                                   (unsigned long)cpu.primask_s,
                                   (unsigned long)cpu.primask_ns);
                        }
                        wake = MM_TRUE;
                    }
                    if (wake) {
                        cpu.sleeping = MM_FALSE;
                        cpu.sleep_wfe = MM_FALSE;
                        cpu.event_reg = MM_FALSE;
                    } else {
                        mm_u64 delta = mm_scs_systick_cycles_until_fire(&scs);
                        if (delta == (mm_u64)-1) {
                            mm_u64 idle_cycles = 0;
                            struct timespec req;
                            req.tv_sec = 0;
                            req.tv_nsec = (long)IDLE_SLEEP_NS;
                            nanosleep(&req, 0);
                            if (cpu_hz != 0) {
                                idle_cycles = (mm_u64)(((long double)cpu_hz * (long double)IDLE_SLEEP_NS) / (long double)NS_PER_SEC);
                            }
                            if (idle_cycles == 0) {
                                idle_cycles = 1;
                            }
                            mm_scs_systick_advance(&scs, idle_cycles);
                            mm_timer_tick(&cfg, idle_cycles);
                            vcycles += idle_cycles;
                            cycle_total += idle_cycles;
                            cycles_since_poll += idle_cycles;
                            mm_target_usart_poll(&cfg);
                            mm_target_spi_poll(&cfg);
                            mm_target_eth_poll(&cfg);
                            rp2350_usb_sync_vector_ready(&scs, &map);
                            mm_usbdev_set_paused(cpu.primask_ns != 0u ? MM_TRUE : MM_FALSE);
                            mm_usbdev_poll();
                            update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                                     &tui_steps_offset, &tui_steps_latched);
                            if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, &cpu, &scs, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                                done = MM_TRUE;
                                continue;
                            }
                            if (systick_pending_select(&cpu, &scs, &systick_sec) || scs.pend_sv || mm_nvic_select_ex(&nvic, &cpu, &scs) >= 0 ||
                                (cpu.sleep_wfe && mm_nvic_any_pending(&nvic))) {
                                cpu.sleeping = MM_FALSE;
                                cpu.sleep_wfe = MM_FALSE;
                                cpu.event_reg = MM_FALSE;
                                goto handle_pending;
                            }
                            if (mm_system_reset_pending() && allow_system_reset(&cfg, cpu_name)) {
                                reset_again = MM_TRUE;
                                mm_system_clear_reset();
                                break;
                            }
                        } else {
                            mm_scs_systick_advance(&scs, delta);
                            mm_timer_tick(&cfg, delta);
                            vcycles += delta;
                            cycle_total += delta;
                            cycles_since_poll += delta;
                            if (systick_pending_select(&cpu, &scs, &systick_sec) || scs.pend_sv) {
                                cpu.sleeping = MM_FALSE;
                                cpu.sleep_wfe = MM_FALSE;
                                cpu.event_reg = MM_FALSE;
                            }
                            host_sync_if_needed(vcycles, &vcycles_last_sync, host0_ns, sync_granularity, cpu_hz);
                            mm_target_usart_poll(&cfg);
                            mm_target_spi_poll(&cfg);
                            mm_target_eth_poll(&cfg);
                            rp2350_usb_sync_vector_ready(&scs, &map);
                            mm_usbdev_set_paused(cpu.primask_ns != 0u ? MM_TRUE : MM_FALSE);
                            mm_usbdev_poll();
                            update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                                     &tui_steps_offset, &tui_steps_latched);
                            if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, &cpu, &scs, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                                done = MM_TRUE;
                                continue;
                            }
                            cycles_since_poll = 0;
                            if (mm_system_reset_pending() && allow_system_reset(&cfg, cpu_name)) {
                                reset_again = MM_TRUE;
                                mm_system_clear_reset();
                                break;
                            }
                        }
                        if (!cpu.sleeping) {
                            goto handle_pending;
                        }
                        continue;
                    }
                    if (!cpu.sleeping) {
                        goto handle_pending;
                    }
                    continue;
                }

                /* Handle pending system exceptions (SysTick, PendSV). */
handle_pending:
                if (scs.pend_sv &&
                    !primask_blocks_current(&cpu) &&
                    !faultmask_blocks_current(&cpu) &&
                    !basepri_blocks_system_exc(&cpu, &scs, MM_VECT_PENDSV)) {
                    mm_u8 current_preempt = 0u;
                    mm_u8 current_raw = 0u;
                    mm_u8 pend_prio = system_exc_priority(&cpu, &scs, MM_VECT_PENDSV);
                    mm_u8 pend_preempt = preempt_priority_value(pend_prio, aircr_prigroup_for_sec(&scs, cpu.sec_state));
                    if (current_execution_priority(&cpu, &scs, &nvic, &current_preempt, &current_raw) &&
                        pend_preempt >= current_preempt) {
                        goto check_systick_pending;
                    }
                    if (!enter_exception(&cpu, &map, &scs, MM_VECT_PENDSV, cpu.r[15] & ~1u, cpu.xpsr)) {
                        done = MM_TRUE;
                    } else {
                        itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                    }
                    continue;
                }
check_systick_pending:
                if (systick_pending_select(&cpu, &scs, &systick_sec) &&
                    !primask_blocks_for_sec(&cpu, systick_sec) &&
                    !faultmask_blocks_for_sec(&cpu, systick_sec) &&
                    !basepri_blocks_system_exc_for_sec(&cpu, &scs, systick_sec, MM_VECT_SYSTICK)) {
                    mm_u8 current_preempt = 0u;
                    mm_u8 current_raw = 0u;
                    mm_u8 pend_prio = system_exc_priority_for_sec(&scs, systick_sec, MM_VECT_SYSTICK);
                    mm_u8 pend_preempt = preempt_priority_value(pend_prio, aircr_prigroup_for_sec(&scs, systick_sec));
                    if (current_execution_priority(&cpu, &scs, &nvic, &current_preempt, &current_raw) &&
                        pend_preempt >= current_preempt) {
                        goto check_irq_pending;
                    }
                    if (!enter_exception_ex(&cpu, &map, &scs, MM_VECT_SYSTICK, cpu.r[15] & ~1u, cpu.xpsr, systick_sec)) {
                        done = MM_TRUE;
                    } else {
                        itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                    }
                    continue;
                }

                /* Manage interrupts */
check_irq_pending:
                {
                    enum mm_sec_state irq_sec = MM_SECURE;
                    pend_irq = mm_nvic_select_routed_ex(&nvic, &cpu, &scs, &irq_sec);
                    if (pend_irq >= 0) {
                        mm_u32 exc_num = 16u + (mm_u32)pend_irq;
                        /* Clear pending when accepted. */
                        mm_nvic_set_pending(&nvic, (mm_u32)pend_irq, MM_FALSE);
                        mm_nvic_set_active(&nvic, (mm_u32)pend_irq, MM_TRUE);
                        if (!enter_exception_ex(&cpu, &map, &scs, exc_num, cpu.r[15] & ~1u, cpu.xpsr, irq_sec)) {
                            done = MM_TRUE;
                        } else {
                            itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                        }
                        continue;
                    }
                }
                }

                if (opt_gdb && mm_gdb_stub_is_reverse(&gdb) && mm_gdb_stub_should_run(&gdb)) {
                    if (!mm_trace_undo_step(&cpu, &map)) {
                        mm_gdb_stub_notify_stop(&gdb, 5);
                        continue;
                    }
                    if (cycle_total > 0u) {
                        cycle_total--;
                    }
                    if (vcycles > 0u) {
                        vcycles--;
                    }
                    if (mm_gdb_stub_should_step(&gdb) || mm_gdb_stub_breakpoint_hit(&gdb, cpu.r[15])) {
                        mm_gdb_stub_notify_stop(&gdb, 5);
                    }
                    continue;
                }

                /* Fast path: translation block cache */
                {
                    mm_bool fast_mode = MM_TRUE;
                    mm_bool fast_fpu_ok = MM_TRUE;
                    mm_u32 tb_chain_steps = 0;
                    const mm_u32 tb_chain_limit = 512u;
                    struct mm_execute_ctx exec_ctx;
                    struct mm_tb *next_tb = 0;
                    struct mm_tb *candidate_tb = 0;
                    mm_u32 pc_after = 0;
                    mm_bool tb_bkpt_hit = MM_FALSE;
                    mm_u32 tb_bkpt_imm = 0;
                    mm_u32 ops_executed = 0;
                    mm_u32 target_idx = 0;
                    if (opt_gdb || opt_capstone || mm_trace_enabled() || g_call_trace) {
                        fast_mode = MM_FALSE;
                    }
                    if (it_remaining != 0u) {
                        fast_mode = MM_FALSE;
                    }
                    if (opt_disable_tb) {
                        fast_mode = MM_FALSE;
                    }
                    if (fast_mode && !fpu_access_allowed(&cpu, &scs)) {
                        fast_fpu_ok = MM_FALSE;
                    }
                    code_cache.fpu_ok = fast_fpu_ok;
                    if (fast_mode) {
                    struct mm_tb *tb = mm_tb_lookup(&code_cache, cpu.r[15] & ~1u, cpu.sec_state);
                        if (tb == 0) {
                            tb = mm_tb_build(&code_cache, &cpu, &map, &scs, cpu.r[15] & ~1u, cpu.sec_state, opt_gdb);
                        }
                        if (tb != 0) {
                            if (tb->fpu_ok != fast_fpu_ok) {
                                tb = 0;
                            }
                        }
                        exec_ctx.cpu = &cpu;
                        exec_ctx.map = &map;
                        exec_ctx.scs = &scs;
                        exec_ctx.nvic = &nvic;
                        exec_ctx.gdb = &gdb;
                        exec_ctx.fetch = 0;
                        exec_ctx.dec = 0;
                        exec_ctx.opt_dump = opt_dump;
                        exec_ctx.opt_gdb = opt_gdb;
                        exec_ctx.opt_expect_bkpt = opt_expect_bkpt;
                        exec_ctx.expect_bkpt = expect_bkpt;
                        exec_ctx.it_pattern = &it_pattern;
                        exec_ctx.it_remaining = &it_remaining;
                        exec_ctx.it_cond = &it_cond;
                        exec_ctx.done = &done;
                        exec_ctx.bkpt_hit = &tb_bkpt_hit;
                        exec_ctx.bkpt_imm = &tb_bkpt_imm;
                        exec_ctx.handle_pc_write = handle_pc_write;
                        exec_ctx.raise_mem_fault = raise_mem_fault;
                        exec_ctx.raise_usage_fault = raise_usage_fault;
                        exec_ctx.exc_return_unstack = exc_return_unstack;
                        exec_ctx.enter_exception = enter_exception;
                        while (tb != 0) {
                            if (tb_chain_steps++ >= tb_chain_limit) {
                                break;
                            }
                            next_tb = 0;
                            candidate_tb = 0;
                            pc_after = 0;
                            tb_bkpt_hit = MM_FALSE;
                            tb_bkpt_imm = 0;
                            ops_executed = 0;

                            cpu.r[13] = mm_cpu_get_active_sp(&cpu);

                            (void)mm_tb_run(tb, &exec_ctx, &done, &tb_bkpt_hit, &tb_bkpt_imm, &ops_executed);
                            if (ops_executed != 0u) {
                                cycles_since_poll += ops_executed;
                                cycle_total += ops_executed;
                                vcycles += ops_executed;
                                mm_scs_systick_advance(&scs, ops_executed);
                                mm_timer_tick(&cfg, ops_executed);
                            }
                            if (tb_bkpt_hit) {
                                printf("[BKPT] imm=0x%02lx\n", (unsigned long)tb_bkpt_imm);
                                if (opt_expect_bkpt) {
                                    if (tb_bkpt_imm == expect_bkpt) {
                                        expect_bkpt_hit = MM_TRUE;
                                        printf("[EXPECT BKPT] Success\n");
                                        done = MM_TRUE;
                                        break;
                                    } else {
                                        printf("[EXPECT BKPT] Fail\n");
                                    }
                                }
                            }
                            if (done) {
                                if (opt_gdb) {
                                    mm_gdb_stub_notify_stop(&gdb, 5);
                                }
                                break;
                            }
                            if (cpu.sleeping) {
                                break;
                            }
                            if (it_remaining != 0u) {
                                break;
                            }
                            fast_fpu_ok = fpu_access_allowed(&cpu, &scs) ? MM_TRUE : MM_FALSE;
                            code_cache.fpu_ok = fast_fpu_ok;
                            pc_after = cpu.r[15];
                            if (pc_after == tb->fallthrough_pc) {
                                if (tb->fallthrough_idx < M33MU_TB_ENTRIES) {
                                    candidate_tb = mm_tb_chain_lookup(&code_cache,
                                                                      tb->fallthrough_idx,
                                                                      tb->fallthrough_gen,
                                                                      tb->end_pc,
                                                                      tb->sec);
                                    if (candidate_tb == 0) {
                                        tb->fallthrough_idx = M33MU_TB_ENTRIES;
                                        tb->fallthrough_gen = 0;
                                    }
                                }
                                if (candidate_tb == 0) {
                                    candidate_tb = mm_tb_lookup(&code_cache, tb->end_pc, tb->sec);
                                    if (candidate_tb != 0) {
                                        target_idx = (tb->end_pc >> 1u) & (M33MU_TB_ENTRIES - 1u);
                                        tb->fallthrough_idx = target_idx;
                                        tb->fallthrough_gen = code_cache.tb_cache_gen[target_idx];
                                    }
                                }
                                next_tb = candidate_tb;
                            } else if (pc_after == tb->branch_pc) {
                                if (tb->branch_idx < M33MU_TB_ENTRIES) {
                                    candidate_tb = mm_tb_chain_lookup(&code_cache,
                                                                      tb->branch_idx,
                                                                      tb->branch_gen,
                                                                      tb->branch_pc & ~1u,
                                                                      tb->sec);
                                    if (candidate_tb == 0) {
                                        tb->branch_idx = M33MU_TB_ENTRIES;
                                        tb->branch_gen = 0;
                                    }
                                }
                                if (candidate_tb == 0) {
                                    candidate_tb = mm_tb_lookup(&code_cache, tb->branch_pc & ~1u, tb->sec);
                                    if (candidate_tb != 0) {
                                        target_idx = ((tb->branch_pc & ~1u) >> 1u) & (M33MU_TB_ENTRIES - 1u);
                                        tb->branch_idx = target_idx;
                                        tb->branch_gen = code_cache.tb_cache_gen[target_idx];
                                    }
                                }
                                next_tb = candidate_tb;
                            }
                            if (next_tb != 0 && next_tb->fpu_ok != fast_fpu_ok) {
                                next_tb = 0;
                            }
                            if (next_tb == 0) {
                                break;
                            }
                            tb = next_tb;
                            continue;
                        }
                        if (tb != 0 && cycles_since_poll >= poll_granularity) {
                            mm_target_usart_poll(&cfg);
                            mm_target_spi_poll(&cfg);
                            mm_target_eth_poll(&cfg);
                            rp2350_usb_sync_vector_ready(&scs, &map);
                            mm_usbdev_set_paused(cpu.primask_ns != 0u ? MM_TRUE : MM_FALSE);
                            mm_usbdev_poll();
                            update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                                     &tui_steps_offset, &tui_steps_latched);
                            if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, &cpu, &scs, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                                done = MM_TRUE;
                                continue;
                            }
                            cycles_since_poll = 0;
                        }
                        if (tb != 0 || done || cpu.sleeping || it_remaining != 0u) {
                            continue;
                        }
                    }
                }

                /* Fetch/decode */
                {
                    struct mm_fetch_result f;
                    struct mm_decoded d;
                    mm_bool execute_it;
                    mm_u32 pc_before_exec = 0;
                    mm_bool fpu_ok = MM_TRUE;
                    const mm_u32 insn_cycles = 1u;
                    mm_bool trace_started = MM_FALSE;
                    mm_bool fault_skip = MM_FALSE;
                    (void)pc_before_exec;
                    cycles_since_poll += insn_cycles;
                    cycle_total += insn_cycles;
                    vcycles += insn_cycles;
                    mm_scs_systick_advance(&scs, insn_cycles);
                    mm_timer_tick(&cfg, insn_cycles);
                    if (opt_gdb) {
                        mm_u8 i;
                        fault_clock_count = gdb.fault_clock_count;
                        if (fault_clock_count > (mm_u8)(sizeof(fault_clocks) / sizeof(fault_clocks[0]))) {
                            fault_clock_count = (mm_u8)(sizeof(fault_clocks) / sizeof(fault_clocks[0]));
                        }
                        for (i = 0; i < fault_clock_count; ++i) {
                            fault_clocks[i] = gdb.fault_clocks[i];
                        }
                    }
                    if (fault_clock_hit(cycle_total, fault_clocks, fault_clock_count)) {
                        fault_skip = MM_TRUE;
                    }

                    /* Keep R13 consistent with the active banked SP so that instructions
                     * like LDR/STR [SP,#imm] and function prologue/epilogue sequences
                     * operate on the correct stack memory.
                     */
                    cpu.r[13] = mm_cpu_get_active_sp(&cpu);

                    if (!g_record_started && g_record_start_set &&
                        ((cpu.r[15] & ~1u) == g_record_start_pc)) {
                        mm_trace_reset();
                        g_record_started = MM_TRUE;
                        if (g_record_start_dump) {
                            dump_record_start_context(&cpu, &map);
                        }
                    }
                    if (g_record_stop_set && ((cpu.r[15] & ~1u) == g_record_stop_pc)) {
                        fprintf(stderr, "[RECORD_STOP] pc=0x%08lx\n", (unsigned long)(cpu.r[15] & ~1u));
                        dump_bytes(&map, cpu.sec_state, g_record_dump_addr, 64u, "record_r_after (r0)");
                        if (g_record_dump_count > 0u && mm_trace_enabled() && g_record_started) {
                            dump_trace_tail(&cpu, &map, g_record_dump_count);
                        }
                        g_record_stop_set = MM_FALSE;
                        g_record_started = MM_FALSE;
                    }
                    if (mm_trace_enabled() && g_record_started) {
                        mm_trace_begin_step(&cpu, cpu.r[15] & ~1u);
                        trace_started = MM_TRUE;
                    }

                    if (mm_bootapi_handle(&cpu, &map)) {
                        finish_trace_step_if_started(trace_started, &cpu, &map);
                        continue;
                    }

                    f = mm_fetch_t32_memmap(&cpu, &map, cpu.sec_state);
                    if (f.fault) {
                        if (!raise_mem_fault(&cpu, &map, &scs, cpu.r[15] & ~1u, cpu.xpsr, f.fault_addr, MM_TRUE)) {
                            printf("Fault on fetch at 0x%08lx (PC=0x%08lx SP=0x%08lx LR=0x%08lx xPSR=0x%08lx)\n",
                                    (unsigned long)f.fault_addr,
                                    (unsigned long)cpu.r[15],
                                    (unsigned long)mm_cpu_get_active_sp(&cpu),
                                    (unsigned long)cpu.r[14],
                                    (unsigned long)cpu.xpsr);
                            if (opt_gdb) {
                                mm_gdb_stub_notify_stop(&gdb, 11);
                            }
                            if (trace_started) {
                                mmio_bus_end_step(&map.mmio, mm_trace_get_undo_sink());
                                mm_trace_end_step(&cpu);
                                record_window_step(&cpu, &map);
                            }
                            break;
                        }
                        if (trace_started) {
                            mmio_bus_end_step(&map.mmio, mm_trace_get_undo_sink());
                            mm_trace_end_step(&cpu);
                            record_window_step(&cpu, &map);
                        }
                        continue;
                    }
                    if (f.len == 4u && !fpu_access_allowed(&cpu, &scs) && mm_is_vfp_insn_fast(f.insn)) {
                        fpu_ok = MM_FALSE;
                    }
                    if (!mm_icache_lookup(&code_cache, &f, cpu.sec_state, fpu_ok, &d)) {
                        d = decode_t32_fast(&f, &cpu, &scs);
                        mm_icache_store(&code_cache, &f, cpu.sec_state, fpu_ok, &d);
                    }
                    mm_memmap_set_last_pc(f.pc_fetch);
                    if (opt_capstone) {
                        mm_bool capstone_match = MM_TRUE;
                        if (opt_capstone_pc) {
                            mm_u32 pc = f.pc_fetch | 1u;
                            if (pc != capstone_pc && f.pc_fetch != capstone_pc) {
                                capstone_match = MM_FALSE;
                            }
                        }
                        if (capstone_match) {
                            if (opt_capstone_verbose) {
                                capstone_log(&f);
                            }
                            if (!capstone_cross_check(&f, &d)) {
                                rc = 1;
                                goto cleanup;
                            }
                            if (!capstone_it_check_pre(&f, &d, it_pattern, it_remaining, it_cond)) {
                                rc = 1;
                                goto cleanup;
                            }
                        }
                    }
                    if (d.undefined) {
                        if (getenv("M33MU_UNDEF_TRACE")) {
                            printf("[UNDEF] pc=0x%08lx len=%u raw=0x%08lx hw1=0x%04lx\n",
                                   (unsigned long)f.pc_fetch,
                                   (unsigned int)d.len,
                                   (unsigned long)d.raw,
                                   (unsigned long)(d.raw & 0xffffu));
                        }
                        if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) {
                            printf("Unimplemented opcode 0x%08lx at PC=0x%08lx\n", (unsigned long)d.raw, (unsigned long)(f.pc_fetch | 1u));
                            if (opt_gdb) {
                                mm_gdb_stub_notify_stop(&gdb, 4);
                            }
                            if (trace_started) {
                                mmio_bus_end_step(&map.mmio, mm_trace_get_undo_sink());
                                mm_trace_end_step(&cpu);
                                record_window_step(&cpu, &map);
                            }
                            break;
                        }
                        if (trace_started) {
                            mmio_bus_end_step(&map.mmio, mm_trace_get_undo_sink());
                            mm_trace_end_step(&cpu);
                            record_window_step(&cpu, &map);
                        }
                        continue;
                    }

                    /* ITSTATE handling: if inside IT block and not IT instruction, conditionally execute. */
                    execute_it = mm_it_should_execute(&cpu, &d, &it_pattern, &it_remaining, &it_cond);
                    if ((g_record_trace_fp != NULL || g_record_trace_live) &&
                        g_record_started && g_record_start_remaining > 0u) {
                        FILE *trace_out = g_record_trace_fp ? g_record_trace_fp : stderr;
                        fprintf(trace_out,
                                "[TRACE_LIVE] pc=0x%08lx len=%u insn=0x%08lx "
                                "r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx "
                                "r4=0x%08lx r5=0x%08lx r6=0x%08lx r7=0x%08lx "
                                "r8=0x%08lx r9=0x%08lx r10=0x%08lx r11=0x%08lx "
                                "r12=0x%08lx sp=0x%08lx lr=0x%08lx xpsr=0x%08lx exec=%u\n",
                                (unsigned long)f.pc_fetch,
                                (unsigned)d.len,
                                (unsigned long)d.raw,
                                (unsigned long)cpu.r[0], (unsigned long)cpu.r[1],
                                (unsigned long)cpu.r[2], (unsigned long)cpu.r[3],
                                (unsigned long)cpu.r[4], (unsigned long)cpu.r[5],
                                (unsigned long)cpu.r[6], (unsigned long)cpu.r[7],
                                (unsigned long)cpu.r[8], (unsigned long)cpu.r[9],
                                (unsigned long)cpu.r[10], (unsigned long)cpu.r[11],
                                (unsigned long)cpu.r[12], (unsigned long)mm_cpu_get_active_sp(&cpu),
                                (unsigned long)cpu.r[14], (unsigned long)cpu.xpsr,
                                execute_it ? 1u : 0u);
                    }

                    if (opt_dump) {
                        printf("[DUMP] PC=0x%08lx len=%u opcode=0x%08lx kind=%d r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx sp=0x%08lx {clocks:%llu}\n",
                                (unsigned long)(f.pc_fetch | 1u),
                                (unsigned)d.len,
                                (unsigned long)d.raw,
                                (int)d.kind,
                                (unsigned long)cpu.r[0],
                                (unsigned long)cpu.r[1],
                                (unsigned long)cpu.r[2],
                                (unsigned long)cpu.r[3],
                                (unsigned long)mm_cpu_get_active_sp(&cpu),
                                (unsigned long long)cycle_total);
                    }
                    if (!execute_it && d.kind != MM_OP_IT) {
                        mm_it_advance(&cpu, &d, &it_pattern, &it_remaining, &it_cond);
                        if (trace_started) {
                            mmio_bus_end_step(&map.mmio, mm_trace_get_undo_sink());
                            mm_trace_end_step(&cpu);
                            record_window_step(&cpu, &map);
                        }
                        if (opt_gdb) {
                            mm_gdb_stub_maybe_rearm(&gdb, &map, cpu.sec_state, cpu.r[15]);
                            if (mm_gdb_stub_should_step(&gdb)) {
                                mm_gdb_stub_notify_stop(&gdb, 5);
                            }
                        }
                        continue;
                    }

                    if (fault_skip) {
                        printf("[FAULT-CLOCK] skip pc=0x%08lx cycle=%llu\n",
                               (unsigned long)(f.pc_fetch | 1u),
                               (unsigned long long)cycle_total);
                        if (it_remaining > 0u && d.kind != MM_OP_IT) {
                            mm_u8 raw = itstate_get(cpu.xpsr);
                            it_pattern >>= 1;
                            it_remaining--;
                            raw = itstate_advance(raw);
                            cpu.xpsr = itstate_set(cpu.xpsr, raw);
                        }
                        if (trace_started) {
                            mmio_bus_end_step(&map.mmio, mm_trace_get_undo_sink());
                            mm_trace_end_step(&cpu);
                            record_window_step(&cpu, &map);
                        }
                        if (opt_gdb) {
                            mm_gdb_stub_maybe_rearm(&gdb, &map, cpu.sec_state, cpu.r[15]);
                            if (mm_gdb_stub_should_step(&gdb)) {
                                mm_gdb_stub_notify_stop(&gdb, 5);
                            }
                        }
                        continue;
                    }

                    {
                        struct mm_execute_ctx exec_ctx;
                        mm_bool exec_bkpt_hit = MM_FALSE;
                        mm_u32 exec_bkpt_imm = 0u;
                        calltrace_handle_decoded(&cpu, &f, &d);
                        exec_ctx.cpu = &cpu;
                        exec_ctx.map = &map;
                        exec_ctx.scs = &scs;
                        exec_ctx.nvic = &nvic;
                        exec_ctx.gdb = &gdb;
                        exec_ctx.fetch = &f;
                        exec_ctx.dec = &d;
                        exec_ctx.opt_dump = opt_dump;
                        exec_ctx.opt_gdb = opt_gdb;
                        exec_ctx.opt_expect_bkpt = opt_expect_bkpt;
                        exec_ctx.expect_bkpt = expect_bkpt;
                        exec_ctx.it_pattern = &it_pattern;
                        exec_ctx.it_remaining = &it_remaining;
                        exec_ctx.it_cond = &it_cond;
                        exec_ctx.done = &done;
                        exec_ctx.bkpt_hit = &exec_bkpt_hit;
                        exec_ctx.bkpt_imm = &exec_bkpt_imm;
                        exec_ctx.handle_pc_write = handle_pc_write;
                        exec_ctx.raise_mem_fault = raise_mem_fault;
                        exec_ctx.raise_usage_fault = raise_usage_fault;
                        exec_ctx.exc_return_unstack = exc_return_unstack;
                        exec_ctx.enter_exception = enter_exception;
                        {
                            int exec_res = mm_execute_decoded(&exec_ctx);
                            if (exec_bkpt_hit) {
                                printf("[BKPT] imm=0x%02lx\n", (unsigned long)exec_bkpt_imm);
                                if (opt_expect_bkpt) {
                                    if (exec_bkpt_imm == expect_bkpt) {
                                        expect_bkpt_hit = MM_TRUE;
                                        printf("[EXPECT BKPT] Success\n");
                                        done = MM_TRUE;
                                    } else {
                                        printf("[EXPECT BKPT] Fail\n");
                                    }
                                }
                            }
                            if (exec_res == MM_EXEC_CONTINUE) {
                                /* Skip IT-state advance when the instruction
                                 * changed PC non-sequentially (taken branch
                                 * or EXC_RETURN). For EXC_RETURN, ITSTATE was
                                 * just restored from the saved exception
                                 * frame and represents the resumed context. */
                                mm_u32 expected_next = (f.pc_fetch + f.len) | 1u;
                                if (execute_it && d.kind != MM_OP_IT &&
                                    cpu.r[15] == expected_next) {
                                    mm_it_advance(&cpu, &d, &it_pattern, &it_remaining, &it_cond);
                                }
                                if (trace_started) {
                                    mmio_bus_end_step(&map.mmio, mm_trace_get_undo_sink());
                                    mm_trace_end_step(&cpu);
                                    record_window_step(&cpu, &map);
                                }
                                if (opt_gdb) {
                                    mm_gdb_stub_maybe_rearm(&gdb, &map, cpu.sec_state, cpu.r[15]);
                                    if (mm_gdb_stub_should_step(&gdb)) {
                                        mm_gdb_stub_notify_stop(&gdb, 5);
                                    }
                                }
                                continue;
                            }
                        }
                        if (opt_tui && !opt_gdb && done && d.kind == MM_OP_BKPT) {
                            done = MM_FALSE;
                            tui_paused = MM_TRUE;
                            tui_step = MM_FALSE;
                        }
                    }
                    if (trace_started) {
                        mmio_bus_end_step(&map.mmio, mm_trace_get_undo_sink());
                        mm_trace_end_step(&cpu);
                        record_window_step(&cpu, &map);
                    }

                    if (opt_capstone) {
                        if (!capstone_it_check_post(&f, &d, it_pattern, it_remaining, it_cond)) {
                            rc = 1;
                            goto cleanup;
                        }
                    }

                    {
                        mm_u32 expected_next = (f.pc_fetch + f.len) | 1u;
                        if (cpu.r[15] == expected_next) {
                            mm_it_advance(&cpu, &d, &it_pattern, &it_remaining, &it_cond);
                        }
                    }
                }

                if (cycles_since_poll >= poll_granularity) {
                    mm_target_usart_poll(&cfg);
                    mm_target_spi_poll(&cfg);
                    mm_target_eth_poll(&cfg);
                    rp2350_usb_sync_vector_ready(&scs, &map);
                    mm_usbdev_set_paused(cpu.primask_ns != 0u ? MM_TRUE : MM_FALSE);
                    mm_usbdev_poll();
                    update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                             &tui_steps_offset, &tui_steps_latched);
                    if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, &cpu, &scs, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                        done = MM_TRUE;
                        continue;
                    }
                    cycles_since_poll = 0;
                }

                host_sync_if_needed(vcycles, &vcycles_last_sync, host0_ns, sync_granularity, cpu_hz);

                if (mm_system_reset_pending() && allow_system_reset(&cfg, cpu_name)) {
                    reset_again = MM_TRUE;
                    mm_system_clear_reset();
                    break;
                }
                if (opt_gdb) {
                    mm_gdb_stub_maybe_rearm(&gdb, &map, cpu.sec_state, cpu.r[15]);
                    if (mm_gdb_stub_should_step(&gdb)) {
                        mm_gdb_stub_notify_stop(&gdb, 5);
                        continue;
                    }
                }
                if (opt_tui && tui_step) {
                    tui_step = MM_FALSE;
                    tui_paused = MM_TRUE;
                }
            }
            if (reset_again) {
                mm_code_cache_release(&code_cache);
                continue;
            }
            if (!opt_gdb) {
                mm_u64 wraps = mm_scs_systick_wrap_count(&scs);
                double avg_cycles_per_wrap = (wraps > 0u) ? ((double)cycle_total / (double)wraps) : 0.0;
                printf("Execution stopped after %llu virtual cycles; PC=0x%08lx LR=0x%08lx\n",
                        (unsigned long long)cycle_total,
                        (unsigned long)cpu.r[15],
                        (unsigned long)cpu.r[14]);
                if (wraps > 0u) {
                    printf("SysTick wraps=%llu avg_cycles_per_wrap=%.1f\n",
                           (unsigned long long)wraps,
                           avg_cycles_per_wrap);
                }
            }
            if (opt_record_dump > 0u && mm_trace_enabled() && g_record_started) {
                dump_trace_tail(&cpu, &map, opt_record_dump);
            }
            covdump_cycles = cycle_total;
            mm_code_cache_release(&code_cache);
            break;
        }
    }

cleanup:
    if (opt_covdump != 0) {
        struct mm_covdump_info covinfo;
        char coverr[MM_COVDUMP_ERRSZ];
        const char *cov_elf = opt_covdump_elf;
        const char *reason;
        mm_bool cov_success;
        int ci;

        coverr[0] = '\0';
        cov_success = (!opt_expect_bkpt || expect_bkpt_hit) ? MM_TRUE : MM_FALSE;
        if (opt_expect_bkpt) {
            reason = expect_bkpt_hit ? "expect-bkpt-hit" : "expect-bkpt-missed";
        } else {
            reason = "stopped";
        }
        if (cov_elf == 0) {
            for (ci = 0; ci < image_count; ++ci) {
                if (images[ci].type == MM_IMAGE_ELF && images[ci].path != 0) {
                    if (cov_elf != 0) {
                        fprintf(stderr, "--covdump: several ELF images are "
                                        "loaded; use --covdump-elf\n");
                        cov_elf = 0;
                        rc = 1;
                        break;
                    }
                    cov_elf = images[ci].path;
                }
            }
        }
        if (cov_elf == 0) {
            if (rc == 0) {
                fprintf(stderr, "--covdump: no ELF image to take coverage "
                                "symbols from; use --covdump-elf\n");
                rc = 1;
            }
        } else if (opt_covdump_success_only && !cov_success) {
            printf("[COVDUMP] skipped (%s, --covdump-when success)\n", reason);
        } else if (!mm_covdump_resolve(cov_elf, &covinfo, coverr,
                                       sizeof(coverr))) {
            fprintf(stderr, "--covdump: %s\n", coverr);
            rc = 1;
        } else if (!mm_covdump_write(&map, cpu.sec_state, &covinfo, cov_elf,
                                     opt_covdump, reason, covdump_cycles,
                                     coverr, sizeof(coverr))) {
            fprintf(stderr, "--covdump: %s\n", coverr);
            rc = 1;
        }
    }
    mm_spiflash_shutdown_all();
#ifdef M33MU_HAS_LIBTPMS
    mm_tpm_tis_shutdown_all();
#endif
    mm_ta100_shutdown_all();
    mm_iotsafe_uart_shutdown_all();
#ifdef M33MU_HAS_RUST_PLUGINS
    mm_se050_shutdown_all();
    mm_atecc608_shutdown_all();
    mm_stsafe_shutdown_all();
    mm_tropic01_shutdown_all();
#endif
    mm_usbdev_stop();
    mm_eth_backend_stop();
    if (opt_capstone) {
        capstone_shutdown();
    }
    mm_gdb_stub_close(&gdb);
    if (tui_active) {
        mm_tui_register(0);
        mm_tui_stop_thread(&tui);
        mm_tui_shutdown(&tui);
    }
    if (g_record_trace_fp != NULL) {
        fclose(g_record_trace_fp);
        g_record_trace_fp = NULL;
    }
    if (opt_expect_bkpt && rc == 0) {
        rc = expect_bkpt_hit ? 0 : 1;
    }
    return rc;
}
