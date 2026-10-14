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

#include "m33mu/signal.h"
#include "m33mu/gpio.h"

#define MM_SIGNAL_MAGIC "SIGC"

struct mm_signal_container_header {
    char   magic[4];
    mm_u32 version;
    mm_u32 num_timebases;
    mm_u32 num_traces;
};

struct mm_signal_timebase_desc {
    mm_u32 id;
    mm_u32 sample_count;
    mm_u32 period_ns; /* 0 => non-uniform: mm_u64 timestamps_ns[] follows inline */
};

struct mm_signal_trace_desc {
    mm_u32 id;
    char   name[MM_SIGNAL_TRACE_NAME_LEN];
    mm_u32 timebase_id;
    mm_i32 full_scale_mv;
};

struct mm_signal_timebase {
    mm_u32  id;
    mm_u32  sample_count;
    mm_u32  period_ns;
    mm_u64 *timestamps_ns;
};

struct mm_signal_trace {
    mm_u32 id;
    char   name[MM_SIGNAL_TRACE_NAME_LEN];
    mm_u32 timebase_idx;
    mm_i16 *samples_mv;
};

struct mm_signal_crossing {
    mm_u64  at_ns;
    mm_bool rising;
};

struct mm_signal_binding {
    mm_u32  trace_idx;
    enum mm_signal_role role;
    int     bank;
    int     pin;

    struct mm_signal_crossing *crossings;
    mm_u32  crossing_count;
    mm_u32  next_crossing;
    mm_bool current_level;
};

static struct mm_signal_timebase g_timebases[MM_SIGNAL_MAX_TIMEBASES];
static mm_u32 g_timebase_count = 0;

static struct mm_signal_trace g_traces[MM_SIGNAL_MAX_TRACES];
static mm_u32 g_trace_count = 0;

static struct mm_signal_binding g_bindings[MM_SIGNAL_MAX_BINDINGS];
static mm_u32 g_binding_count = 0;

static mm_bool g_trigger_set = MM_FALSE;
static mm_u32 g_trigger_pc = 0;
static mm_bool g_triggered = MM_FALSE;
static mm_u64 g_trigger_ns = 0;

/* Typical STM32H5 I/O Schmitt hysteresis is 250mV, centered at VDD/2. */
static mm_i32 g_vdd_mv = 3300;
#define MM_SIGNAL_HYSTERESIS_HALF_MV 125

#define MM_SIGNAL_NS_PER_SEC 1000000000ull
#define MM_SIGNAL_DEFAULT_CPU_HZ 64000000ull

static mm_u64 g_cpu_hz = MM_SIGNAL_DEFAULT_CPU_HZ;
static mm_u64 g_total_vcycles = 0;
/* Stimulus time is g_base_ns at g_base_vcycles, the last clock change. */
static mm_u64 g_base_vcycles = 0;
static mm_u64 g_base_ns = 0;
static mm_bool g_rewrite_levels = MM_FALSE;

static mm_u64 signal_cycles_to_ns(mm_u64 cycles)
{
    __int128 prod = (__int128)cycles * (__int128)MM_SIGNAL_NS_PER_SEC;
    prod /= (__int128)g_cpu_hz;
    return (mm_u64)prod;
}

static mm_u64 signal_now_ns(void)
{
    return g_base_ns + signal_cycles_to_ns(g_total_vcycles - g_base_vcycles);
}

void mm_signal_set_cpu_hz(mm_u64 hz)
{
    if (hz == 0u) {
        hz = MM_SIGNAL_DEFAULT_CPU_HZ;
    }
    if (hz == g_cpu_hz) {
        return;
    }
    g_base_ns = signal_now_ns();
    g_base_vcycles = g_total_vcycles;
    g_cpu_hz = hz;
}

static mm_u64 signal_ns_to_cycles_ceil(mm_u64 ns)
{
    __int128 prod = (__int128)ns * (__int128)g_cpu_hz;
    prod += (__int128)(MM_SIGNAL_NS_PER_SEC - 1u);
    prod /= (__int128)MM_SIGNAL_NS_PER_SEC;
    return (mm_u64)prod;
}

