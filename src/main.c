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
#include "m33mu/vector.h"
#include "m33mu/scs.h"
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/capstone.h"
#include "m33mu/memmap.h"
#include "m33mu/nvic.h"
#include "m33mu/gdbstub.h"
#include "m33mu/exec_helpers.h"
#include "m33mu/execute.h"
#include "m33mu/core_sys.h"
#include "m33mu/mem_prot.h"
#include "m33mu/vector.h"
#include "m33mu/exc_return.h"
#include "m33mu/tz.h"
#include "m33mu/exception.h"
#include "m33mu/table_branch.h"
#include "m33mu/timer.h"
#include "m33mu/target_hal.h"
#include "m33mu/spiflash.h"
#include "m33mu/usbdev.h"
#ifdef M33MU_HAS_LIBTPMS
#include "m33mu/tpm_tis.h"
#endif
#include "tui.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

#define CCR_DIV_0_TRP (1u << 4)
#define UFSR_UNDEFINSTR (1u << 16)
#define UFSR_DIVBYZERO (1u << 25)

#define MM_CPU_HZ 64000000ull
#define NS_PER_SEC 1000000000ull
#define DEFAULT_BATCH_CYCLES 64ull         /* ~1 us @ 64 MHz */
#define DEFAULT_SYNC_GRANULARITY 640ull    /* ~10 us pacing interval */
#define IDLE_SLEEP_NS 200000ull            /* 200 us host nap when fully idle */

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

static mm_bool parse_pc_trace_range(const char *s, mm_u32 *start_out, mm_u32 *end_out)
{
    const char *dash;
    mm_u32 start = 0;
    mm_u32 end = 0;
    if (s == 0 || start_out == 0 || end_out == 0) {
        return MM_FALSE;
    }
    dash = strchr(s, '-');
    if (dash == 0) {
        return MM_FALSE;
    }
    if (!parse_hex_u32(s, &start)) {
        return MM_FALSE;
    }
    if (!parse_hex_u32(dash + 1, &end)) {
        return MM_FALSE;
    }
    *start_out = start;
    *end_out = end;
    return MM_TRUE;
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
};

static mm_bool reload_images(struct mm_image_spec *images,
                             int image_count,
                             mm_u8 *flash,
                             size_t flash_size,
                             size_t *loaded_total,
                             size_t *loaded_max_end)
{
    int i;
    size_t idx;
    size_t total = 0;
    size_t max_end = 0;
    if (images == 0 || flash == 0 || loaded_total == 0 || loaded_max_end == 0) {
        return MM_FALSE;
    }
    for (idx = 0; idx < flash_size; ++idx) {
        flash[idx] = 0xFFu;
    }
    for (i = 0; i < image_count; ++i) {
        size_t n = 0;
        mm_u32 b0 = images[i].offset;
        if (load_file_at(images[i].path, flash, flash_size, images[i].offset, &n) != 0) {
            fprintf(stderr, "failed to reload image %s\n", images[i].path);
            return MM_FALSE;
        }
        images[i].loaded = n;
        total += n;
        if ((size_t)b0 + n > max_end) {
            max_end = (size_t)b0 + n;
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
                 "exec arm-none-eabi-gdb -q -ex \"file %s\" -ex \"tar rem:%d\" -ex \"tui enable\" -ex \"focus cmd\"",
                 elf, port);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "exec arm-none-eabi-gdb -q -ex \"tar rem:%d\" -ex \"tui enable\" -ex \"focus cmd\"",
                 port);
    }
    if (fork() == 0) {
        execl("/usr/bin/x-terminal-emulator", "/usr/bin/x-terminal-emulator",
              "-e", "/bin/sh", "-c", cmd, (char *)0);
        _exit(127);
    }
}

static mm_bool handle_tui(struct mm_tui *tui,
                          mm_bool opt_tui,
                          mm_bool *opt_capstone,
                          mm_bool *opt_gdb,
                          struct mm_gdb_stub *gdb,
                          const char *cpu_name,
                          const char *gdb_symbols,
                          struct mm_cpu *cpu,
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
        mm_tui_set_core_state(tui,
                              cpu->r[15],
                              mm_cpu_get_active_sp(cpu),
                              (mm_u8)cpu->sec_state,
                              (mm_u8)cpu->mode,
                              *steps_latched);
        mm_tui_set_registers(tui, cpu);
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
                launch_gdb_tui(tui);
                printf("Waiting for GDB connection...\n");
                if (mm_gdb_stub_wait_client(gdb)) {
                    const char *exec_path = (gdb_symbols != 0) ? gdb_symbols : tui->image0_path;
                    if (exec_path != 0 && exec_path[0] != '\0') {
                        mm_gdb_stub_set_exec_path(gdb, exec_path);
                    }
                } else {
                    fprintf(stderr, "Failed to accept GDB connection\n");
                }
            } else {
                fprintf(stderr, "Failed to start GDB server\n");
            }
        }
    }
    return MM_FALSE;
}

static mm_bool parse_u32(const char *s, mm_u32 *out);
static mm_bool parse_usb_spec(const char *spec, int *port_out);
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

static mm_bool parse_usb_spec(const char *spec, int *port_out)
{
    mm_u32 port;
    if (spec == 0 || port_out == 0) return MM_FALSE;
    if (strncmp(spec, "port=", 5) != 0) {
        return MM_FALSE;
    }
    if (!parse_u32(spec + 5, &port)) {
        return MM_FALSE;
    }
    if (port == 0u || port > 65535u) {
        return MM_FALSE;
    }
    *port_out = (int)port;
    return MM_TRUE;
}

static volatile mm_bool g_system_reset_pending = MM_FALSE;

