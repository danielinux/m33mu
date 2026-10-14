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
#define MM_SIGNAL_VERSION 1u

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

/* Default band: the STM32H5 typical 250mV hysteresis, centered at VDD/2. */
static mm_i32 g_vdd_mv = 3300;
static mm_bool g_has_vil = MM_FALSE;
static mm_bool g_has_vih = MM_FALSE;
static mm_i32 g_vil_mv = 0;
static mm_i32 g_vih_mv = 0;
#define MM_SIGNAL_HYSTERESIS_HALF_MV 125

#define MM_SIGNAL_NS_PER_SEC 1000000000ull
#define MM_SIGNAL_DEFAULT_CPU_HZ 64000000ull

static mm_u64 g_cpu_hz = MM_SIGNAL_DEFAULT_CPU_HZ;
static mm_u64 g_total_vcycles = 0;
/* Stimulus time is g_base_ns at g_base_vcycles, the last clock change. */
static mm_u64 g_base_vcycles = 0;
static mm_u64 g_base_ns = 0;
static mm_bool g_rewrite_levels = MM_FALSE;

/* Split at whole seconds so the products fit in 64 bits for any clock below 18 GHz. */
static mm_u64 signal_cycles_to_ns(mm_u64 cycles)
{
    return (cycles / g_cpu_hz) * MM_SIGNAL_NS_PER_SEC +
           ((cycles % g_cpu_hz) * MM_SIGNAL_NS_PER_SEC) / g_cpu_hz;
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
    return (ns / MM_SIGNAL_NS_PER_SEC) * g_cpu_hz +
           ((ns % MM_SIGNAL_NS_PER_SEC) * g_cpu_hz + MM_SIGNAL_NS_PER_SEC - 1u) /
               MM_SIGNAL_NS_PER_SEC;
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
    g_has_vil = MM_FALSE;
    g_has_vih = MM_FALSE;
    g_vil_mv = 0;
    g_vih_mv = 0;
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

void mm_signal_set_gpio_vil_mv(mm_i32 vil_mv)
{
    g_has_vil = MM_TRUE;
    g_vil_mv = vil_mv;
}

void mm_signal_set_gpio_vih_mv(mm_i32 vih_mv)
{
    g_has_vih = MM_TRUE;
    g_vih_mv = vih_mv;
}

mm_i32 mm_signal_gpio_vih_mv(void)
{
    return g_has_vih ? g_vih_mv : (g_vdd_mv / 2) + MM_SIGNAL_HYSTERESIS_HALF_MV;
}

mm_i32 mm_signal_gpio_vil_mv(void)
{
    return g_has_vil ? g_vil_mv : (g_vdd_mv / 2) - MM_SIGNAL_HYSTERESIS_HALF_MV;
}

struct signal_reader {
    FILE  *f;
    mm_u64 remaining;
};

static mm_bool signal_read_bytes(struct signal_reader *r, void *buf, mm_u64 n)
{
    if (n > r->remaining || fread(buf, 1u, (size_t)n, r->f) != (size_t)n) {
        return MM_FALSE;
    }
    r->remaining -= n;
    return MM_TRUE;
}

static mm_bool signal_read_le(struct signal_reader *r, mm_u64 *out, int bytes)
{
    mm_u8 buf[8];
    int i;

    if (!signal_read_bytes(r, buf, (mm_u64)bytes)) {
        return MM_FALSE;
    }
    *out = 0;
    for (i = bytes - 1; i >= 0; --i) {
        *out = (*out << 8) | buf[i];
    }
    return MM_TRUE;
}

static mm_bool signal_read_u32(struct signal_reader *r, mm_u32 *out)
{
    mm_u64 v;
    if (!signal_read_le(r, &v, 4)) {
        return MM_FALSE;
    }
    *out = (mm_u32)v;
    return MM_TRUE;
}

static mm_bool signal_load_timebase(struct signal_reader *r, const char *path, mm_u32 i)
{
    struct mm_signal_timebase *tb = &g_timebases[i];
    mm_u32 j;

    if (!signal_read_u32(r, &tb->id) || !signal_read_u32(r, &tb->sample_count) ||
        !signal_read_u32(r, &tb->period_ns)) {
        fprintf(stderr, "signal: %s: short read on timebase %u\n", path, i);
        return MM_FALSE;
    }
    for (j = 0; j < i; ++j) {
        if (g_timebases[j].id == tb->id) {
            fprintf(stderr, "signal: %s: duplicate timebase id %u\n", path, tb->id);
            return MM_FALSE;
        }
    }
    if (tb->period_ns != 0u) {
        return MM_TRUE;
    }
    if (tb->sample_count == 0u) {
        fprintf(stderr, "signal: %s: timebase %u is non-uniform with 0 samples\n", path, i);
        return MM_FALSE;
    }
    if ((mm_u64)tb->sample_count * 8u > r->remaining) {
        fprintf(stderr, "signal: %s: timebase %u is truncated\n", path, i);
        return MM_FALSE;
    }
    tb->timestamps_ns = (mm_u64 *)malloc(sizeof(mm_u64) * (size_t)tb->sample_count);
    if (tb->timestamps_ns == 0) {
        fprintf(stderr, "signal: %s: out of memory (timebase %u timestamps)\n", path, i);
        return MM_FALSE;
    }
    for (j = 0; j < tb->sample_count; ++j) {
        if (!signal_read_le(r, &tb->timestamps_ns[j], 8)) {
            fprintf(stderr, "signal: %s: timebase %u is truncated\n", path, i);
            return MM_FALSE;
        }
        if (j > 0u && tb->timestamps_ns[j] < tb->timestamps_ns[j - 1u]) {
            fprintf(stderr, "signal: %s: timebase %u timestamps go backwards at sample %u\n",
                    path, i, j);
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static mm_bool signal_load_trace(struct signal_reader *r, const char *path, mm_u32 i)
{
    struct mm_signal_trace *tr = &g_traces[i];
    mm_u32 timebase_id;
    mm_u32 full_scale_mv;
    mm_u32 n;
    mm_u32 j;

    if (!signal_read_u32(r, &tr->id) ||
        !signal_read_bytes(r, tr->name, MM_SIGNAL_TRACE_NAME_LEN) ||
        !signal_read_u32(r, &timebase_id) || !signal_read_u32(r, &full_scale_mv)) {
        fprintf(stderr, "signal: %s: short read on trace %u\n", path, i);
        return MM_FALSE;
    }
    if (memchr(tr->name, '\0', MM_SIGNAL_TRACE_NAME_LEN) == 0) {
        fprintf(stderr, "signal: %s: trace %u name is not NUL-terminated\n", path, i);
        return MM_FALSE;
    }
    for (j = 0; j < i; ++j) {
        if (g_traces[j].id == tr->id) {
            fprintf(stderr, "signal: %s: duplicate trace id %u\n", path, tr->id);
            return MM_FALSE;
        }
        if (strcmp(g_traces[j].name, tr->name) == 0) {
            fprintf(stderr, "signal: %s: duplicate trace name '%s'\n", path, tr->name);
            return MM_FALSE;
        }
    }
    for (j = 0; j < g_timebase_count; ++j) {
        if (g_timebases[j].id == timebase_id) {
            break;
        }
    }
    if (j == g_timebase_count) {
        fprintf(stderr, "signal: %s: trace %u references unknown timebase id %u\n",
                path, i, timebase_id);
        return MM_FALSE;
    }
    tr->timebase_idx = j;
    n = g_timebases[j].sample_count;
    if (n == 0u) {
        return MM_TRUE;
    }
    if ((mm_u64)n * 2u > r->remaining) {
        fprintf(stderr, "signal: %s: trace %u is truncated\n", path, i);
        return MM_FALSE;
    }
    tr->samples_mv = (mm_i16 *)malloc(sizeof(mm_i16) * (size_t)n);
    if (tr->samples_mv == 0) {
        fprintf(stderr, "signal: %s: out of memory (trace %u samples)\n", path, i);
        return MM_FALSE;
    }
    for (j = 0; j < n; ++j) {
        mm_u64 v = 0;
        if (!signal_read_le(r, &v, 2)) {
            fprintf(stderr, "signal: %s: trace %u is truncated\n", path, i);
            return MM_FALSE;
        }
        tr->samples_mv[j] = (mm_i16)(mm_u16)v;
    }
    return MM_TRUE;
}

mm_bool mm_signal_load(const char *path)
{
    struct signal_reader r;
    char magic[4];
    mm_u32 version;
    mm_u32 num_timebases;
    mm_u32 num_traces;
    long size;
    mm_u32 i;

    if (path == 0) {
        return MM_FALSE;
    }
    r.f = fopen(path, "rb");
    if (r.f == 0) {
        fprintf(stderr, "signal: failed to open %s\n", path);
        return MM_FALSE;
    }
    if (fseek(r.f, 0, SEEK_END) != 0 || (size = ftell(r.f)) < 0 ||
        fseek(r.f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "signal: %s: cannot read file size\n", path);
        fclose(r.f);
        return MM_FALSE;
    }
    r.remaining = (mm_u64)size;

    if (!signal_read_bytes(&r, magic, 4) || !signal_read_u32(&r, &version) ||
        !signal_read_u32(&r, &num_timebases) || !signal_read_u32(&r, &num_traces)) {
        fprintf(stderr, "signal: %s: short read on header\n", path);
        fclose(r.f);
        return MM_FALSE;
    }
    if (memcmp(magic, MM_SIGNAL_MAGIC, 4) != 0) {
        fprintf(stderr, "signal: %s: bad magic\n", path);
        fclose(r.f);
        return MM_FALSE;
    }
    if (version != MM_SIGNAL_VERSION) {
        fprintf(stderr, "signal: %s: unsupported version %u\n", path, version);
        fclose(r.f);
        return MM_FALSE;
    }
    if (num_timebases > MM_SIGNAL_MAX_TIMEBASES || num_traces > MM_SIGNAL_MAX_TRACES) {
        fprintf(stderr, "signal: %s: exceeds timebase/trace limits (%u/%u)\n",
                path, num_timebases, num_traces);
        fclose(r.f);
        return MM_FALSE;
    }

    signal_free_loaded();
    for (i = 0; i < num_timebases; ++i) {
        g_timebase_count = i + 1u;
        if (!signal_load_timebase(&r, path, i)) {
            goto fail;
        }
    }
    for (i = 0; i < num_traces; ++i) {
        g_trace_count = i + 1u;
        if (!signal_load_trace(&r, path, i)) {
            goto fail;
        }
    }
    if (r.remaining != 0u) {
        fprintf(stderr, "signal: %s: trailing data after the last trace\n", path);
        goto fail;
    }
    fclose(r.f);
    return MM_TRUE;

fail:
    fclose(r.f);
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

/* Counts the trace's crossings, and stores them if out is not null. */
static mm_u32 signal_walk_crossings(const struct mm_signal_trace *tr,
                                    struct mm_signal_crossing *out)
{
    const struct mm_signal_timebase *tb = &g_timebases[tr->timebase_idx];
    mm_i32 hi_thr = mm_signal_gpio_vih_mv();
    mm_i32 lo_thr = mm_signal_gpio_vil_mv();
    mm_bool state = (tr->samples_mv[0] >= hi_thr) ? MM_TRUE : MM_FALSE;
    mm_u32 count = 0;
    mm_u32 i;

    for (i = 1; i < tb->sample_count; ++i) {
        mm_i16 v = tr->samples_mv[i];
        if (!state && v >= hi_thr) {
            state = MM_TRUE;
        } else if (state && v <= lo_thr) {
            state = MM_FALSE;
        } else {
            continue;
        }
        if (out != 0) {
            out[count].at_ns = signal_timestamp_for_index(tb, i);
            out[count].rising = state;
        }
        ++count;
    }
    return count;
}

static mm_bool signal_build_gpio_schedule(struct mm_signal_binding *b,
                                           const struct mm_signal_trace *tr)
{
    mm_u32 count;

    b->crossings = 0;
    b->crossing_count = 0;
    b->next_crossing = 0;
    b->current_level = MM_FALSE;
    if (g_timebases[tr->timebase_idx].sample_count == 0u) {
        return MM_TRUE;
    }
    b->current_level = (tr->samples_mv[0] >= mm_signal_gpio_vih_mv()) ? MM_TRUE : MM_FALSE;
    count = signal_walk_crossings(tr, 0);
    if (count == 0u) {
        return MM_TRUE;
    }
    b->crossings = (struct mm_signal_crossing *)malloc(sizeof(struct mm_signal_crossing) *
                                                        (size_t)count);
    if (b->crossings == 0) {
        fprintf(stderr, "signal: out of memory building crossing schedule\n");
        return MM_FALSE;
    }
    b->crossing_count = signal_walk_crossings(tr, b->crossings);
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
    if (role == MM_SIGNAL_ROLE_GPIO && mm_signal_gpio_vil_mv() >= mm_signal_gpio_vih_mv()) {
        fprintf(stderr, "signal: GPIO VIL (%d mV) must be below VIH (%d mV)\n",
                (int)mm_signal_gpio_vil_mv(), (int)mm_signal_gpio_vih_mv());
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
    return (g_trigger_set && !g_triggered && g_trigger_pc == pc) ? MM_TRUE : MM_FALSE;
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
    if (g_binding_count == 0u) {
        return;
    }
    now_ns = g_triggered ? signal_now_ns() : 0u;

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
