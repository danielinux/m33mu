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
#include "m33mu/signal.h"

static int test_defaults(void)
{
    mm_signal_reset();
    if (mm_signal_adc_vref_mv() != 3300) return 1;
    if (mm_signal_adc_noise_lsb() != 0u || mm_signal_adc_seed() != 0u) return 1;
    return 0;
}

static int test_vref_follows_vdd(void)
{
    int rc = 1;
    mm_signal_reset();
    mm_signal_set_vdd_mv(2800);
    if (mm_signal_adc_vref_mv() != 2800) goto out;
    mm_signal_set_adc_vref_mv(2500);
    if (mm_signal_adc_vref_mv() != 2500) goto out;
    mm_signal_set_vdd_mv(3000);
    if (mm_signal_adc_vref_mv() != 2500) goto out;
    mm_signal_set_adc_vref_mv(0);
    if (mm_signal_adc_vref_mv() != 3000) goto out;
    mm_signal_set_adc_vref_mv(1800);
    mm_signal_set_adc_vref_mv(-1);
    if (mm_signal_adc_vref_mv() != 3000) goto out;
    rc = 0;
out:
    mm_signal_reset();
    return rc;
}

static int test_noise_round_trip(void)
{
    int rc = 1;
    mm_signal_reset();
    mm_signal_set_adc_noise(4095u, 0xFFFFFFFFFFFFFFFFull);
    if (mm_signal_adc_noise_lsb() != 4095u) goto out;
    if (mm_signal_adc_seed() != 0xFFFFFFFFFFFFFFFFull) goto out;
    mm_signal_set_adc_noise(3u, 42u);
    if (mm_signal_adc_noise_lsb() != 3u || mm_signal_adc_seed() != 42u) goto out;
    rc = 0;
out:
    mm_signal_reset();
    return rc;
}

static int test_reset_clears(void)
{
    mm_signal_set_vdd_mv(2000);
    mm_signal_set_adc_vref_mv(2500);
    mm_signal_set_adc_noise(2u, 7u);
    mm_signal_reset();
    if (mm_signal_adc_vref_mv() != 3300) return 1;
    if (mm_signal_adc_noise_lsb() != 0u || mm_signal_adc_seed() != 0u) return 1;
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "defaults", test_defaults },
        { "vref_follows_vdd", test_vref_follows_vdd },
        { "noise_round_trip", test_noise_round_trip },
        { "reset_clears", test_reset_clears },
    };
    size_t i;
    int failures = 0;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int rc = tests[i].fn();
        printf("%-45s %s\n", tests[i].name, rc == 0 ? "PASS" : "FAIL");
        if (rc != 0) failures++;
    }
    return (failures == 0) ? 0 : 1;
}
