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

/* --- Container format (see m33mu-signal-injection-gpio.md, section 3) --- */

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
    mm_i32 full_scale_mv; /* provenance metadata only, not consumed here --
                            * see the ADC-phase open item about this field */
};

/* --- Loaded state --- */

struct mm_signal_timebase {
    mm_u32  id;
    mm_u32  sample_count;
    mm_u32  period_ns;    /* 0 if non-uniform */
    mm_u64 *timestamps_ns; /* NULL if uniform (period_ns != 0) */
};

struct mm_signal_trace {
    mm_u32 id;
    char   name[MM_SIGNAL_TRACE_NAME_LEN];
    mm_u32 timebase_id;   /* index into g_timebases, not the on-disk id */
    mm_i16 *samples_mv;
};

/* Precomputed rising/falling crossing, for GPIO bindings only. */
struct mm_signal_crossing {
    mm_u64  at_ns;
    mm_bool rising; /* MM_TRUE = 0->1, MM_FALSE = 1->0 */
};

struct mm_signal_binding {
    mm_bool armed;
    mm_u32  trace_idx;
    mm_u32  group_id;
    enum mm_signal_role role;
    int     bank;
    int     pin;

    mm_u64  elapsed_ns;      /* this binding's replay cursor, recomputed from
                               * vcycles each tick -- see signal_elapsed_ns() */
    mm_u64  trigger_vcycles; /* g_total_vcycles at the moment this binding's
                               * group last fired */
    mm_bool triggered;       /* has this binding's group fired yet */

    /* GPIO role only */
    struct mm_signal_crossing *crossings;
    mm_u32  crossing_count;
    mm_u32  next_crossing; /* index of the next crossing not yet dispatched */
    mm_bool current_level; /* last level written to IDR, to avoid redundant writes */
};

struct mm_signal_master_trigger {
    mm_bool configured;
    mm_bool fired;
    mm_u32  group_id;
    mm_u32  trigger_pc;
};

#define MM_SIGNAL_MAX_TRIGGERS 8u

static struct mm_signal_timebase g_timebases[MM_SIGNAL_MAX_TIMEBASES];
static mm_u32 g_timebase_count = 0;

static struct mm_signal_trace g_traces[MM_SIGNAL_MAX_TRACES];
static mm_u32 g_trace_count = 0;

static struct mm_signal_binding g_bindings[MM_SIGNAL_MAX_BINDINGS];
static mm_u32 g_binding_count = 0;

static struct mm_signal_master_trigger g_triggers[MM_SIGNAL_MAX_TRIGGERS];
static mm_u32 g_trigger_count = 0;

/* Schmitt-trigger thresholds derived from --vdd_mv (decision #17): a
 * fixed 250mV hysteresis band centered at vdd_mv/2, per the STM32H5F4
 * datasheet's typical figure, rather than a percentage-of-VDD guess. */
static mm_i32 g_vdd_mv = 3300;
#define MM_SIGNAL_HYSTERESIS_HALF_MV 125 /* 250mV band / 2 */

/* Cycle -> ns conversion, mirroring src/main.c's deadline_ns() exactly (same
 * __int128 multiply-then-divide, same NS_PER_SEC), since signal replay must
 * stay on the identical timebase the rest of the emulator paces against
 * (section 1 of the design doc). g_total_vcycles accumulates every call to
 * mm_signal_tick(); it is *not* the same counter as main.c's own vcycles
 * (there's no shared-state dependency), it just needs to advance at the
 * same per-instruction rate, which it does since both are incremented once
 * per mm_signal_tick()/mm_timer_tick() call at the same tick site. */
#define MM_SIGNAL_NS_PER_SEC 1000000000ull
#define MM_SIGNAL_DEFAULT_CPU_HZ 64000000ull /* matches MM_CPU_HZ fallback in main.c */

static mm_u64 g_cpu_hz = MM_SIGNAL_DEFAULT_CPU_HZ;
static mm_u64 g_total_vcycles = 0;

void mm_signal_set_cpu_hz(mm_u64 hz)
{
    g_cpu_hz = (hz != 0u) ? hz : MM_SIGNAL_DEFAULT_CPU_HZ;
}

