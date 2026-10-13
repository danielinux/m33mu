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

#ifndef M33MU_HOST_RNG_H
#define M33MU_HOST_RNG_H

#include "m33mu/types.h"

/*
 * The one source of per-run randomness in the emulator. Every modeled RNG
 * peripheral draws from it, so a run is fully described by its seed: a
 * failure that depends on a particular random stream can be replayed with
 * --rng-seed instead of being chased by repetition. The seed is chosen from
 * the host CSPRNG when none is given and is always announced, so even an
 * unattended CI run records how to reproduce itself.
 *
 * This models a peripheral, it does not secure anything. The generator is a
 * plain xorshift64*, not a CSPRNG, and must never be used for emulator
 * security decisions.
 */

/* Fix the stream. Call before the machine is built; later calls re-seed. */
void mm_host_rng_seed(mm_u32 seed);

/* The seed in use, drawing one if none has been set yet. */
mm_u32 mm_host_rng_get_seed(void);

/* Next word of the stream. Never fails. */
mm_u32 mm_host_rng_u32(void);

/* Fill a buffer from the stream. */
void mm_host_rng_bytes(void *buf, size_t len);

#endif /* M33MU_HOST_RNG_H */
