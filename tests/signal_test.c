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
#include <stdlib.h>
#include "m33mu/signal.h"
#include "m33mu/gpio.h"

/* Mirrors the on-disk layout in src/signal.c exactly (see that file's
 * comments for the format doc reference). Kept local to the test rather
 * than shared via a header, same as the format isn't otherwise exposed
 * outside signal.c. */
struct test_container_header {
    char   magic[4];
    mm_u32 version;
    mm_u32 num_timebases;
    mm_u32 num_traces;
};
struct test_timebase_desc {
    mm_u32 id;
    mm_u32 sample_count;
    mm_u32 period_ns;
};
struct test_trace_desc {
    mm_u32 id;
    char   name[32];
    mm_u32 timebase_id;
    mm_i32 full_scale_mv;
};

static const char *g_tmp_path = "/tmp/signal_test_fixture.sigc";

/* Writes a single uniform-timebase (period_ns=1000) trace named "trace_name"
 * with the given millivolt samples. */
static mm_bool write_fixture(const char *path, const char *trace_name,
                              const mm_i16 *samples, mm_u32 count, mm_u32 period_ns)
{
    FILE *f = fopen(path, "wb");
    struct test_container_header hdr;
    struct test_timebase_desc tb;
    struct test_trace_desc tr;

    if (f == 0) return MM_FALSE;

    memcpy(hdr.magic, "SIGC", 4);
    hdr.version = 1;
    hdr.num_timebases = 1;
    hdr.num_traces = 1;
    fwrite(&hdr, sizeof(hdr), 1, f);

    tb.id = 1;
    tb.sample_count = count;
    tb.period_ns = period_ns;
    fwrite(&tb, sizeof(tb), 1, f);

    memset(&tr, 0, sizeof(tr));
    tr.id = 1;
    snprintf(tr.name, sizeof(tr.name), "%s", trace_name);
    tr.timebase_id = 1;
    tr.full_scale_mv = 3300;
    fwrite(&tr, sizeof(tr), 1, f);

    fwrite(samples, sizeof(mm_i16), count, f);
    fclose(f);
    return MM_TRUE;
}

/* Fake GPIO external-input writer: records the last (bank, pin, level) it
 * was called with, and how many times. */
static int g_write_calls;
static int g_last_bank, g_last_pin;
static mm_bool g_last_level;

static mm_bool fake_gpio_writer(void *opaque, int bank, int pin, mm_bool level)
{
    (void)opaque;
    g_write_calls++;
    g_last_bank = bank;
    g_last_pin = pin;
    g_last_level = level;
    return MM_TRUE;
}

static void reset_fakes(void)
{
    g_write_calls = 0;
    g_last_bank = -1;
    g_last_pin = -1;
    g_last_level = MM_FALSE;
    mm_gpio_set_external_input_writer(fake_gpio_writer, 0);
    mm_signal_reset();
    mm_signal_set_cpu_hz(250000000ull); /* STM32H5F4 nominal, per design doc section 1 */
}

/* --- Test: basic load + bind + a single rising crossing dispatches once --- */
static int test_basic_rising_crossing(void)
{
    /* 3300mV midpoint threshold band: hi=1650+125=1775, lo=1650-125=1525.
     * Trace: 0mV held, then jumps to 3300mV -- one rising crossing. */
    mm_i16 samples[4] = { 0, 0, 3300, 3300 };
    int bid;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "vin", samples, 4, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;

    bid = mm_signal_bind("vin", 0 /* PA */, 5, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP);
    if (bid < 0) return 1;

    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);

    /* Not yet triggered: ticking with an unrelated PC must not fire the
     * binding or write anything. */
    mm_signal_check_trigger(0x08000000u);
    mm_signal_advance(1000);
    if (g_write_calls != 0) return 1;
    if (mm_signal_group_triggered(MM_SIGNAL_MASTER_GROUP)) return 1;

    /* Trigger fires exactly once (decision #10, one-shot). */
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (!mm_signal_group_triggered(MM_SIGNAL_MASTER_GROUP)) return 1;

    /* Advance past the crossing at sample index 2 (t = 2000ns). At
     * 250MHz, 1 cycle = 4ns, so 500 cycles = 2000ns. */
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(500);
    if (g_write_calls != 1) return 1;
    if (g_last_bank != 0 || g_last_pin != 5) return 1;
    if (g_last_level != MM_TRUE) return 1;

    /* One-shot re-arm: hitting the trigger PC again must not re-fire or
     * reset the cursor. */
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (g_write_calls != 1) return 1; /* unchanged */

    return 0;
}

