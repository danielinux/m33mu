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
#include <time.h>
#include <unistd.h>
#include <sys/random.h>

#include "m33mu/host_rng.h"

static mm_u64 rng_state;
static mm_u32 rng_seed;
static int rng_seeded;

/* Pick a seed from the host CSPRNG, or from the clock and pid if that is
 * unavailable. The fallback only has to be different between runs; the
 * stream it feeds is not a CSPRNG either way. */
static mm_u32 draw_host_seed(void)
{
    mm_u32 v = 0;
    ssize_t n;

    n = getrandom(&v, sizeof(v), GRND_NONBLOCK);
    if (n != (ssize_t)sizeof(v) || v == 0u) {
        v = (mm_u32)time(0) ^ ((mm_u32)getpid() << 16);
        if (v == 0u) {
            v = 1u;
        }
    }
    return v;
}

void mm_host_rng_seed(mm_u32 seed)
{
    if (seed == 0u) {
        seed = 1u;    /* xorshift64* is dead at zero */
    }
    rng_seed = seed;
    /* Spread a 32-bit seed over the 64-bit state so neighbouring seeds do
     * not produce visibly related streams. */
    rng_state = ((mm_u64)seed << 32) ^ ((mm_u64)seed * 0x9E3779B97F4A7C15ull);
    if (rng_state == 0u) {
        rng_state = 0x9E3779B97F4A7C15ull;
    }
    rng_seeded = 1;
}

static void ensure_seeded(void)
{
    if (!rng_seeded) {
        mm_host_rng_seed(draw_host_seed());
        fprintf(stderr, "[RNG] host seed 0x%08lx (replay with --rng-seed "
                "0x%08lx)\n", (unsigned long)rng_seed, (unsigned long)rng_seed);
    }
}

mm_u32 mm_host_rng_get_seed(void)
{
    ensure_seeded();
    return rng_seed;
}

mm_u32 mm_host_rng_u32(void)
{
    mm_u64 x;

    ensure_seeded();
    x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return (mm_u32)((x * 0x2545F4914F6CDD1Dull) >> 32);
}

void mm_host_rng_bytes(void *buf, size_t len)
{
    mm_u8 *p = (mm_u8 *)buf;
    size_t off = 0;
    mm_u32 word;

    if (p == 0) {
        return;
    }
    while (off < len) {
        size_t chunk = len - off;

        if (chunk > sizeof(word)) {
            chunk = sizeof(word);
        }
        word = mm_host_rng_u32();
        memcpy(p + off, &word, chunk);
        off += chunk;
    }
}
