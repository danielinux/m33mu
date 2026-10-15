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

static const char *g_tmp_path = "/tmp/signal_test_fixture.sigc";

static void put_le(FILE *f, mm_u64 v, int bytes)
{
    int i;
    for (i = 0; i < bytes; ++i) {
        fputc((int)((v >> (8 * i)) & 0xFFu), f);
    }
}

/* One timebase declaring sample_count samples, one trace with count samples. */
static mm_bool write_container(const char *path, mm_u32 version, const char *trace_name,
                               mm_u32 sample_count, mm_u32 period_ns, const mm_u64 *timestamps,
                               const mm_i16 *samples, mm_u32 count, mm_u32 trailing_bytes)
{
    FILE *f = fopen(path, "wb");
    char name[32];
    size_t name_len = strlen(trace_name);
    mm_u32 i;

    if (f == 0) return MM_FALSE;
    fwrite("SIGC", 1, 4, f);
    put_le(f, version, 4);
    put_le(f, 1, 4);
    put_le(f, 1, 4);

    put_le(f, 1, 4);
    put_le(f, sample_count, 4);
    put_le(f, period_ns, 4);
    if (period_ns == 0u) {
        for (i = 0; i < sample_count; ++i) put_le(f, timestamps[i], 8);
    }

    memset(name, 0, sizeof(name));
    memcpy(name, trace_name, name_len < sizeof(name) ? name_len : sizeof(name));
    put_le(f, 1, 4);
    fwrite(name, 1, sizeof(name), f);
    put_le(f, 1, 4);
    put_le(f, 3300, 4);
    for (i = 0; i < count; ++i) put_le(f, (mm_u16)samples[i], 2);
    for (i = 0; i < trailing_bytes; ++i) fputc(0, f);
    fclose(f);
    return MM_TRUE;
}

static mm_bool write_fixture(const char *path, const char *trace_name,
                              const mm_i16 *samples, mm_u32 count, mm_u32 period_ns)
{
    return write_container(path, 1, trace_name, count, period_ns, 0, samples, count, 0);
}

/* One uniform two-sample timebase shared by two traces. */
static mm_bool write_two_traces(const char *path, mm_u32 id_a, const char *name_a,
                                mm_u32 id_b, const char *name_b)
{
    FILE *f = fopen(path, "wb");
    const mm_u32 ids[2] = { id_a, id_b };
    const char *names[2] = { name_a, name_b };
    char name[32];
    int t;

    if (f == 0) return MM_FALSE;
    fwrite("SIGC", 1, 4, f);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 2, 4);
    put_le(f, 1, 4);
    put_le(f, 2, 4);
    put_le(f, 1000, 4);
    for (t = 0; t < 2; ++t) {
        memset(name, 0, sizeof(name));
        snprintf(name, sizeof(name), "%s", names[t]);
        put_le(f, ids[t], 4);
        fwrite(name, 1, sizeof(name), f);
        put_le(f, 1, 4);
        put_le(f, 3300, 4);
        put_le(f, 0, 2);
        put_le(f, 3300, 2);
    }
    fclose(f);
    return MM_TRUE;
}

/* One uniform timebase shared by two traces of count samples each. */
static mm_bool write_two_waveforms(const char *path, const char *name_a, const mm_i16 *samples_a,
                                   const char *name_b, const mm_i16 *samples_b, mm_u32 count,
                                   mm_u32 period_ns)
{
    FILE *f = fopen(path, "wb");
    const char *names[2] = { name_a, name_b };
    const mm_i16 *samples[2] = { samples_a, samples_b };
    char name[32];
    mm_u32 i;
    int t;

    if (f == 0) return MM_FALSE;
    fwrite("SIGC", 1, 4, f);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 2, 4);
    put_le(f, 1, 4);
    put_le(f, count, 4);
    put_le(f, period_ns, 4);
    for (t = 0; t < 2; ++t) {
        memset(name, 0, sizeof(name));
        snprintf(name, sizeof(name), "%s", names[t]);
        put_le(f, (mm_u64)(t + 1), 4);
        fwrite(name, 1, sizeof(name), f);
        put_le(f, 1, 4);
        put_le(f, 3300, 4);
        for (i = 0; i < count; ++i) put_le(f, (mm_u16)samples[t][i], 2);
    }
    fclose(f);
    return MM_TRUE;
}

