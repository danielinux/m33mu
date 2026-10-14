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
    mm_signal_set_cpu_hz(250000000ull);
}

static int test_basic_rising_crossing(void)
{
    mm_i16 samples[4] = { 0, 0, 3300, 3300 };
    int bid;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "vin", samples, 4, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;

    bid = mm_signal_bind("vin", 0 /* PA */, 5, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP);
    if (bid < 0) return 1;

    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);

    mm_signal_check_trigger(0x08000000u);
    mm_signal_advance(1000);
    if (g_write_calls != 0) return 1;
    if (mm_signal_group_triggered(MM_SIGNAL_MASTER_GROUP)) return 1;

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (!mm_signal_group_triggered(MM_SIGNAL_MASTER_GROUP)) return 1;

    /* Crossing at 2000ns = 500 cycles at 250MHz. */
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(500);
    if (g_write_calls != 1) return 1;
    if (g_last_bank != 0 || g_last_pin != 5) return 1;
    if (g_last_level != MM_TRUE) return 1;

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (g_write_calls != 1) return 1;

    return 0;
}

static int test_hysteresis_suppresses_inband_noise(void)
{
    /* All samples inside the 1525..1775mV band at vdd=3300. */
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

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(100000);

    if (g_write_calls != 0) return 1;
    return 0;
}

static mm_bool fake_gpio_writer_reject(void *opaque, int bank, int pin, mm_bool level)
{
    (void)opaque; (void)bank; (void)pin; (void)level;
    g_write_calls++;
    return MM_FALSE;
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

    if (g_write_calls < 1) return 1;
    return 0;
}

static int test_unknown_trace_rejected(void)
{
    mm_i16 samples[1] = { 0 };
    reset_fakes();
    if (!write_fixture(g_tmp_path, "vin3", samples, 1, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("does_not_exist", 0, 0, MM_SIGNAL_ROLE_GPIO, 0) != -1) return 1;
    return 0;
}

static int test_reset_keeps_stimulus_running(void)
{
    mm_i16 samples[5] = { 0, 0, 3300, 3300, 0 };
    int bid;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "vin4", samples, 5, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    bid = mm_signal_bind("vin4", 0, 5, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP);
    if (bid < 0) return 1;
    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(500);
    if (g_write_calls != 1 || g_last_level != MM_TRUE) return 1;

    mm_signal_prepare_for_run();
    if (!mm_signal_group_triggered(MM_SIGNAL_MASTER_GROUP)) return 1;

    /* The GPIO model was reset: the pad level is written again. */
    g_write_calls = 0;
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (g_write_calls != 1 || g_last_level != MM_TRUE) return 1;

    /* No rewind: the falling crossing at 4000ns is 499 cycles away. */
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(498);
    if (g_write_calls != 1) return 1;
    mm_signal_advance(1);
    if (g_write_calls != 2 || g_last_level != MM_FALSE) return 1;

    return 0;
}

static int test_initial_level_applied_before_trigger(void)
{
    mm_i16 samples[3] = { 3300, 3300, 0 };

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "idle_hi", samples, 3, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("idle_hi", 0, 5, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP) < 0) return 1;
    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);
    mm_signal_prepare_for_run();

    mm_signal_check_trigger(0x08000000u);
    mm_signal_advance(1);
    if (g_write_calls != 1 || g_last_level != MM_TRUE) return 1;
    mm_signal_advance(1000);
    if (g_write_calls != 1) return 1;

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(499);
    if (g_write_calls != 1) return 1;
    mm_signal_advance(1);
    if (g_write_calls != 2 || g_last_level != MM_FALSE) return 1;
    return 0;
}

static int test_cycles_until_next_event(void)
{
    mm_i16 samples[4] = { 0, 0, 3300, 3300 };
    mm_u64 until;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "vin6", samples, 4, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("vin6", 0, 5, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP) < 0) return 1;
    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);
    if (mm_signal_cycles_until_next_event() != (mm_u64)-1) return 1;

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    until = mm_signal_cycles_until_next_event();
    if (until != 499u) return 1;
    mm_signal_advance(until - 1u);
    if (g_write_calls != 0) return 1;
    mm_signal_advance(1);
    if (g_write_calls != 1) return 1;
    if (mm_signal_cycles_until_next_event() != (mm_u64)-1) return 1;
    return 0;
}

static int test_trigger_pc_and_all_triggered(void)
{
    mm_i16 samples[2] = { 0, 3300 };

    reset_fakes();
    if (!mm_signal_all_triggered()) return 1;
    if (!write_fixture(g_tmp_path, "vin5", samples, 2, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("vin5", 0, 5, MM_SIGNAL_ROLE_GPIO, MM_SIGNAL_MASTER_GROUP) < 0) return 1;
    if (mm_signal_is_trigger_pc(0x08001000u)) return 1;
    mm_signal_set_master_trigger(MM_SIGNAL_MASTER_GROUP, 0x08001000u);
    if (!mm_signal_is_trigger_pc(0x08001000u)) return 1;
    if (mm_signal_is_trigger_pc(0x08001002u)) return 1;

    mm_signal_check_trigger(0x08000000u);
    mm_signal_advance(10);
    if (mm_signal_all_triggered()) return 1;
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (!mm_signal_all_triggered()) return 1;
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
        { "reset_keeps_stimulus_running", test_reset_keeps_stimulus_running },
        { "trigger_pc_and_all_triggered", test_trigger_pc_and_all_triggered },
        { "initial_level_applied_before_trigger", test_initial_level_applied_before_trigger },
        { "cycles_until_next_event", test_cycles_until_next_event },
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
