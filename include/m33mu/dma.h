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

#ifndef M33MU_DMA_H
#define M33MU_DMA_H

#include "types.h"

/*
 * DMA bus-master stub interface. Peripherals can request host-driven transfers.
 */

typedef mm_bool (*mm_dma_request_fn)(void *opaque,
                                     mm_u32 addr,
                                     void *buffer,
                                     size_t length_bytes,
                                     mm_bool write_direction); /* MM_TRUE = write to memory */

struct mm_dma_master {
    mm_dma_request_fn request;
    void *opaque;
};

void mm_dma_master_init(struct mm_dma_master *dma, mm_dma_request_fn request_fn, void *opaque);
mm_bool mm_dma_transfer(struct mm_dma_master *dma, mm_u32 addr, void *buffer, size_t length_bytes, mm_bool write_direction);

#endif /* M33MU_DMA_H */
