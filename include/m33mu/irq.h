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

#ifndef M33MU_IRQ_H
#define M33MU_IRQ_H

#include "types.h"

/*
 * IRQ line abstraction. Allows peripherals to raise/lower an interrupt signal.
 */

typedef void (*mm_irq_sink_fn)(void *opaque, mm_bool level);

struct mm_irq_line {
    mm_irq_sink_fn sink;
    void *opaque;
    mm_bool level;
};

void mm_irq_line_init(struct mm_irq_line *line, mm_irq_sink_fn sink, void *opaque);
void mm_irq_line_raise(struct mm_irq_line *line);
void mm_irq_line_lower(struct mm_irq_line *line);
mm_bool mm_irq_line_level(const struct mm_irq_line *line);

#endif /* M33MU_IRQ_H */