void mm_system_request_reset(void)
{
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

static mm_bool g_quit_on_faults = MM_FALSE;
static mm_bool g_fault_pending = MM_FALSE;
static int g_stack_trace = -1;

static mm_bool stack_trace_enabled(void)
{
    if (g_stack_trace < 0) {
        const char *v = getenv("M33MU_STACK_TRACE");
        g_stack_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_stack_trace ? MM_TRUE : MM_FALSE;
}

static mm_u32 exc_return_encode(enum mm_sec_state sec, mm_bool use_psp, mm_bool to_thread)
{
    /* EXC_RETURN encodings (Armv8-M, DDI0553):
     *  bits[31:8] are always 0xFFFFFF
     *  bit6 selects target security state (1=Secure, 0=Non-secure)
     *  bit3 selects Thread(1) vs Handler(0) return
     *  bit2 selects PSP(1) vs MSP(0) when returning to Thread
     *
     * Typical values:
     *  Secure Thread/MSP: 0xFFFFFFF9
     *  Secure Thread/PSP: 0xFFFFFFFD
     *  Secure Handler/MSP:0xFFFFFFF1
     *  Non-sec Thread/MSP:0xFFFFFFB9
     *  Non-sec Thread/PSP:0xFFFFFFBD
     *  Non-sec Handler/MSP:0xFFFFFFB1
     */
    mm_u32 base = to_thread
        ? ((sec == MM_NONSECURE) ? 0xFFFFFFB9u : 0xFFFFFFF9u)
        : ((sec == MM_NONSECURE) ? 0xFFFFFFB1u : 0xFFFFFFF1u);
    if (to_thread && use_psp) {
        base |= 0x4u; /* SPSEL bit */
    }
    return base;
}

static mm_bool exc_return_unstack(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 exc_ret)
{
    struct mm_exc_return_info info;
    mm_u32 sp;
    mm_u32 frame[8];
    mm_u32 msp_s_val;
    mm_u32 msp_ns_val;
    mm_u32 psp_s_val;
    mm_u32 psp_ns_val;
    mm_u32 control_s_val;
    mm_u32 control_ns_val;
    int i;

    info = mm_exc_return_decode(exc_ret);
    if (!info.valid) {
        printf("[EXC_UNSTACK] invalid exc_return=0x%08lx\n", (unsigned long)exc_ret);
        return MM_FALSE;
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

    /* Prefer the recorded SP from exception entry to avoid guessing. */
    if (cpu->exc_depth > 0) {
        cpu->exc_depth--;
        sp = cpu->exc_sp[cpu->exc_depth];
        /* Optional: warn if EXC_RETURN disagrees with recorded stack choice. */
        if (cpu->exc_sec[cpu->exc_depth] != info.target_sec) {
            /* Fall back to architectural SP selection if security mismatched. */
            if (info.use_psp) {
                sp = (info.target_sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s;
            } else {
                sp = (info.target_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
            }
        }
    } else {
        /* Fallback to architectural selection if no recorded frame. */
        if (info.use_psp) {
            sp = (info.target_sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s;
        } else {
            sp = (info.target_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
        }
    }
    if (stack_trace_enabled()) {
        printf("[EXC_UNSTACK] chosen sp=0x%08lx exc_depth=%u\n",
               (unsigned long)sp,
               (unsigned)cpu->exc_depth);
    }

    msp_s_val = cpu->msp_s;
    msp_ns_val = cpu->msp_ns;
    psp_s_val = cpu->psp_s;
    psp_ns_val = cpu->psp_ns;
    control_s_val = cpu->control_s;
    control_ns_val = cpu->control_ns;
    (void)msp_s_val; (void)msp_ns_val; (void)psp_s_val; (void)psp_ns_val;
    (void)control_s_val; (void)control_ns_val;

    for (i = 0; i < 8; ++i) {
        if (!mm_memmap_read(map, info.target_sec, sp + (mm_u32)(i * 4), 4u, &frame[i])) {
            return MM_FALSE;
        }
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
        if (pc_suspect) {
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

    sp += 32u;
    if (info.use_psp) {
        if (info.target_sec == MM_NONSECURE) cpu->psp_ns = sp;
        else cpu->psp_s = sp;
    } else {
        if (info.target_sec == MM_NONSECURE) cpu->msp_ns = sp;
        else cpu->msp_s = sp;
    }
    /* Mirror active SP into R13 for thread execution. */
    if (info.use_psp) {
        cpu->r[13] = (info.target_sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s;
    } else {
        cpu->r[13] = (info.target_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    }

    cpu->sec_state = info.target_sec;
    cpu->mode = info.to_thread ? MM_THREAD : MM_HANDLER;
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
    return MM_TRUE;
}

/* Handle writes to PC; detect EXC_RETURN magic values and perform unstack. */
static mm_bool handle_pc_write(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               mm_u32 value,
                               mm_u8 *it_pattern,
                               mm_u8 *it_remaining,
                               mm_u8 *it_cond)
{
    if ((value & 0xffffff00u) == 0xffffff00u) {
        if (!exc_return_unstack(cpu, map, value)) {
            printf("EXC_RETURN unstack failed\n");
            return MM_FALSE;
        }
        itstate_sync_from_xpsr(cpu->xpsr, it_pattern, it_remaining, it_cond);
        return MM_TRUE;
    }
    cpu->r[15] = value | 1u;
    return MM_TRUE;
}

static mm_bool raise_hard_fault(struct mm_cpu *cpu, struct mm_memmap *map, struct mm_scs *scs, mm_u32 fault_pc, mm_u32 fault_xpsr)
{
    mm_u32 handler = 0;
    mm_u32 sp;
    mm_u32 exc_ret_val;
    mm_bool use_psp_entry;
    enum mm_mode pre_mode;
    mm_u32 frame[8];
    enum mm_sec_state sec;
    int i;

    if (cpu == 0 || map == 0 || scs == 0) {
        return MM_FALSE;
    }

    sec = cpu->sec_state;
    scs->hfsr |= (1u << 30); /* FORCED */
    if (sec == MM_NONSECURE) {
        scs->shcsr_ns |= (1u << 1); /* HARDFAULTACT */
    } else {
        scs->shcsr_s |= (1u << 1);
    }
    (void)mm_exception_read_handler(map, scs, sec, MM_VECT_HARDFAULT, &handler);

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
    frame[6] = fault_pc | 1u;
    frame[7] = fault_xpsr | 0x01000000u; /* Preserve full xPSR/IT/flags/IPSR; ensure T */

    pre_mode = cpu->mode;
    use_psp_entry = (pre_mode == MM_THREAD) && (((sec == MM_NONSECURE) ? cpu->control_ns : cpu->control_s) & 0x2u);
    exc_ret_val = exc_return_encode(sec, use_psp_entry, pre_mode == MM_THREAD);

    sp = use_psp_entry ? ((sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s)
                       : ((sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s);
    for (i = 7; i >= 0; --i) {
        sp -= 4u;
        if (!mm_memmap_write(map, sec, sp, 4u, frame[i])) {
            printf("HardFault: stacking failed at 0x%08lx\n", (unsigned long)sp);
            return MM_FALSE;
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
        cpu->exc_depth++;
    }
    /* Handler mode always uses MSP (ARM ARM DDI0553). Set R13 accordingly. */
    cpu->r[13] = (sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    cpu->xpsr = (fault_xpsr & 0xF8000000u) | 0x01000003u;
    cpu->r[14] = exc_ret_val;
    cpu->mode = MM_HANDLER;
    cpu->r[15] = handler | 1u;
    return MM_TRUE;
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
    sec = cpu->sec_state;
    printf("[MEMFAULT] pc=0x%08lx addr=0x%08lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx "
           "r4=%08lx r5=%08lx r6=%08lx r7=%08lx r12=%08lx sp=%08lx lr=%08lx xpsr=%08lx\n",
           (unsigned long)fault_pc,
           (unsigned long)addr,
           (unsigned long)cpu->r[0],
           (unsigned long)cpu->r[1],
           (unsigned long)cpu->r[2],
           (unsigned long)cpu->r[3],
           (unsigned long)cpu->r[4],
           (unsigned long)cpu->r[5],
           (unsigned long)cpu->r[6],
           (unsigned long)cpu->r[7],
           (unsigned long)cpu->r[12],
           (unsigned long)mm_cpu_get_active_sp(cpu),
           (unsigned long)cpu->r[14],
           (unsigned long)cpu->xpsr);
    if (g_quit_on_faults) {
        g_fault_pending = MM_TRUE;
    }

    /* TrustZone: SAU attribution violations are recorded in SAU_SFSR/SAU_SFAR.
     * For now we deliver the fault in the originating security state (so NS
     * firmware can handle and continue), while leaving SecureFault pending
     * information available for inspection. */
    if (sec == MM_SECURE && scs->securefault_pending) {
        scs->securefault_pending = MM_FALSE;
        return enter_exception_ex(cpu, map, scs, MM_VECT_SECUREFAULT, fault_pc, fault_xpsr, MM_SECURE);
    }

    bits |= (1u << 7); /* MMARVALID */
    scs->cfsr |= bits;
    scs->mmfar = addr;
    if ((scs->cfsr & 0x3u) == 0x3u) {
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
    if (sec == MM_NONSECURE) {
        scs->shcsr_ns |= 0x1u; /* MEMFAULTACT */
    } else {
        scs->shcsr_s |= 0x1u;
    }
    /* Deliver MemManage if enabled, otherwise escalate to HardFault. */
    if (sec == MM_NONSECURE) {
        if ((scs->shcsr_ns & (1u << 16)) != 0u) {
            return enter_exception(cpu, map, scs, MM_VECT_MEMMANAGE, fault_pc, fault_xpsr);
        }
    } else {
        if ((scs->shcsr_s & (1u << 16)) != 0u) {
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
    int i;
    enum mm_sec_state sec;

    if (cpu == 0 || map == 0 || scs == 0) {
        return MM_FALSE;
    }

    if (ufsr_bits == 0u) {
        ufsr_bits = UFSR_UNDEFINSTR;
    }
    sec = cpu->sec_state;
    scs->cfsr |= ufsr_bits;
    if (sec == MM_NONSECURE) {
        scs->shcsr_ns |= (1u << 2);
    } else {
        scs->shcsr_s |= (1u << 2); /* USGFAULTACT approximation */
    }
    (void)mm_exception_read_handler(map, scs, sec, MM_VECT_USAGEFAULT, &handler);

    frame[0] = cpu->r[0];
    frame[1] = cpu->r[1];
    frame[2] = cpu->r[2];
    frame[3] = cpu->r[3];
    frame[4] = cpu->r[12];
    frame[5] = cpu->r[14];
    frame[6] = fault_pc | 1u;
    frame[7] = fault_xpsr | (1u << 24); /* ensure Thumb; keep IPSR from preempted ctx */

    /* Select stack based on pre-fault thread CONTROL.SPSEL (Handler always MSP). */
    use_psp_entry = (cpu->mode == MM_THREAD) && ((sec == MM_NONSECURE ? cpu->control_ns : cpu->control_s) & 0x2u);
    sp = use_psp_entry ? ((sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s)
                       : ((sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s);
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
    exc_ret_val = exc_return_encode(sec, use_psp_entry, cpu->mode == MM_THREAD);
    printf("[USGFLT] enter sec=%d mode=%d use_psp=%d active_sp=0x%08lx MSP_S=0x%08lx MSP_NS=0x%08lx PSP_S=0x%08lx PSP_NS=0x%08lx CONTROL_S=0x%08lx CONTROL_NS=0x%08lx fault_pc=0x%08lx xpsr=0x%08lx handler=0x%08lx exc_ret=0x%08lx\n",
           (int)sec,
           (int)cpu->mode,
           (int)use_psp_entry,
           (unsigned long)sp,
           (unsigned long)msp_s_val,
           (unsigned long)msp_ns_val,
           (unsigned long)psp_s_val,
           (unsigned long)psp_ns_val,
           (unsigned long)control_s_val,
           (unsigned long)control_ns_val,
           (unsigned long)fault_pc,
           (unsigned long)fault_xpsr,
           (unsigned long)handler,
           (unsigned long)exc_ret_val);
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
    for (i = 7; i >= 0; --i) {
        mm_bool ok;
        sp -= 4u;
        ok = mm_memmap_write(map, sec, sp, 4u, frame[i]);
        if (!ok) {
            /* Stack write failed: escalate to HardFault (per ARMv8-M, stacking fault escalates). */
            printf("HardFault: stacking failed at 0x%08lx\n", (unsigned long)sp);
            return MM_FALSE;
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
    enum mm_mode pre_mode;
    mm_u32 exc_ret_val;
    int i;

    if (cpu == 0 || map == 0 || scs == 0) {
        return MM_FALSE;
    }

    sec = cpu->sec_state;
    pre_mode = cpu->mode;

    if (exc_num >= 16u) {
        vtor = (handler_sec == MM_NONSECURE) ? scs->vtor_ns : scs->vtor_s;
        (void)mm_vector_read(map, handler_sec, vtor, exc_num, &handler);
        if (exc_num == (16u + 74u)) {
            printf("[IRQ_ENTER] irq=74 sec=%d vtor=0x%08lx handler=0x%08lx pc=0x%08lx\n",
                   (int)handler_sec,
                   (unsigned long)vtor,
                   (unsigned long)handler,
                   (unsigned long)cpu->r[15]);
        }
    } else {
        (void)mm_exception_read_handler(map, scs, handler_sec, (enum mm_vector_index)exc_num, &handler);
    }

    switch (exc_num) {
    case MM_VECT_SVCALL:
        if (sec == MM_NONSECURE) scs->shcsr_ns |= (1u << 7);
        else scs->shcsr_s |= (1u << 7);
        break;
    case MM_VECT_PENDSV:
        scs->pend_sv = MM_FALSE;
        if (sec == MM_NONSECURE) scs->shcsr_ns |= (1u << 10);
        else scs->shcsr_s |= (1u << 10);
        break;
    case MM_VECT_SYSTICK:
        scs->pend_st = MM_FALSE;
        if (sec == MM_NONSECURE) scs->shcsr_ns |= (1u << 11);
        else scs->shcsr_s |= (1u << 11);
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
    frame[6] = return_pc | 1u;
    frame[7] = xpsr_in | 0x01000000u; /* preserve full xPSR/IT/flags/IPSR; ensure T */

    use_psp_entry = (pre_mode == MM_THREAD) && (((sec == MM_NONSECURE) ? cpu->control_ns : cpu->control_s) & 0x2u);
    exc_ret_val = exc_return_encode(sec, use_psp_entry, pre_mode == MM_THREAD);

    sp = use_psp_entry ? ((sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s)
                       : ((sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s);
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
    for (i = 7; i >= 0; --i) {
        sp -= 4u;
        if (!mm_memmap_write(map, sec, sp, 4u, frame[i])) {
            printf("HardFault: stacking failed at 0x%08lx\n", (unsigned long)sp);
            return MM_FALSE;
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
        cpu->exc_depth++;
    }
    cpu->r[13] = (handler_sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    cpu->xpsr = (xpsr_in & 0xF8000000u) | 0x01000000u | (exc_num & 0x1FFu);
    cpu->r[14] = exc_ret_val;
    cpu->mode = MM_HANDLER;
    cpu->sec_state = handler_sec;
    cpu->r[15] = handler | 1u;
    cpu->sleeping = MM_FALSE;
    cpu->event_reg = MM_FALSE;
    return MM_TRUE;
}

int main(int argc, char **argv)
{
    struct mm_image_spec images[16];
    int image_count = 0;
    size_t loaded_total = 0;
    size_t loaded_max_end = 0;
    mm_bool opt_gdb = MM_FALSE;
    mm_bool opt_dump = MM_FALSE;
    mm_bool opt_tui = MM_FALSE;
    mm_bool opt_persist = MM_FALSE;
    mm_bool opt_quit_on_faults = MM_FALSE;
    mm_bool opt_capstone = MM_FALSE;
    mm_bool opt_capstone_verbose = MM_FALSE;
    mm_bool opt_uart_stdout = MM_FALSE;
    mm_bool opt_meminfo = MM_FALSE;
    const char *gdb_symbols = 0;
    int gdb_port = 1234;
    const char *cpu_name = 0;
    int i;
    struct mm_target_cfg cfg;
    struct mm_memmap map;
    struct mmio_region regions[128];
    struct mm_cpu cpu;
    struct mm_scs scs;
    struct mm_prot_ctx prot;
    struct mm_nvic nvic;
    mm_u8 *flash;
    mm_u8 *ram;
    struct mm_gdb_stub gdb;
    struct mm_tui tui;
    struct mm_flash_persist persist;
    mm_bool tui_active = MM_FALSE;
    int rc = 0;
    /* IT block tracking: pattern encodes THEN(1)/ELSE(0) for remaining instructions. */
    mm_u8 it_pattern = 0;
    mm_u8 it_remaining = 0;
    mm_u8 it_cond = 0;
    mm_bool tui_paused = MM_FALSE;
    mm_bool tui_step = MM_FALSE;
    mm_bool reload_pending = MM_FALSE;
    mm_u64 tui_steps_offset = 0;
    mm_u64 tui_steps_latched = 0;
    mm_bool last_running = MM_TRUE;
    mm_bool opt_pc_trace = MM_FALSE;
    mm_bool opt_pc_trace_mem = MM_FALSE;
    mm_bool opt_strcmp_trace = MM_FALSE;
    mm_bool opt_usb = MM_FALSE;
    int usb_port = 3240;
    struct mm_spiflash_cfg spiflash_cfgs[8];
    int spiflash_count = 0;
#ifdef M33MU_HAS_LIBTPMS
    struct mm_tpm_tis_cfg tpm_cfgs[4];
    int tpm_count = 0;
#endif
    mm_u32 pc_trace_start = 0;
    mm_u32 pc_trace_end = 0;
    mm_u32 strcmp_trace_start = 0;
    mm_u32 strcmp_trace_end = 0;
    mm_u32 strcmp_entry = 0;
    mm_bool strcmp_active = MM_FALSE;
    mm_u32 strcmp_entry_r0 = 0;
    mm_bool strcmp_after_it = MM_FALSE;
    const char *pc_trace_env = getenv("M33MU_PC_TRACE");
    const char *pc_trace_mem_env = getenv("M33MU_PC_TRACE_MEM");
    const char *strcmp_trace_env = getenv("M33MU_STRCMP_TRACE");
    const char *strcmp_entry_env = getenv("M33MU_STRCMP_ENTRY");
    const char *memwatch_env = getenv("M33MU_MEMWATCH");
    const char *capstone_pc_env = getenv("CAPSTONE_PC");
    mm_u32 memwatch_addr = 0;
    mm_u32 memwatch_size = 0;
    mm_u32 capstone_pc = 0;
    mm_bool opt_capstone_pc = MM_FALSE;

    if (pc_trace_env != 0 && pc_trace_env[0] != '\0') {
        if (parse_pc_trace_range(pc_trace_env, &pc_trace_start, &pc_trace_end)) {
            opt_pc_trace = MM_TRUE;
        }
    }
    if (pc_trace_mem_env != 0 && pc_trace_mem_env[0] != '\0') {
        opt_pc_trace_mem = MM_TRUE;
    }
    if (strcmp_trace_env != 0 && strcmp_trace_env[0] != '\0') {
        if (parse_pc_trace_range(strcmp_trace_env, &strcmp_trace_start, &strcmp_trace_end)) {
            opt_strcmp_trace = MM_TRUE;
            strcmp_entry = strcmp_trace_start;
        }
    }
    if (strcmp_entry_env != 0 && strcmp_entry_env[0] != '\0') {
        (void)parse_hex_u32(strcmp_entry_env, &strcmp_entry);
    }
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
        } else if (strcmp(argv[i], "--tui") == 0) {
            opt_tui = MM_TRUE;
        } else if (strcmp(argv[i], "--gdb-symbols") == 0 && i + 1 < argc) {
            gdb_symbols = argv[i + 1];
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
        } else if (strcmp(argv[i], "--meminfo") == 0) {
            opt_meminfo = MM_TRUE;
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
            if (!parse_usb_spec(argv[i] + 6, &usb_port)) {
                fprintf(stderr, "invalid usb spec: %s\n", argv[i]);
                return 1;
            }
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
            if (!parse_image_spec(argv[i], &images[image_count].path, &images[image_count].offset)) {
                fprintf(stderr, "invalid image spec: %s\n", argv[i]);
                return 1;
            }
            image_count++;
        }
    }

    if (image_count == 0) {
        fprintf(stderr, "usage: %s [--cpu cpu] [--gdb] [--port <n>] [--dump] [--tui] [--persist] "
#ifdef M33MU_USE_LIBCAPSTONE
                        "[--capstone] [--capstone-verbose] "
#endif
                        "[--uart-stdout] [--quit-on-faults] [--meminfo] [--gdb-symbols <elf>] "
                        "[--spiflash:SPIx:file=<path>:size=<n>[:mmap=0xaddr][:cs=GPIONAME]] "
                        "[--usb[:port=<n>]] "
#ifdef M33MU_HAS_LIBTPMS
                        "[--tpm:SPIx:cs=GPIONAME[:file=<path>]] "
#endif
                        "<image.bin[:offset]> [more images...]\n",
                argv[0]);
        return 1;
    }

    g_quit_on_faults = opt_quit_on_faults;
    mm_uart_io_set_stdout(opt_uart_stdout);
    if (opt_meminfo) {
        mm_scs_set_meminfo(MM_TRUE);
    }

    if (cpu_name == 0) {
        cpu_name = mm_cpu_default_name();
    }
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

    memset(&tui, 0, sizeof(tui));
    if (opt_tui) {
        if (!mm_tui_init(&tui) || !mm_tui_redirect_stdio(&tui)) {
            fprintf(stderr, "failed to initialize TUI\n");
            return 1;
        }
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
        for (i = 0; i < cfg.flash_size_s; ++i) {
            flash[i] = 0xFFu;
        }
        for (i = 0; i < cfg_total_ram(&cfg); ++i) {
            ram[i] = (mm_u8)(rand() & 0xFF);
        }
    }

    for (i = 0; i < image_count; ++i) {
        size_t n = 0;
        int j;
        mm_u32 b0;
        b0 = images[i].offset;
        if (load_file_at(images[i].path, flash, cfg.flash_size_s, images[i].offset, &n) != 0) {
            fprintf(stderr, "failed to load image %s\n", images[i].path);
            rc = 1;
            goto cleanup;
        }
        images[i].loaded = n;
        loaded_total += n;
        if ((size_t)images[i].offset + n > loaded_max_end) {
            loaded_max_end = (size_t)images[i].offset + n;
        }
        for (j = 0; j < i; ++j) {
            mm_u32 a0 = images[j].offset;
            mm_u32 a1 = images[j].offset + (mm_u32)images[j].loaded;
            mm_u32 b1 = b0 + (mm_u32)images[i].loaded;
            if (!(b1 <= a0 || b0 >= a1)) {
                fprintf(stderr, "warning: image %s overlaps %s\n", images[i].path, images[j].path);
            }
        }
    }

    memset(&persist, 0, sizeof(persist));
    if (opt_persist) {
        const char *paths[16];
        mm_u32 offsets[16];
        int k;
        for (k = 0; k < image_count; ++k) {
            paths[k] = images[k].path;
            offsets[k] = images[k].offset;
        }
        mm_flash_persist_build(&persist, flash, cfg.flash_size_s, paths, offsets, image_count);
    }

    mm_gdb_stub_init(&gdb);
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
        if (!mm_gdb_stub_wait_client(&gdb)) {
            fprintf(stderr, "Failed to accept GDB connection\n");
            rc = 1;
            goto cleanup;
        }
        mm_gdb_stub_set_exec_path(&gdb, (gdb_symbols != 0) ? gdb_symbols : images[0].path);
    }

    {
        int k;
        for (k = 0; k < image_count; ++k) {
            printf("Loaded %zu bytes from %s @+0x%08lx\n",
                   images[k].loaded,
                   images[k].path,
                   (unsigned long)images[k].offset);
        }
        printf("Loaded total %zu bytes (max_end=0x%08lx)\n",
               loaded_total,
               (unsigned long)loaded_max_end);
    }

    {
        mm_bool first_start = MM_TRUE;
        for (;;) {
            mm_u64 cycle_total = 0;
            mm_bool done = MM_FALSE;
            mm_bool reset_again = MM_FALSE;
            mm_u64 vcycles = 0;
            mm_u64 vcycles_last_sync = 0;
            mm_u64 cycles_since_poll = 0;
            const mm_u64 poll_granularity = DEFAULT_BATCH_CYCLES;
            mm_u64 sync_granularity = DEFAULT_SYNC_GRANULARITY;
            mm_u64 host0_ns = host_now_ns();
            mm_u64 cpu_hz = MM_CPU_HZ;
            mm_u64 hz_now = 0;
            mm_u64 last_hz = 0;
            tui_steps_offset = 0;

            mm_system_clear_reset();
            mm_memmap_init(&map, regions, sizeof(regions) / sizeof(regions[0]));
            mm_target_soc_reset(&cfg);
            mm_timer_reset(&cfg);
            mm_spiflash_reset_all();
#ifdef M33MU_HAS_LIBTPMS
            mm_tpm_tis_reset_all();
#endif
            mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE);
            mm_memmap_configure_flash(&map, &cfg, flash, MM_FALSE);
            map.flash.base = cfg.flash_base_s;
            map.flash.length = cfg.flash_size_s;
            mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE);
            mm_memmap_configure_ram(&map, &cfg, ram, MM_FALSE);
            map.ram.base = cfg.ram_base_s;
            map.ram.length = cfg_total_ram(&cfg);
            mm_target_register_mmio(&cfg, &map.mmio);
            mm_spiflash_register_mmap_regions(&map.mmio);
            mm_target_flash_bind(&cfg, &map, flash, cfg.flash_size_s, opt_persist ? &persist : 0);
            mm_target_usart_reset(&cfg);
            mm_target_usart_init(&cfg, &map.mmio, &nvic);
            mm_target_spi_reset(&cfg);
            mm_target_spi_init(&cfg, &map.mmio, &nvic);
            mm_timer_init(&cfg, &map.mmio, &nvic);

            mm_scs_init(&scs, 0x410fc241u);
            mm_scs_register_regions(&scs, &map.mmio, 0xE000ED00u, 0xE002ED00u, &nvic);
            mm_core_sys_register(&map.mmio);
            mm_prot_init(&prot, &scs, &cfg);
            mm_memmap_set_interceptor(&map, mm_prot_interceptor, &prot);
            mm_prot_add_region(&prot, cfg.flash_base_s, cfg.flash_size_s, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
            mm_prot_add_region(&prot, cfg.flash_base_ns, cfg.flash_size_ns, MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
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
            mm_spiflash_register_prot_regions(&prot);

            mm_nvic_init(&nvic);

            /* Reset CPU state */
            {
                int i;
                for (i = 0; i < 16; ++i) cpu.r[i] = 0;
                cpu.xpsr = 0;
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
                cpu.vtor_s = cfg.flash_base_s;
                cpu.vtor_ns = cfg.flash_base_ns;
                cpu.exc_depth = 0;
                cpu.tz_depth = 0;
                cpu.sleeping = MM_FALSE;
                cpu.event_reg = MM_FALSE;
            }

            if (!mm_vector_apply_reset(&cpu, &map, MM_SECURE)) {
                fprintf(stderr, "failed to apply reset\n");
                rc = 1;
                goto cleanup;
            }
            /* Keep SCS VTOR banks in sync with CPU reset values so exception dispatch uses VTOR_S/VTOR_NS. */
            scs.vtor_s = cpu.vtor_s;
            scs.vtor_ns = cpu.vtor_ns;

            if (opt_gdb) {
                mm_gdb_stub_notify_stop(&gdb, 5);
            }

            if (first_start) {
                if (opt_usb) {
                    if (!mm_usbdev_start(usb_port)) {
                        fprintf(stderr, "failed to start USB/IP server\n");
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
            strcmp_active = MM_FALSE;
            strcmp_entry_r0 = 0;
            strcmp_after_it = MM_FALSE;
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
                    if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, gdb_symbols, &cpu, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
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
                
                if (!running_now) {
                    host_sync_if_needed(vcycles, &vcycles_last_sync, host0_ns, sync_granularity, cpu_hz);
                    mm_target_usart_poll(&cfg);
                    mm_target_spi_poll(&cfg);
                    mm_usbdev_poll();
                    update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                             &tui_steps_offset, &tui_steps_latched);
                    if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, gdb_symbols, &cpu, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                        done = MM_TRUE;
                        continue;
                    }
                    {
                        struct timespec req;
                        req.tv_sec = 0;
                        req.tv_nsec = (long)IDLE_SLEEP_NS;
                        nanosleep(&req, 0);
                    }
                    if (mm_system_reset_pending()) {
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
                    if (reload_images(images, image_count, flash, cfg.flash_size_s, &loaded_total, &loaded_max_end)) {
                        if (opt_persist) {
                            const char *paths[16];
                            mm_u32 offsets[16];
                            int k;
                            for (k = 0; k < image_count; ++k) {
                                paths[k] = images[k].path;
                                offsets[k] = images[k].offset;
                            }
                            mm_flash_persist_build(&persist, flash, cfg.flash_size_s, paths, offsets, image_count);
                        }
                        mm_system_request_reset();
                    }
                    reload_pending = MM_FALSE;
                    continue;
                }

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
                    } else if (scs.pend_st || scs.pend_sv) {
                        wake = MM_TRUE;
                    } else if (mm_nvic_select(&nvic, &cpu) >= 0) {
                        wake = MM_TRUE;
                    }
                    if (wake) {
                        cpu.sleeping = MM_FALSE;
                        cpu.event_reg = MM_FALSE;
                    } else {
                        mm_u64 delta = mm_scs_systick_cycles_until_fire(&scs);
                        if (delta == (mm_u64)-1) {
                            struct timespec req;
                            req.tv_sec = 0;
                            req.tv_nsec = (long)IDLE_SLEEP_NS;
                            nanosleep(&req, 0);
                            mm_target_usart_poll(&cfg);
                            mm_target_spi_poll(&cfg);
                            mm_usbdev_poll();
                            update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                                     &tui_steps_offset, &tui_steps_latched);
                            if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, gdb_symbols, &cpu, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                                done = MM_TRUE;
                                continue;
                            }
                            if (mm_system_reset_pending()) {
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
                            if (scs.pend_st || scs.pend_sv) {
                                cpu.sleeping = MM_FALSE;
                                cpu.event_reg = MM_FALSE;
                            }
                            host_sync_if_needed(vcycles, &vcycles_last_sync, host0_ns, sync_granularity, cpu_hz);
                            mm_target_usart_poll(&cfg);
                            mm_target_spi_poll(&cfg);
                            mm_usbdev_poll();
                            update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                                     &tui_steps_offset, &tui_steps_latched);
                            if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, gdb_symbols, &cpu, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                                done = MM_TRUE;
                                continue;
                            }
                            cycles_since_poll = 0;
                            if (mm_system_reset_pending()) {
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
                if (scs.pend_st) {
                    if (!enter_exception(&cpu, &map, &scs, MM_VECT_SYSTICK, cpu.r[15] & ~1u, cpu.xpsr)) {
                        done = MM_TRUE;
                    } else {
                        itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                    }
                    continue;
                }
                if (scs.pend_sv) {
                    if (!enter_exception(&cpu, &map, &scs, MM_VECT_PENDSV, cpu.r[15] & ~1u, cpu.xpsr)) {
                        done = MM_TRUE;
                    } else {
                        itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                    }
                    continue;
                }

                /* Manage interrupts */
                {
                    enum mm_sec_state irq_sec = MM_SECURE;
                    pend_irq = mm_nvic_select_routed(&nvic, &cpu, &irq_sec);
                    if (pend_irq >= 0) {
                        mm_u32 exc_num = 16u + (mm_u32)pend_irq;
                        /* Clear pending when accepted. */
                        mm_nvic_set_pending(&nvic, (mm_u32)pend_irq, MM_FALSE);
                        if (!enter_exception_ex(&cpu, &map, &scs, exc_num, cpu.r[15] & ~1u, cpu.xpsr, irq_sec)) {
                            done = MM_TRUE;
                        } else {
                            itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                        }
                        continue;
                    }
                }

                /* Fetch/decode */
                {
                    struct mm_fetch_result f;
                    struct mm_decoded d;
                    mm_bool execute_it;
                    mm_u32 pc_before_exec = 0;
                    const mm_u32 insn_cycles = 1u;
                    (void)pc_before_exec;
                    cycles_since_poll += insn_cycles;
                    cycle_total += insn_cycles;
                    vcycles += insn_cycles;
                    mm_scs_systick_advance(&scs, insn_cycles);
                    mm_timer_tick(&cfg, insn_cycles);

                    /* Keep R13 consistent with the active banked SP so that instructions
                     * like LDR/STR [SP,#imm] and function prologue/epilogue sequences
                     * operate on the correct stack memory.
                     */
                    cpu.r[13] = mm_cpu_get_active_sp(&cpu);

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
                            break;
                        }
                        continue;
                    }
                    d = mm_decode_t32(&f);
                    mm_memmap_set_last_pc(f.pc_fetch);
                    if (opt_pc_trace) {
                        mm_u32 pc = f.pc_fetch | 1u;
                        if (pc >= pc_trace_start && pc <= pc_trace_end) {
                            printf("[PC_TRACE] PC=0x%08lx insn=0x%08lx len=%u kind=%u rn=%u rd=%u rm=%u imm=0x%08lx r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx r4=0x%08lx r5=0x%08lx r6=0x%08lx r7=0x%08lx r8=0x%08lx r9=0x%08lx r10=0x%08lx r11=0x%08lx r12=0x%08lx sp=0x%08lx lr=0x%08lx xpsr=0x%08lx it_pat=0x%02x it_rem=%u it_cond=0x%02x\n",
                                   (unsigned long)pc,
                                   (unsigned long)f.insn,
                                   (unsigned)d.len,
                                   (unsigned)d.kind,
                                   (unsigned)d.rn,
                                   (unsigned)d.rd,
                                   (unsigned)d.rm,
                                   (unsigned long)d.imm,
                                   (unsigned long)cpu.r[0],
                                   (unsigned long)cpu.r[1],
                                   (unsigned long)cpu.r[2],
                                   (unsigned long)cpu.r[3],
                                   (unsigned long)cpu.r[4],
                                   (unsigned long)cpu.r[5],
                                   (unsigned long)cpu.r[6],
                                   (unsigned long)cpu.r[7],
                                   (unsigned long)cpu.r[8],
                                   (unsigned long)cpu.r[9],
                                   (unsigned long)cpu.r[10],
                                   (unsigned long)cpu.r[11],
                                   (unsigned long)cpu.r[12],
                                   (unsigned long)mm_cpu_get_active_sp(&cpu),
                                   (unsigned long)cpu.r[14],
                                   (unsigned long)cpu.xpsr,
                                   (unsigned)it_pattern,
                                   (unsigned)it_remaining,
                                   (unsigned)it_cond);
                            if (opt_pc_trace_mem) {
                                mm_u32 addr = cpu.r[6];
                                mm_u32 prev = addr - 8u;
                                mm_u32 v0 = 0;
                                mm_u32 v1 = 0;
                                mm_u32 v2 = 0;
                                mm_u32 v3 = 0;
                                mm_bool ok0 = mm_memmap_read(&map, cpu.sec_state, prev, 4u, &v0);
                                mm_bool ok1 = mm_memmap_read(&map, cpu.sec_state, prev + 4u, 4u, &v1);
                                mm_bool ok2 = mm_memmap_read(&map, cpu.sec_state, addr, 4u, &v2);
                                mm_bool ok3 = mm_memmap_read(&map, cpu.sec_state, addr + 4u, 4u, &v3);
                                if (ok0 && ok1 && ok2 && ok3) {
                                    printf("[PC_TRACE_MEM] r6=0x%08lx prev[0]=0x%08lx prev[1]=0x%08lx next[0]=0x%08lx next[1]=0x%08lx\n",
                                           (unsigned long)addr,
                                           (unsigned long)v0,
                                           (unsigned long)v1,
                                           (unsigned long)v2,
                                           (unsigned long)v3);
                                } else {
                                    printf("[PC_TRACE_MEM] r6=0x%08lx prev=<fault> next=<fault>\n",
                                           (unsigned long)addr);
                                }
                            }
                        }
                    }
                    if (opt_strcmp_trace) {
                        mm_u32 pc = f.pc_fetch | 1u;
                        if (pc >= strcmp_trace_start && pc <= strcmp_trace_end) {
                            if (!strcmp_active && cpu.r[0] == cpu.r[1]) {
                                strcmp_active = MM_TRUE;
                                strcmp_after_it = MM_FALSE;
                                strcmp_entry_r0 = cpu.r[0];
                                printf("[STRCMP_TRACE] entry PC=0x%08lx ptr=0x%08lx\n",
                                       (unsigned long)pc,
                                       (unsigned long)strcmp_entry_r0);
                            }
                            if (strcmp_active && d.kind == MM_OP_IT) {
                                strcmp_after_it = MM_TRUE;
                            }
                            if (strcmp_active && strcmp_after_it && d.kind != MM_OP_IT &&
                                it_remaining == 0u && cpu.r[0] != cpu.r[1]) {
                                printf("[STRCMP_TRACE] divergence PC=0x%08lx r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx sp=0x%08lx lr=0x%08lx xpsr=0x%08lx\n",
                                       (unsigned long)pc,
                                       (unsigned long)cpu.r[0],
                                       (unsigned long)cpu.r[1],
                                       (unsigned long)cpu.r[2],
                                       (unsigned long)cpu.r[3],
                                       (unsigned long)mm_cpu_get_active_sp(&cpu),
                                       (unsigned long)cpu.r[14],
                                       (unsigned long)cpu.xpsr);
                                done = MM_TRUE;
                            }
                        } else if (strcmp_active) {
                            strcmp_active = MM_FALSE;
                        }
                    }
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
                        if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) {
                            printf("Unimplemented opcode 0x%08lx at PC=0x%08lx\n", (unsigned long)d.raw, (unsigned long)(f.pc_fetch | 1u));
                            if (opt_gdb) {
                                mm_gdb_stub_notify_stop(&gdb, 4);
                            }
                            break;
                        }
                        continue;
                    }

                    /* ITSTATE handling: if inside IT block and not IT instruction, conditionally execute. */
                    execute_it = MM_TRUE;
                    if (it_remaining > 0u && d.kind != MM_OP_IT) {
                        mm_bool cond_true = MM_FALSE;
                        mm_bool take = MM_FALSE;
                        mm_bool n = (cpu.xpsr & (1u << 31)) != 0u;
                        mm_bool z = (cpu.xpsr & (1u << 30)) != 0u;
                        mm_bool c = (cpu.xpsr & (1u << 29)) != 0u;
                        mm_bool v = (cpu.xpsr & (1u << 28)) != 0u;
                        mm_u8 cond = it_cond;
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
                        take = ((it_pattern & 0x1u) != 0u) ? cond_true : !cond_true;
                        execute_it = take;
                    }

                    if (opt_dump) {
                        printf("[DUMP] PC=0x%08lx len=%u opcode=0x%08lx kind=%d r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx sp=0x%08lx\n",
                                (unsigned long)(f.pc_fetch | 1u),
                                (unsigned)d.len,
                                (unsigned long)d.raw,
                                (int)d.kind,
                                (unsigned long)cpu.r[0],
                                (unsigned long)cpu.r[1],
                                (unsigned long)cpu.r[2],
                                (unsigned long)cpu.r[3],
                                (unsigned long)mm_cpu_get_active_sp(&cpu));
                    }
                    if (!execute_it && d.kind != MM_OP_IT) {
                        if (it_remaining > 0u) {
                            mm_u8 raw = itstate_get(cpu.xpsr);
                            it_pattern >>= 1;
                            it_remaining--;
                            raw = itstate_advance(raw);
                            cpu.xpsr = itstate_set(cpu.xpsr, raw);
                        }
                        continue;
                    }

                    {
                        struct mm_execute_ctx exec_ctx;
                        exec_ctx.cpu = &cpu;
                        exec_ctx.map = &map;
                        exec_ctx.scs = &scs;
                        exec_ctx.gdb = &gdb;
                        exec_ctx.fetch = &f;
                        exec_ctx.dec = &d;
                        exec_ctx.opt_dump = opt_dump;
                        exec_ctx.opt_gdb = opt_gdb;
                        exec_ctx.it_pattern = &it_pattern;
                        exec_ctx.it_remaining = &it_remaining;
                        exec_ctx.it_cond = &it_cond;
                        exec_ctx.done = &done;
                        exec_ctx.handle_pc_write = handle_pc_write;
                        exec_ctx.raise_mem_fault = raise_mem_fault;
                        exec_ctx.raise_usage_fault = raise_usage_fault;
                        exec_ctx.exc_return_unstack = exc_return_unstack;
                        exec_ctx.enter_exception = enter_exception;
                        if (mm_execute_decoded(&exec_ctx) == MM_EXEC_CONTINUE) {
                            continue;
                        }
                        if (opt_tui && !opt_gdb && done && d.kind == MM_OP_BKPT) {
                            done = MM_FALSE;
                            tui_paused = MM_TRUE;
                            tui_step = MM_FALSE;
                        }
                    }

                    if (opt_capstone) {
                        if (!capstone_it_check_post(&f, &d, it_pattern, it_remaining, it_cond)) {
                            rc = 1;
                            goto cleanup;
                        }
                    }

                    if (it_remaining > 0u && d.kind != MM_OP_IT) {
                        mm_u8 raw = itstate_get(cpu.xpsr);
                        it_pattern >>= 1;
                        it_remaining--;
                        raw = itstate_advance(raw);
                        cpu.xpsr = itstate_set(cpu.xpsr, raw);
                    }
                }

                if (cycles_since_poll >= poll_granularity) {
                    mm_target_usart_poll(&cfg);
                    mm_target_spi_poll(&cfg);
                    mm_usbdev_poll();
                    update_tui_steps_latched(opt_gdb, &gdb, tui_paused, tui_step, cycle_total,
                                             &tui_steps_offset, &tui_steps_latched);
                    if (handle_tui(&tui, opt_tui, &opt_capstone, &opt_gdb, &gdb, cpu_name, gdb_symbols, &cpu, &map, cycle_total, &tui_steps_offset, &tui_steps_latched, &tui_paused, &tui_step, &reload_pending, gdb_port)) {
                        done = MM_TRUE;
                        continue;
                    }
                    cycles_since_poll = 0;
                }

                host_sync_if_needed(vcycles, &vcycles_last_sync, host0_ns, sync_granularity, cpu_hz);

                if (mm_system_reset_pending()) {
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
            break;
        }
    }

cleanup:
    mm_spiflash_shutdown_all();
#ifdef M33MU_HAS_LIBTPMS
    mm_tpm_tis_shutdown_all();
#endif
    mm_usbdev_stop();
    if (opt_capstone) {
        capstone_shutdown();
    }
    mm_gdb_stub_close(&gdb);
    if (tui_active) {
        mm_tui_register(0);
        mm_tui_stop_thread(&tui);
        mm_tui_shutdown(&tui);
    }
    return rc;
}