static mm_u64 signal_cycles_to_ns(mm_u64 cycles)
{
    __int128 prod = (__int128)cycles * (__int128)MM_SIGNAL_NS_PER_SEC;
    prod /= (__int128)g_cpu_hz;
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
    g_trigger_count = 0;
}

void mm_signal_reset(void)
{
    signal_free_loaded();
    memset(g_timebases, 0, sizeof(g_timebases));
    memset(g_traces, 0, sizeof(g_traces));
    memset(g_bindings, 0, sizeof(g_bindings));
    memset(g_triggers, 0, sizeof(g_triggers));
    g_vdd_mv = 3300;
    g_total_vcycles = 0;
}

void mm_signal_prepare_for_run(void)
{
    mm_u32 i;
    g_total_vcycles = 0;
    for (i = 0; i < g_trigger_count; ++i) {
        g_triggers[i].fired = MM_FALSE;
    }
    for (i = 0; i < g_binding_count; ++i) {
        g_bindings[i].triggered = MM_FALSE;
        g_bindings[i].elapsed_ns = 0;
        g_bindings[i].trigger_vcycles = 0;
        g_bindings[i].next_crossing = 0;
        g_bindings[i].current_level = MM_FALSE;
    }
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

/* --- Container loading --- */

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
        tr->timebase_id = tb_idx;
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

/* --- Zero-order-hold sample lookup (design doc section 3.2) --- */

static mm_i16 signal_sample_at(const struct mm_signal_trace *tr, mm_u64 elapsed_ns)
{
    const struct mm_signal_timebase *tb = &g_timebases[tr->timebase_id];
    mm_u32 n = tb->sample_count;

    if (n == 0u) {
        return 0;
    }
    if (tb->period_ns != 0u) {
        /* Uniform timebase: O(1) index instead of a binary search. */
        mm_u64 idx = elapsed_ns / (mm_u64)tb->period_ns;
        if (idx >= (mm_u64)n) {
            idx = (mm_u64)n - 1u;
        }
        return tr->samples_mv[idx];
    }

    /* Non-uniform: binary search for the most recent sample at or before
     * elapsed_ns. */
    {
        mm_u32 lo = 0, hi = n;
        if (elapsed_ns < tb->timestamps_ns[0]) {
            return tr->samples_mv[0];
        }
        while (lo + 1u < hi) {
            mm_u32 mid = lo + (hi - lo) / 2u;
            if (tb->timestamps_ns[mid] <= elapsed_ns) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return tr->samples_mv[lo];
    }
}

/* --- Binding / crossing-schedule setup --- */

static mm_u64 signal_timestamp_for_index(const struct mm_signal_timebase *tb, mm_u32 idx)
{
    if (tb->period_ns != 0u) {
        return (mm_u64)tb->period_ns * (mm_u64)idx;
    }
    return tb->timestamps_ns[idx];
}

/* Walks the trace once, building the Schmitt-trigger crossing schedule
 * for a GPIO binding (design doc section 4.1). Returns MM_FALSE if the
 * trace would need more crossings than MM_SIGNAL_MAX_CROSSINGS. */
static mm_bool signal_build_gpio_schedule(struct mm_signal_binding *b,
                                           const struct mm_signal_trace *tr)
{
    const struct mm_signal_timebase *tb = &g_timebases[tr->timebase_id];
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
        /* Deliberately not shrunk with realloc(): the max-size buffer is
         * at most MM_SIGNAL_MAX_CROSSINGS * sizeof(crossing) ~= 48KB per
         * GPIO binding, trivial for this emulator's use case, and avoids
         * the realloc-shrink-failure edge case entirely rather than
         * working around it. */
        b->crossings = tmp;
        b->crossing_count = count;
    }
    b->next_crossing = 0;
    return MM_TRUE;
}

int mm_signal_bind(const char *trace_name, int bank, int pin,
                    enum mm_signal_role role, mm_u32 group_id)
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
    b->armed = MM_TRUE;
    b->trace_idx = trace_idx;
    b->group_id = group_id;
    b->role = role;
    b->bank = bank;
    b->pin = pin;
    b->elapsed_ns = 0;
    b->triggered = MM_FALSE;

    if (role == MM_SIGNAL_ROLE_GPIO) {
        if (!signal_build_gpio_schedule(b, &g_traces[trace_idx])) {
            return -1;
        }
    }

    g_binding_count++;
    return (int)(g_binding_count - 1u);
}

