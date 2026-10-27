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

#ifndef M33MU_STM32_DMA_REQ_H
#define M33MU_STM32_DMA_REQ_H

enum stm32_dma_req_result {
    STM32_DMA_REQ_NONE = 0,     /* no ready channel, or the beat failed */
    STM32_DMA_REQ_SERVED = 1,   /* one beat moved */
    STM32_DMA_REQ_LAST = 2      /* one beat moved and the channel completed */
};

#endif /* M33MU_STM32_DMA_REQ_H */
