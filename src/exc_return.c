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

#include "m33mu/exc_return.h"
#include <stdlib.h>

/* EXC_RETURN format (Armv8-M, DDI0553):
 *  bits[31:8] = 0xFFFFFF
 *  bit6 selects target Security state (1=Secure, 0=Non-secure)
 *  bit4 basic frame (no FP state)
 *  bit3 return to Thread(1) vs Handler(0)
 *  bit2 select PSP(1) vs MSP(0) when returning to Thread
 */

struct mm_exc_return_info mm_exc_return_decode(mm_u32 value)
{
    struct mm_exc_return_info info;
    info.valid = MM_FALSE;
    info.use_psp = MM_FALSE;
    info.basic_frame = MM_TRUE;
    info.to_thread = MM_TRUE;
    info.target_sec = MM_SECURE;

    if ((value & 0xffffff00u) != 0xffffff00u) {
        return info;
    }

    /* Armv8-M EXC_RETURN: bit6 selects target Security state, bit2 selects PSP vs MSP,
       bit4=1 means basic frame (no FP context). */
    info.basic_frame = ((value & (1u << 4)) != 0u);
    info.use_psp = (value & (1u << 2)) != 0u;
    /* Bit3 distinguishes Thread (1) vs Handler (0) return (DDI0553 C2.4.5). */
    info.to_thread = (value & (1u << 3)) != 0u;
    /* Bit6 distinguishes Secure(1) from Non-secure(0). */
    info.target_sec = ((value & (1u << 6)) != 0u) ? MM_SECURE : MM_NONSECURE;
    info.valid = MM_TRUE;
    return info;
}

/* deprecated; kept for compatibility: always false now */
mm_bool exc_return_trace_enabled(void)
{
    return MM_FALSE;
}