/* --- Master trigger (decisions #7-#10) --- */

void mm_signal_set_master_trigger(mm_u32 group_id, mm_u32 trigger_pc)
{
    mm_u32 i;
    for (i = 0; i < g_trigger_count; ++i) {
        if (g_triggers[i].group_id == group_id) {
            g_triggers[i].trigger_pc = trigger_pc;
            g_triggers[i].configured = MM_TRUE;
            g_triggers[i].fired = MM_FALSE;
            return;
        }
    }
    if (g_trigger_count >= MM_SIGNAL_MAX_TRIGGERS) {
        fprintf(stderr, "signal: too many master triggers configured\n");
        return;
    }
    g_triggers[g_trigger_count].configured = MM_TRUE;
    g_triggers[g_trigger_count].fired = MM_FALSE;
    g_triggers[g_trigger_count].group_id = group_id;
    g_triggers[g_trigger_count].trigger_pc = trigger_pc;
    g_trigger_count++;
}

static void signal_group_trigger(mm_u32 group_id)
{
    mm_u32 i;
    for (i = 0; i < g_binding_count; ++i) {
        if (g_bindings[i].group_id == group_id) {
            g_bindings[i].elapsed_ns = 0;
            g_bindings[i].trigger_vcycles = g_total_vcycles;
            g_bindings[i].triggered = MM_TRUE;
            g_bindings[i].next_crossing = 0;
        }
    }
}

mm_bool mm_signal_group_triggered(mm_u32 group_id)
{
    mm_u32 i;
    for (i = 0; i < g_binding_count; ++i) {
        if (g_bindings[i].group_id == group_id) {
            return g_bindings[i].triggered;
        }
    }
    return MM_FALSE;
}

/* --- Per-instruction tick --- */

static void signal_dispatch_gpio(struct mm_signal_binding *b)
{
    while (b->next_crossing < b->crossing_count &&
           b->crossings[b->next_crossing].at_ns <= b->elapsed_ns) {
        mm_bool level = b->crossings[b->next_crossing].rising;
        if (level != b->current_level) {
            /* No-op (returns MM_FALSE) if the pin isn't currently
             * configured as input -- see stm32_gpio_set_external_input()
             * and the gpio-input-mode-guard test in the design doc. */
            (void)mm_gpio_set_external_input(b->bank, b->pin, level);
            b->current_level = level;
        }
        b->next_crossing++;
    }
}

void mm_signal_check_trigger(mm_u32 pc)
{
    mm_u32 i;
    for (i = 0; i < g_trigger_count; ++i) {
        if (g_triggers[i].configured && !g_triggers[i].fired &&
            pc == g_triggers[i].trigger_pc) {
            g_triggers[i].fired = MM_TRUE; /* one-shot: no re-arm (decision #10) */
            signal_group_trigger(g_triggers[i].group_id);
        }
    }
}

void mm_signal_advance(mm_u64 cycles)
{
    mm_u32 i;

    g_total_vcycles += cycles;

    for (i = 0; i < g_binding_count; ++i) {
        struct mm_signal_binding *b = &g_bindings[i];
        if (!b->armed || !b->triggered) {
            continue;
        }
        /* Recomputed fresh from vcycles each call (not accumulated
         * cycle-by-cycle in ns units) to avoid rounding drift across many
         * small conversions -- matches deadline_ns()'s own approach. */
        b->elapsed_ns = signal_cycles_to_ns(g_total_vcycles - b->trigger_vcycles);
        if (b->role == MM_SIGNAL_ROLE_GPIO) {
            signal_dispatch_gpio(b);
        }
    }
    (void)signal_sample_at; /* reserved for the ADC phase's mm_signal_sample() */
}