#define LOG_MAX 16

static int g_write_calls;
static int g_last_bank, g_last_pin;
static mm_bool g_last_level;
static mm_u64 g_last_late;
static int g_log_pin[LOG_MAX];
static mm_bool g_log_level[LOG_MAX];
static mm_u64 g_log_late[LOG_MAX];

static mm_bool fake_gpio_writer(void *opaque, int bank, int pin, mm_bool level, mm_u64 late_cycles)
{
    (void)opaque;
    if (g_write_calls < LOG_MAX) {
        g_log_pin[g_write_calls] = pin;
        g_log_level[g_write_calls] = level;
        g_log_late[g_write_calls] = late_cycles;
    }
    g_write_calls++;
    g_last_bank = bank;
    g_last_pin = pin;
    g_last_level = level;
    g_last_late = late_cycles;
    return MM_TRUE;
}

static void reset_fakes(void)
{
    g_write_calls = 0;
    g_last_bank = -1;
    g_last_pin = -1;
    g_last_level = MM_FALSE;
    g_last_late = 0u;
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

    bid = mm_signal_bind("vin", 0 /* PA */, 5, MM_SIGNAL_ROLE_GPIO);
    if (bid < 0) return 1;

    mm_signal_set_trigger(0x08001000u);

    mm_signal_check_trigger(0x08000000u);
    mm_signal_advance(1000);
    if (g_write_calls != 0) return 1;
    if (mm_signal_triggered()) return 1;

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (!mm_signal_triggered()) return 1;

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

    bid = mm_signal_bind("noisy", 1 /* PB */, 2, MM_SIGNAL_ROLE_GPIO);
    if (bid < 0) return 1;

    mm_signal_set_trigger(0x08001000u);
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(100000);

    if (g_write_calls != 0) return 1;
    return 0;
}

