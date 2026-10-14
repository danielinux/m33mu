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

#include "m33mu/signal_record.h"
#include "m33mu/signal.h"
#include "m33mu/gpio.h"

#define RECORD_LEVEL_UNKNOWN 0xFFu

struct record_watch {
    int bank;
    int pin;
    mm_u8 level;
};

/* A change seen before the trigger fired; its time is known only afterwards. */
struct record_pending {
    mm_u64 now_ns;
    mm_u8 watch;
    mm_u8 level;
};

static struct record_watch g_watches[MM_SIGNAL_RECORD_MAX_WATCHES];
static mm_u32 g_watch_count = 0;
static FILE *g_file = 0;
static char g_path[256];
static struct record_pending *g_pending = 0;
static mm_u32 g_pending_count = 0;
static mm_u32 g_pending_cap = 0;
static mm_bool g_failed = MM_FALSE;

void mm_signal_record_reset(void)
{
    if (g_file != 0) {
        fclose(g_file);
        g_file = 0;
    }
    mm_gpio_set_pad_listener(0, 0);
    free(g_pending);
    g_pending = 0;
    g_pending_count = 0;
    g_pending_cap = 0;
    g_watch_count = 0;
    g_failed = MM_FALSE;
    g_path[0] = '\0';
}

mm_bool mm_signal_record_watch(int bank, int pin)
{
    mm_u32 i;
    for (i = 0; i < g_watch_count; ++i) {
        if (g_watches[i].bank == bank && g_watches[i].pin == pin) {
            return MM_FALSE;
        }
    }
    if (g_watch_count >= MM_SIGNAL_RECORD_MAX_WATCHES) {
        return MM_FALSE;
    }
    g_watches[g_watch_count].bank = bank;
    g_watches[g_watch_count].pin = pin;
    g_watches[g_watch_count].level = RECORD_LEVEL_UNKNOWN;
    g_watch_count++;
    return MM_TRUE;
}

static void record_write_row(mm_i64 t_ns, const struct record_watch *w, mm_u8 level)
{
    char lc = (level == MM_GPIO_LEVEL_Z) ? 'Z' : (char)('0' + level);
    if (fprintf(g_file, "%lld,P%c%d,%c\n", (long long)t_ns, 'A' + w->bank, w->pin, lc) < 0) {
        g_failed = MM_TRUE;
    }
}

static void record_flush_pending(mm_u64 origin_ns)
{
    mm_u32 i;
    for (i = 0; i < g_pending_count; ++i) {
        record_write_row((mm_i64)g_pending[i].now_ns - (mm_i64)origin_ns,
                         &g_watches[g_pending[i].watch], g_pending[i].level);
    }
    g_pending_count = 0;
}

static void record_hold(mm_u64 now_ns, mm_u32 watch, mm_u8 level)
{
    if (g_pending_count == g_pending_cap) {
        mm_u32 cap = (g_pending_cap == 0u) ? 64u : g_pending_cap * 2u;
        struct record_pending *p = (struct record_pending *)realloc(g_pending, cap * sizeof(*p));
        if (p == 0) {
            g_failed = MM_TRUE;
            return;
        }
        g_pending = p;
        g_pending_cap = cap;
    }
    g_pending[g_pending_count].now_ns = now_ns;
    g_pending[g_pending_count].watch = (mm_u8)watch;
    g_pending[g_pending_count].level = level;
    g_pending_count++;
}

static void record_pad_changed(void *opaque, int bank, int pin, mm_u8 level)
{
    mm_u32 i;
    mm_u64 now_ns;
    mm_u64 trigger_ns = 0;
    (void)opaque;

    if (g_file == 0) {
        return;
    }
    for (i = 0; i < g_watch_count; ++i) {
        if (g_watches[i].bank == bank && g_watches[i].pin == pin) {
            break;
        }
    }
    if (i == g_watch_count || g_watches[i].level == level) {
        return;
    }
    g_watches[i].level = level;
    now_ns = mm_signal_now_ns();
    if (!mm_signal_trigger_set()) {
        record_write_row((mm_i64)now_ns, &g_watches[i], level);
        return;
    }
    if (!mm_signal_trigger_ns(&trigger_ns)) {
        record_hold(now_ns, i, level);
        return;
    }
    record_flush_pending(trigger_ns);
    record_write_row((mm_i64)now_ns - (mm_i64)trigger_ns, &g_watches[i], level);
}

mm_bool mm_signal_record_open(const char *path)
{
    if (path == 0 || g_file != 0) {
        return MM_FALSE;
    }
    g_file = fopen(path, "w");
    if (g_file == 0) {
        fprintf(stderr, "signal: cannot open %s for writing\n", path);
        return MM_FALSE;
    }
    snprintf(g_path, sizeof(g_path), "%s", path);
    if (fprintf(g_file, "t_ns,pin,level\n") < 0) {
        g_failed = MM_TRUE;
    }
    mm_gpio_set_pad_listener(record_pad_changed, 0);
    return MM_TRUE;
}

mm_bool mm_signal_record_close(void)
{
    mm_u64 trigger_ns = 0;
    mm_bool ok;

    if (g_file == 0) {
        return MM_TRUE;
    }
    mm_gpio_set_pad_listener(0, 0);
    if (g_pending_count > 0u) {
        if (mm_signal_trigger_ns(&trigger_ns)) {
            record_flush_pending(trigger_ns);
        } else {
            fprintf(stderr, "signal: the trigger was never reached, %s times count from the first instruction\n",
                    g_path);
            record_flush_pending(0u);
        }
    }
    if (fclose(g_file) != 0) {
        g_failed = MM_TRUE;
    }
    g_file = 0;
    ok = g_failed ? MM_FALSE : MM_TRUE;
    if (!ok) {
        fprintf(stderr, "signal: failed to write %s\n", g_path);
    }
    return ok;
}
