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

#ifndef M33MU_SIGNAL_RECORD_H
#define M33MU_SIGNAL_RECORD_H

#include "m33mu/types.h"

#define MM_SIGNAL_RECORD_MAX_WATCHES 32u

void mm_signal_record_reset(void);

/* bank: 0=A, 1=B, ...; MM_FALSE if the list is full or the pin is already watched. */
mm_bool mm_signal_record_watch(int bank, int pin);

/* Writes the CSV header and starts listening to pad changes. */
mm_bool mm_signal_record_open(const char *path);

/* Writes rows still held back for the trigger and closes the file. Idempotent. */
mm_bool mm_signal_record_close(void);

#endif /* M33MU_SIGNAL_RECORD_H */