static mm_bool fake_gpio_writer_reject(void *opaque, int bank, int pin, mm_bool level,
                                       mm_u64 late_cycles)
{
    (void)opaque; (void)bank; (void)pin; (void)level; (void)late_cycles;
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

    bid = mm_signal_bind("vin2", 0, 5, MM_SIGNAL_ROLE_GPIO);
    if (bid < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
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
    if (mm_signal_bind("does_not_exist", 0, 0, MM_SIGNAL_ROLE_GPIO) != -1) return 1;
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
    bid = mm_signal_bind("vin4", 0, 5, MM_SIGNAL_ROLE_GPIO);
    if (bid < 0) return 1;
    mm_signal_set_trigger(0x08001000u);

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(500);
    if (g_write_calls != 1 || g_last_level != MM_TRUE) return 1;

    mm_signal_prepare_for_run();
    if (!mm_signal_triggered()) return 1;

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
    if (mm_signal_bind("idle_hi", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
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
    if (mm_signal_bind("vin6", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
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

static int test_clock_change_keeps_elapsed_time(void)
{
    /* Rising crossing at 1.5ms. */
    mm_i16 samples[4] = { 0, 0, 0, 3300 };

    reset_fakes();
    mm_signal_set_cpu_hz(64000000ull);
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "vin7", samples, 4, 500000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("vin7", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);

    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(64000u);
    mm_signal_set_cpu_hz(250000000ull);
    if (mm_signal_cycles_until_next_event() != 125000u) return 1;
    mm_signal_advance(124999u);
    if (g_write_calls != 0) return 1;
    mm_signal_advance(1u);
    if (g_write_calls != 1 || g_last_level != MM_TRUE) return 1;
    return 0;
}

static int test_crossings_across_bindings_in_time_order(void)
{
    /* PA0 rises at 1us and falls at 3us, PA1 rises at 2us and falls at 4us. */
    mm_i16 a[5] = { 0, 3300, 3300, 0, 0 };
    mm_i16 b[5] = { 0, 0, 3300, 3300, 0 };
    const int pins[4] = { 0, 1, 0, 1 };
    const mm_bool levels[4] = { MM_TRUE, MM_TRUE, MM_FALSE, MM_FALSE };
    const mm_u64 lates[4] = { 1000u, 750u, 500u, 250u };
    int i;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_two_waveforms(g_tmp_path, "a", a, "b", b, 5, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("a", 0, 0, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    if (mm_signal_bind("b", 0, 1, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
    mm_signal_check_trigger(0x08001000u);
    if (mm_signal_cycles_until_next_event() != 250u) return 1;

    mm_signal_advance(1250u);
    if (g_write_calls != 4) return 1;
    for (i = 0; i < 4; ++i) {
        if (g_log_pin[i] != pins[i] || g_log_level[i] != levels[i] || g_log_late[i] != lates[i]) {
            return 1;
        }
    }
    if (mm_signal_cycles_until_next_event() != (mm_u64)-1) return 1;
    return 0;
}

static int test_idle_advances_keep_initial_rewrite(void)
{
    mm_i16 samples[3] = { 3300, 3300, 0 };
    int i;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "idle_hi2", samples, 3, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("idle_hi2", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);

    for (i = 0; i < 10; ++i) {
        mm_signal_check_trigger(0x08000000u);
        mm_signal_advance(100u);
    }
    if (g_write_calls != 0) return 1;

    mm_signal_prepare_for_run();
    mm_signal_advance(1u);
    if (g_write_calls != 1 || g_last_level != MM_TRUE || g_last_late != 0u) return 1;
    mm_signal_advance(100u);
    if (g_write_calls != 1) return 1;

    mm_signal_prepare_for_run();
    mm_signal_advance(0u);
    if (g_write_calls != 2 || g_last_level != MM_TRUE) return 1;
    if (mm_signal_triggered()) return 1;
    return 0;
}

static int test_clock_drop_between_crossings(void)
{
    /* Rises at 1ms and falls at 2ms. */
    mm_i16 samples[5] = { 0, 0, 3300, 3300, 0 };

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "vin9", samples, 5, 500000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("vin9", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
    mm_signal_check_trigger(0x08001000u);

    if (mm_signal_cycles_until_next_event() != 250000u) return 1;
    mm_signal_advance(250000u);
    if (g_write_calls != 1 || g_last_level != MM_TRUE || g_last_late != 0u) return 1;

    /* 0.5ms at 250MHz, then the remaining 0.5ms at 64MHz. */
    mm_signal_advance(125000u);
    mm_signal_set_cpu_hz(64000000ull);
    if (mm_signal_cycles_until_next_event() != 32000u) return 1;
    mm_signal_advance(31999u);
    if (g_write_calls != 1) return 1;
    mm_signal_advance(1u);
    if (g_write_calls != 2 || g_last_level != MM_FALSE || g_last_late != 0u) return 1;
    return 0;
}

static int test_trigger_pc_and_triggered(void)
{
    mm_i16 samples[2] = { 0, 3300 };

    reset_fakes();
    if (!write_fixture(g_tmp_path, "vin5", samples, 2, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("vin5", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    if (mm_signal_is_trigger_pc(0x08001000u)) return 1;
    mm_signal_set_trigger(0x08001000u);
    if (!mm_signal_is_trigger_pc(0x08001000u)) return 1;
    if (mm_signal_is_trigger_pc(0x08001002u)) return 1;

    mm_signal_check_trigger(0x08000000u);
    mm_signal_advance(10);
    if (mm_signal_triggered()) return 1;
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(1);
    if (!mm_signal_triggered()) return 1;
    if (mm_signal_is_trigger_pc(0x08001000u)) return 1;
    return 0;
}

static int test_loader_rejects_bad_containers(void)
{
    mm_i16 samples[3] = { 0, 3300, 0 };
    mm_u64 ts_ok[3] = { 0, 1000, 2000 };
    mm_u64 ts_back[3] = { 0, 2000, 1000 };

    reset_fakes();
    if (!write_container(g_tmp_path, 1, "v", 3, 0, ts_ok, samples, 3, 0)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;

    if (!write_container(g_tmp_path, 2, "v", 3, 1000, 0, samples, 3, 0)) return 1;
    if (mm_signal_load(g_tmp_path)) return 1;
    if (!write_container(g_tmp_path, 1, "v", 3, 0, ts_back, samples, 3, 0)) return 1;
    if (mm_signal_load(g_tmp_path)) return 1;
    if (!write_container(g_tmp_path, 1, "v", 0x40000000u, 1000, 0, samples, 3, 0)) return 1;
    if (mm_signal_load(g_tmp_path)) return 1;
    if (!write_container(g_tmp_path, 1, "v", 3, 1000, 0, samples, 3, 1)) return 1;
    if (mm_signal_load(g_tmp_path)) return 1;
    if (!write_two_traces(g_tmp_path, 1, "a", 2, "b")) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (!write_two_traces(g_tmp_path, 1, "a", 2, "a")) return 1;
    if (mm_signal_load(g_tmp_path)) return 1;
    if (!write_two_traces(g_tmp_path, 1, "a", 1, "b")) return 1;
    if (mm_signal_load(g_tmp_path)) return 1;
    return 0;
}

static int test_loader_rejects_unterminated_name(void)
{
    mm_i16 samples[2] = { 0, 3300 };
    const char *name31 = "abcdefghijklmnopqrstuvwxyz01234";
    const char *name32 = "abcdefghijklmnopqrstuvwxyz012345";

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, name31, samples, 2, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind(name31, 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    if (!write_fixture(g_tmp_path, name32, samples, 2, 1000)) return 1;
    if (mm_signal_load(g_tmp_path)) return 1;
    return 0;
}

static int test_long_times_convert_exactly(void)
{
    mm_i16 samples[3] = { 0, 3300, 0 };

    reset_fakes();
    mm_signal_set_cpu_hz(64000001ull);
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "slow", samples, 3, 1500000000u)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("slow", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
    mm_signal_check_trigger(0x08001000u);

    if (mm_signal_cycles_until_next_event() != 96000002u) return 1;
    mm_signal_advance(96000001u);
    if (g_write_calls != 0) return 1;
    mm_signal_advance(1u);
    if (g_write_calls != 1 || g_last_level != MM_TRUE) return 1;
    if (mm_signal_cycles_until_next_event() != 96000001u) return 1;
    return 0;
}

static int test_late_cycles_reported(void)
{
    mm_i16 samples[5] = { 0, 0, 3300, 3300, 0 };
    mm_u64 due;

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "late", samples, 5, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("late", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
    mm_signal_check_trigger(0x08001000u);

    due = mm_signal_cycles_until_next_event();
    mm_signal_advance(due);
    if (g_write_calls != 1 || g_last_level != MM_TRUE || g_last_late != 0u) return 1;
    due = mm_signal_cycles_until_next_event();
    mm_signal_advance(due + 100u);
    if (g_write_calls != 2 || g_last_level != MM_FALSE || g_last_late != 100u) return 1;
    return 0;
}

/* 0 -> 3300 -> 0mV in 100mV steps, one sample per microsecond. */
static int run_ramp(mm_bool set_vil, mm_i32 vil_mv, mm_bool set_vih, mm_i32 vih_mv,
                    mm_u64 *rise_cycles, mm_u64 *fall_cycles)
{
    mm_i16 samples[67];
    mm_u32 i;

    for (i = 0; i < 67u; ++i) {
        samples[i] = (mm_i16)(100 * (i <= 33u ? i : 66u - i));
    }
    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (set_vil) mm_signal_set_gpio_vil_mv(vil_mv);
    if (set_vih) mm_signal_set_gpio_vih_mv(vih_mv);
    if (!write_fixture(g_tmp_path, "ramp", samples, 67, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("ramp", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
    mm_signal_check_trigger(0x08001000u);

    *rise_cycles = mm_signal_cycles_until_next_event();
    mm_signal_advance(*rise_cycles);
    if (g_write_calls != 1 || g_last_level != MM_TRUE) return 1;
    *fall_cycles = mm_signal_cycles_until_next_event();
    mm_signal_advance(*fall_cycles);
    if (g_write_calls != 2 || g_last_level != MM_FALSE) return 1;
    return 0;
}

static int test_thresholds_move_crossings(void)
{
    mm_u64 rise, fall;

    /* Default 1525/1775mV: rise at 1800mV (18us), fall at 1500mV (51us). */
    if (run_ramp(MM_FALSE, 0, MM_FALSE, 0, &rise, &fall) != 0) return 1;
    if (rise != 4500u || fall != 8250u) return 1;

    /* 900/1200mV: rise at 12us, fall at 57us. */
    if (run_ramp(MM_TRUE, 900, MM_TRUE, 1200, &rise, &fall) != 0) return 1;
    if (rise != 3000u || fall != 11250u) return 1;

    /* 2000/2400mV: rise at 24us, fall at 46us. */
    if (run_ramp(MM_TRUE, 2000, MM_TRUE, 2400, &rise, &fall) != 0) return 1;
    if (rise != 6000u || fall != 5500u) return 1;

    /* VIH alone: VIL keeps its 1525mV default, fall at 51us. */
    if (run_ramp(MM_FALSE, 0, MM_TRUE, 2400, &rise, &fall) != 0) return 1;
    if (rise != 6000u || fall != 6750u) return 1;
    return 0;
}

static int test_inverted_thresholds_rejected(void)
{
    mm_i16 samples[2] = { 0, 3300 };

    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    mm_signal_set_gpio_vil_mv(2000);
    mm_signal_set_gpio_vih_mv(2000);
    if (!write_fixture(g_tmp_path, "vin8", samples, 2, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("vin8", 0, 5, MM_SIGNAL_ROLE_GPIO) != -1) return 1;

    /* Effective pair: VIL 1800mV against the 1775mV default VIH. */
    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    mm_signal_set_gpio_vil_mv(1800);
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("vin8", 0, 5, MM_SIGNAL_ROLE_GPIO) != -1) return 1;

    reset_fakes();
    if (mm_signal_gpio_vil_mv() != 1525 || mm_signal_gpio_vih_mv() != 1775) return 1;
    return 0;
}

static int test_many_crossings(void)
{
    static mm_i16 samples[10000];
    mm_u32 i;

    for (i = 0; i < 10000u; ++i) {
        samples[i] = (i & 1u) ? 3300 : 0;
    }
    reset_fakes();
    mm_signal_set_vdd_mv(3300);
    if (!write_fixture(g_tmp_path, "square", samples, 10000, 1000)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("square", 0, 5, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
    mm_signal_check_trigger(0x08001000u);
    mm_signal_advance(250u * 10000u);
    if (g_write_calls != 9999) return 1;
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
        { "trigger_pc_and_triggered", test_trigger_pc_and_triggered },
        { "loader_rejects_bad_containers", test_loader_rejects_bad_containers },
        { "loader_rejects_unterminated_name", test_loader_rejects_unterminated_name },
        { "many_crossings", test_many_crossings },
        { "long_times_convert_exactly", test_long_times_convert_exactly },
        { "late_cycles_reported", test_late_cycles_reported },
        { "initial_level_applied_before_trigger", test_initial_level_applied_before_trigger },
        { "cycles_until_next_event", test_cycles_until_next_event },
        { "clock_change_keeps_elapsed_time", test_clock_change_keeps_elapsed_time },
        { "thresholds_move_crossings", test_thresholds_move_crossings },
        { "inverted_thresholds_rejected", test_inverted_thresholds_rejected },
        { "crossings_across_bindings_in_time_order", test_crossings_across_bindings_in_time_order },
        { "idle_advances_keep_initial_rewrite", test_idle_advances_keep_initial_rewrite },
        { "clock_drop_between_crossings", test_clock_drop_between_crossings },
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
