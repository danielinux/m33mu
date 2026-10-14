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

#include "m33mu/signal.h"
#include "m33mu/signal_record.h"
#include "m33mu/gpio.h"

static const char *g_csv_path = "/tmp/signal_record_test.csv";

static void start(void)
{
    mm_signal_reset();
    mm_signal_record_reset();
    mm_signal_set_cpu_hz(1000000000ull);
}

static int csv_equals(const char *expected)
{
    char buf[1024];
    size_t n;
    FILE *f = fopen(g_csv_path, "r");
    if (f == 0) return 0;
    n = fread(buf, 1, sizeof(buf) - 1u, f);
    fclose(f);
    buf[n] = '\0';
    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "got:\n%s\nexpected:\n%s\n", buf, expected);
        return 0;
    }
    return 1;
}

static int test_rows_without_trigger(void)
{
    start();
    if (!mm_signal_record_watch(1, 0)) return 1;
    if (!mm_signal_record_open(g_csv_path)) return 1;
    mm_gpio_notify_pad(1, 0, MM_GPIO_LEVEL_Z);
    mm_gpio_notify_pad(1, 0, MM_GPIO_LEVEL_Z);
    mm_gpio_notify_pad(1, 1, 1u);
    mm_signal_advance(100u);
    mm_gpio_notify_pad(1, 0, 1u);
    mm_signal_advance(50u);
    mm_gpio_notify_pad(1, 0, 0u);
    if (!mm_signal_record_close()) return 1;
    if (!csv_equals("t_ns,pin,level\n0,PB0,Z\n100,PB0,1\n150,PB0,0\n")) return 1;
    return 0;
}

static int test_rows_before_trigger_are_negative(void)
{
    start();
    mm_signal_set_trigger(0x1000u);
    if (!mm_signal_record_watch(1, 0)) return 1;
    if (!mm_signal_record_watch(2, 7)) return 1;
    if (!mm_signal_record_open(g_csv_path)) return 1;
    mm_gpio_notify_pad(1, 0, MM_GPIO_LEVEL_Z);
    mm_gpio_notify_pad(2, 7, MM_GPIO_LEVEL_Z);
    mm_signal_advance(100u);
    mm_gpio_notify_pad(1, 0, 0u);
    mm_signal_advance(400u);
    mm_signal_check_trigger(0x1000u);
    mm_gpio_notify_pad(2, 7, 1u);
    mm_signal_advance(25u);
    mm_gpio_notify_pad(1, 0, 1u);
    if (!mm_signal_record_close()) return 1;
    if (!csv_equals("t_ns,pin,level\n-500,PB0,Z\n-500,PC7,Z\n-400,PB0,0\n"
                    "0,PC7,1\n25,PB0,1\n")) return 1;
    return 0;
}

static int test_held_rows_written_at_close(void)
{
    start();
    mm_signal_set_trigger(0x1000u);
    if (!mm_signal_record_watch(0, 3)) return 1;
    if (!mm_signal_record_open(g_csv_path)) return 1;
    mm_gpio_notify_pad(0, 3, MM_GPIO_LEVEL_Z);
    mm_signal_advance(10u);
    mm_signal_check_trigger(0x1000u);
    mm_signal_advance(10u);
    if (!mm_signal_record_close()) return 1;
    if (!csv_equals("t_ns,pin,level\n-10,PA3,Z\n")) return 1;
    return 0;
}

static int test_unreached_trigger_counts_from_start(void)
{
    start();
    mm_signal_set_trigger(0x1000u);
    if (!mm_signal_record_watch(0, 3)) return 1;
    if (!mm_signal_record_open(g_csv_path)) return 1;
    mm_gpio_notify_pad(0, 3, MM_GPIO_LEVEL_Z);
    mm_signal_advance(100u);
    mm_gpio_notify_pad(0, 3, 1u);
    if (!mm_signal_record_close()) return 1;
    if (!csv_equals("t_ns,pin,level\n0,PA3,Z\n100,PA3,1\n")) return 1;
    return 0;
}

static int test_watch_list_limits(void)
{
    mm_u32 i;
    start();
    if (!mm_signal_record_watch(0, 0)) return 1;
    if (mm_signal_record_watch(0, 0)) return 1;
    mm_signal_record_reset();
    for (i = 0; i < MM_SIGNAL_RECORD_MAX_WATCHES; ++i) {
        if (!mm_signal_record_watch((int)(i / 16u), (int)(i % 16u))) return 1;
    }
    if (mm_signal_record_watch(5, 0)) return 1;
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "rows_without_trigger", test_rows_without_trigger },
        { "rows_before_trigger_are_negative", test_rows_before_trigger_are_negative },
        { "held_rows_written_at_close", test_held_rows_written_at_close },
        { "unreached_trigger_counts_from_start", test_unreached_trigger_counts_from_start },
        { "watch_list_limits", test_watch_list_limits },
    };
    size_t i;
    int failures = 0;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int rc = tests[i].fn();
        printf("%-45s %s\n", tests[i].name, rc == 0 ? "PASS" : "FAIL");
        if (rc != 0) failures++;
    }
    mm_signal_record_reset();
    remove(g_csv_path);
    return (failures == 0) ? 0 : 1;
}