static void signal_free_loaded(void)
{
    mm_u32 i;
    for (i = 0; i < g_timebase_count; ++i) {
        free(g_timebases[i].timestamps_ns);
        g_timebases[i].timestamps_ns = 0;
    }
    for (i = 0; i < g_trace_count; ++i) {
        free(g_traces[i].samples_mv);
        g_traces[i].samples_mv = 0;
    }
    for (i = 0; i < g_binding_count; ++i) {
        free(g_bindings[i].crossings);
        g_bindings[i].crossings = 0;
    }
    g_timebase_count = 0;
    g_trace_count = 0;
    g_binding_count = 0;
    g_trigger_set = MM_FALSE;
    g_triggered = MM_FALSE;
}

void mm_signal_reset(void)
{
    signal_free_loaded();
    memset(g_timebases, 0, sizeof(g_timebases));
    memset(g_traces, 0, sizeof(g_traces));
    memset(g_bindings, 0, sizeof(g_bindings));
    g_vdd_mv = 3300;
    g_total_vcycles = 0;
    g_base_vcycles = 0;
    g_base_ns = 0;
    g_rewrite_levels = MM_FALSE;
}

void mm_signal_prepare_for_run(void)
{
    g_rewrite_levels = MM_TRUE;
}

void mm_signal_set_vdd_mv(mm_i32 vdd_mv)
{
    g_vdd_mv = vdd_mv;
}

static mm_i32 signal_hi_threshold_mv(void)
{
    return (g_vdd_mv / 2) + MM_SIGNAL_HYSTERESIS_HALF_MV;
}

static mm_i32 signal_lo_threshold_mv(void)
{
    return (g_vdd_mv / 2) - MM_SIGNAL_HYSTERESIS_HALF_MV;
}

