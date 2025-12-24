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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "m33mu/spi_bus.h"

#define MM_SPI_BUS_DEVICE_MAX 16

static struct mm_spi_device g_spi_devices[MM_SPI_BUS_DEVICE_MAX];
static size_t g_spi_device_count = 0;

mm_bool mm_spi_bus_register_device(const struct mm_spi_device *dev)
{
    if (dev == 0 || dev->xfer == 0) {
        return MM_FALSE;
    }
    if (g_spi_device_count >= MM_SPI_BUS_DEVICE_MAX) {
        return MM_FALSE;
    }
    memcpy(&g_spi_devices[g_spi_device_count], dev, sizeof(*dev));
    g_spi_device_count++;
    return MM_TRUE;
}

static struct mm_spi_device *spi_bus_select(int bus)
{
    size_t i;
    struct mm_spi_device *fallback = 0;
    for (i = 0; i < g_spi_device_count; ++i) {
        struct mm_spi_device *dev = &g_spi_devices[i];
        if (dev->bus != bus) {
            continue;
        }
        if (dev->cs_level == 0) {
            if (fallback == 0) {
                fallback = dev;
            }
            continue;
        }
        if (dev->cs_level(dev->opaque) == 0u) {
            return dev;
        }
    }
    return fallback;
}

mm_u8 mm_spi_bus_xfer(int bus, mm_u8 out)
{
    struct mm_spi_device *dev = spi_bus_select(bus);
    if (dev == 0) {
        return 0xFFu;
    }
    return dev->xfer(dev->opaque, out);
}

void mm_spi_bus_end(int bus)
{
    size_t i;
    for (i = 0; i < g_spi_device_count; ++i) {
        struct mm_spi_device *dev = &g_spi_devices[i];
        if (dev->bus != bus) {
            continue;
        }
        if (dev->end != 0) {
            dev->end(dev->opaque);
        }
    }
}