/* --- Test: in-band noise produces zero spurious crossings (decision #17) --- */
static int test_hysteresis_suppresses_inband_noise(void)
{
    /* vdd=3300: hi=1775, lo=1525. Every sample sits inside [1525,1775],
     * wobbling around the naive VDD/2=1650 midpoint. A flat single-
     * threshold comparator would flip state repeatedly; the Schmitt
     * state machine must not. */
    mm_i16 samples[6] = { 1600, 1700, 1600, 1700, 1600, 1700 };
    int bid;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "noisy", samples, 6, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;

    bid = mm_signal_bind("noisy", 1 /* PB */, 2, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP);
    if (bid < 0) return 1;

    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);

    /* Run well past the end of the trace. */
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(100000);

    if (g_write_calls != 0) return 1; /* zero spurious crossings */
    return 0;
}

/* --- Test: external-input write is rejected when pin isn't input mode --- */
static mm_bool fake_gpio_writer_reject(void *opaque, int bank, int pin, mm_bool level)
{
    (void)opaque; (void)bank; (void)pin; (void)level;
    g_write_calls++;
    return MM_FALSE; /* simulates stm32_gpio_set_external_input()'s mode guard */
}

static int test_rejected_write_does_not_crash_or_wedge(void)
{
    mm_i16 samples[2] = { 0, 3300 };
    int bid;

    reset_fakes();
    mm_gpio_set_external_input_writer(fake_gpio_writer_reject, 0);
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "vin2", samples, 2, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;

    bid = mm_signal_bind("vin2", 0, 5, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP);
    if (bid < 0) return 1;
    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1000);

    /* The write attempt happened (writer got called) even though it was
     * rejected -- and critically, nothing crashed or looped. */
    if (g_write_calls < 1) return 1;
    return 0;
}

/* --- Test: unknown trace name fails bind cleanly --- */
static int test_unknown_trace_rejected(void)
{
    mm_i16 samples[1] = { 0 };
    reset_fakes();
    if (!write_fixture(g_tmp_path, "vin3", samples, 1, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("does_not_exist", 0, 0, MM_SIGNAL_ROLE_GPIO, 0) != -1) return 1;
    return 0;
}

/* --- Test: prepare_for_run re-arms without needing to reload the file --- */
static int test_prepare_for_run_rearms(void)
{
    mm_i16 samples[4] = { 0, 0, 3300, 3300 };
    int bid;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "vin4", samples, 4, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    bid = mm_signal_bind("vin4", 0, 5, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP);
    if (bid < 0) return 1;
    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (!mm_signal_group_triggered(MM_SIGNAL_MASTER_GROUP)) return 1;
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(500);
    if (g_write_calls != 1) return 1;

    /* Simulate a soft reset: re-arm without reloading/re-binding. */
    mm_signal_prepare_for_run();
    if (mm_signal_group_triggered(MM_SIGNAL_MASTER_GROUP)) return 1;

    g_write_calls = 0;
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (!mm_signal_group_triggered(MM_SIGNAL_MASTER_GROUP)) return 1;
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(500);
    if (g_write_calls != 1) return 1; /* fires again, cleanly, from t=0 */

    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "basic_rising_crossing", test_basic_rising_crossing },
        { "hysteresis_suppresses_inband_noise", test_hysteresis_suppresses_inband_noise },
        { "rejected_write_does_not_crash_or_wedge", test_rejected_write_does_not_crash_or_wedge },
        { "unknown_trace_rejected", test_unknown_trace_rejected },
        { "prepare_for_run_rearms", test_prepare_for_run_rearms },
    };
    size_t i;
    int failures = 0;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int rc = tests[i].fn();
        printf("%-45s %s\n", tests[i].name, rc == 0 ? "PASS" : "FAIL");
        if (rc != 0) failures++;
    }

    remove(g_tmp_path);
    return (failures == 0) ? 0 : 1;
}
