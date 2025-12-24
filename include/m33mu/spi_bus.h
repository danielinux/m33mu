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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#ifndef M33MU_SPI_BUS_H
#define M33MU_SPI_BUS_H

#include "types.h"

typedef mm_u8 (*mm_spi_xfer_fn)(void *opaque, mm_u8 out);
typedef void (*mm_spi_end_fn)(void *opaque);
typedef mm_u8 (*mm_spi_cs_level_fn)(void *opaque);

struct mm_spi_device {
    int bus;
    mm_spi_xfer_fn xfer;
    mm_spi_end_fn end;
    mm_spi_cs_level_fn cs_level;
    void *opaque;
};

mm_bool mm_spi_bus_register_device(const struct mm_spi_device *dev);
mm_u8 mm_spi_bus_xfer(int bus, mm_u8 out);
void mm_spi_bus_end(int bus);

#endif /* M33MU_SPI_BUS_H */