static mm_bool signal_read_exact(FILE *f, void *buf, size_t n)
{
    return (fread(buf, 1u, n, f) == n) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_signal_load(const char *path)
{
    FILE *f;
    struct mm_signal_container_header hdr;
    mm_u32 i;

    if (path == 0) {
        return MM_FALSE;
    }

    f = fopen(path, "rb");
    if (f == 0) {
        fprintf(stderr, "signal: failed to open %s\n", path);
        return MM_FALSE;
    }

    if (!signal_read_exact(f, &hdr, sizeof(hdr))) {
        fprintf(stderr, "signal: %s: short read on header\n", path);
        fclose(f);
        return MM_FALSE;
    }
    if (memcmp(hdr.magic, MM_SIGNAL_MAGIC, 4) != 0) {
        fprintf(stderr, "signal: %s: bad magic\n", path);
        fclose(f);
        return MM_FALSE;
    }
    if (hdr.num_timebases > MM_SIGNAL_MAX_TIMEBASES ||
        hdr.num_traces > MM_SIGNAL_MAX_TRACES) {
        fprintf(stderr, "signal: %s: exceeds timebase/trace limits (%u/%u)\n",
                path, hdr.num_timebases, hdr.num_traces);
        fclose(f);
        return MM_FALSE;
    }

    signal_free_loaded();

    for (i = 0; i < hdr.num_timebases; ++i) {
        struct mm_signal_timebase_desc desc;
        struct mm_signal_timebase *tb = &g_timebases[i];

        if (!signal_read_exact(f, &desc, sizeof(desc))) {
            fprintf(stderr, "signal: %s: short read on timebase %u\n", path, i);
            goto fail;
        }
        tb->id = desc.id;
        tb->sample_count = desc.sample_count;
        tb->period_ns = desc.period_ns;
        tb->timestamps_ns = 0;

        if (desc.period_ns == 0u) {
            if (desc.sample_count == 0u) {
                fprintf(stderr, "signal: %s: timebase %u is non-uniform with 0 samples\n",
                        path, i);
                goto fail;
            }
            tb->timestamps_ns = (mm_u64 *)malloc(sizeof(mm_u64) * (size_t)desc.sample_count);
            if (tb->timestamps_ns == 0) {
                fprintf(stderr, "signal: %s: out of memory (timebase %u timestamps)\n", path, i);
                goto fail;
            }
            if (!signal_read_exact(f, tb->timestamps_ns,
                                    sizeof(mm_u64) * (size_t)desc.sample_count)) {
                fprintf(stderr, "signal: %s: short read on timebase %u timestamps\n", path, i);
                goto fail;
            }
        }
        g_timebase_count = i + 1;
    }

    for (i = 0; i < hdr.num_traces; ++i) {
        struct mm_signal_trace_desc desc;
        struct mm_signal_trace *tr = &g_traces[i];
        mm_u32 tb_idx;
        mm_bool found_tb = MM_FALSE;

        if (!signal_read_exact(f, &desc, sizeof(desc))) {
            fprintf(stderr, "signal: %s: short read on trace %u\n", path, i);
            goto fail;
        }
        desc.name[MM_SIGNAL_TRACE_NAME_LEN - 1] = '\0';

        for (tb_idx = 0; tb_idx < g_timebase_count; ++tb_idx) {
            if (g_timebases[tb_idx].id == desc.timebase_id) {
                found_tb = MM_TRUE;
                break;
            }
        }
        if (!found_tb) {
            fprintf(stderr, "signal: %s: trace %u references unknown timebase id %u\n",
                    path, i, desc.timebase_id);
            goto fail;
        }

        tr->id = desc.id;
        memcpy(tr->name, desc.name, MM_SIGNAL_TRACE_NAME_LEN);
        tr->timebase_idx = tb_idx;
        tr->samples_mv = 0;

        {
            mm_u32 n = g_timebases[tb_idx].sample_count;
            if (n > 0u) {
                tr->samples_mv = (mm_i16 *)malloc(sizeof(mm_i16) * (size_t)n);
                if (tr->samples_mv == 0) {
                    fprintf(stderr, "signal: %s: out of memory (trace %u samples)\n", path, i);
                    goto fail;
                }
                if (!signal_read_exact(f, tr->samples_mv, sizeof(mm_i16) * (size_t)n)) {
                    fprintf(stderr, "signal: %s: short read on trace %u samples\n", path, i);
                    goto fail;
                }
            }
        }
        g_trace_count = i + 1;
    }

    fclose(f);
    return MM_TRUE;

fail:
    fclose(f);
    signal_free_loaded();
    return MM_FALSE;
}

static mm_u64 signal_timestamp_for_index(const struct mm_signal_timebase *tb, mm_u32 idx)
{
    if (tb->period_ns != 0u) {
        return (mm_u64)tb->period_ns * (mm_u64)idx;
    }
    return tb->timestamps_ns[idx];
}

static mm_bool signal_build_gpio_schedule(struct mm_signal_binding *b,
                                           const struct mm_signal_trace *tr)
{
    const struct mm_signal_timebase *tb = &g_timebases[tr->timebase_idx];
    mm_u32 n = tb->sample_count;
    mm_i32 hi_thr = signal_hi_threshold_mv();
    mm_i32 lo_thr = signal_lo_threshold_mv();
    struct mm_signal_crossing *tmp;
    mm_u32 count = 0;
    mm_bool state;
    mm_u32 i;

    if (n == 0u) {
        b->crossings = 0;
        b->crossing_count = 0;
        b->current_level = MM_FALSE;
        return MM_TRUE;
    }

    tmp = (struct mm_signal_crossing *)malloc(
        sizeof(struct mm_signal_crossing) * (size_t)MM_SIGNAL_MAX_CROSSINGS);
    if (tmp == 0) {
        fprintf(stderr, "signal: out of memory building crossing schedule\n");
        return MM_FALSE;
    }

    state = (tr->samples_mv[0] >= hi_thr) ? MM_TRUE : MM_FALSE;
    b->current_level = state;

    for (i = 1; i < n; ++i) {
        mm_i16 v = tr->samples_mv[i];
        if (!state && v >= hi_thr) {
            state = MM_TRUE;
        } else if (state && v <= lo_thr) {
            state = MM_FALSE;
        } else {
            continue;
        }
        if (count >= MM_SIGNAL_MAX_CROSSINGS) {
            fprintf(stderr, "signal: trace '%s' exceeds MM_SIGNAL_MAX_CROSSINGS (%u)\n",
                    tr->name, MM_SIGNAL_MAX_CROSSINGS);
            free(tmp);
            return MM_FALSE;
        }
        tmp[count].at_ns = signal_timestamp_for_index(tb, i);
        tmp[count].rising = state;
        ++count;
    }

    if (count == 0u) {
        free(tmp);
        b->crossings = 0;
        b->crossing_count = 0;
    } else {
        b->crossings = tmp;
        b->crossing_count = count;
    }
    b->next_crossing = 0;
    return MM_TRUE;
}

int mm_signal_bind(const char *trace_name, int bank, int pin,
                    enum mm_signal_role role)
{
    mm_u32 trace_idx;
    mm_bool found = MM_FALSE;
    struct mm_signal_binding *b;

    if (trace_name == 0 || g_binding_count >= MM_SIGNAL_MAX_BINDINGS) {
        return -1;
    }

    for (trace_idx = 0; trace_idx < g_trace_count; ++trace_idx) {
        if (strncmp(g_traces[trace_idx].name, trace_name, MM_SIGNAL_TRACE_NAME_LEN) == 0) {
            found = MM_TRUE;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "signal: unknown trace '%s'\n", trace_name);
        return -1;
    }

    b = &g_bindings[g_binding_count];
    memset(b, 0, sizeof(*b));
    b->trace_idx = trace_idx;
    b->role = role;
    b->bank = bank;
    b->pin = pin;

    if (role == MM_SIGNAL_ROLE_GPIO) {
        if (!signal_build_gpio_schedule(b, &g_traces[trace_idx])) {
            return -1;
        }
    }

    g_binding_count++;
    return (int)(g_binding_count - 1u);
}

void mm_signal_set_trigger(mm_u32 trigger_pc)
{
    g_trigger_set = MM_TRUE;
    g_trigger_pc = trigger_pc;
    g_triggered = MM_FALSE;
}

mm_bool mm_signal_triggered(void)
{
    return g_triggered;
}

static void signal_dispatch_gpio(struct mm_signal_binding *b, mm_u64 elapsed_ns)
{
    while (b->next_crossing < b->crossing_count &&
           b->crossings[b->next_crossing].at_ns <= elapsed_ns) {
        mm_bool level = b->crossings[b->next_crossing].rising;
        if (level != b->current_level) {
            (void)mm_gpio_set_external_input(b->bank, b->pin, level);
            b->current_level = level;
        }
        b->next_crossing++;
    }
}

mm_bool mm_signal_is_trigger_pc(mm_u32 pc)
{
    return (g_trigger_set && g_trigger_pc == pc) ? MM_TRUE : MM_FALSE;
}

void mm_signal_check_trigger(mm_u32 pc)
{
    if (g_trigger_set && !g_triggered && pc == g_trigger_pc) {
        g_triggered = MM_TRUE;
        g_trigger_ns = signal_now_ns();
    }
}

mm_u64 mm_signal_cycles_until_next_event(void)
{
    mm_u64 best = (mm_u64)-1;
    mm_u32 i;

    if (!g_triggered) {
        return best;
    }
    for (i = 0; i < g_binding_count; ++i) {
        const struct mm_signal_binding *b = &g_bindings[i];
        mm_u64 due;
        mm_u64 done;
        if (b->role != MM_SIGNAL_ROLE_GPIO || b->next_crossing >= b->crossing_count) {
            continue;
        }
        due = g_trigger_ns + b->crossings[b->next_crossing].at_ns;
        due = (due > g_base_ns) ? signal_ns_to_cycles_ceil(due - g_base_ns) : 0u;
        done = g_total_vcycles - g_base_vcycles;
        due = (due > done) ? (due - done) : 0u;
        if (due < best) {
            best = due;
        }
    }
    return best;
}

void mm_signal_advance(mm_u64 cycles)
{
    mm_u32 i;
    mm_u64 now_ns;

    g_total_vcycles += cycles;
    now_ns = signal_now_ns();

    for (i = 0; i < g_binding_count; ++i) {
        struct mm_signal_binding *b = &g_bindings[i];
        if (b->role != MM_SIGNAL_ROLE_GPIO) {
            continue;
        }
        if (g_rewrite_levels) {
            (void)mm_gpio_set_external_input(b->bank, b->pin, b->current_level);
        }
        if (g_triggered) {
            signal_dispatch_gpio(b, now_ns - g_trigger_ns);
        }
    }
    g_rewrite_levels = MM_FALSE;
}
