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
 *  bits[31:24] = 0xFF (prefix)
 *  bits[23:7]  = RES1
 *  bit6  S     = stack security (1=Secure, 0=Non-secure)
 *  bit5  DCRS  = 1 when default callee stacking applies
 *  bit4  FType = 1 when no FP context stacked
 *  bit3  Mode  = 1 Thread, 0 Handler
 *  bit2  SPSEL = 1 PSP, 0 MSP
 *  bit1  RES0
 *  bit0  ES    = 1 Secure, 0 Non-secure (exception security)
 */

struct mm_exc_return_info mm_exc_return_decode(mm_u32 value)
{
    struct mm_exc_return_info info;
    info.valid = MM_FALSE;
    info.use_psp = MM_FALSE;
    info.default_callee_stacking = MM_TRUE;
    info.basic_frame = MM_TRUE;
    info.to_thread = MM_TRUE;
    info.target_sec = MM_SECURE;
    info.exception_sec = MM_SECURE;
    info.return_sec = MM_SECURE;

    if ((value & 0xffffff00u) != 0xffffff00u) {
        return info;
    }
    if ((value & (1u << 1)) != 0u) {
        return info;
    }

    /* Armv8-M EXC_RETURN: bit6 selects stack security, bit2 selects PSP vs MSP,
       bit4=1 means basic frame (no FP context). */
    info.default_callee_stacking = (value & (1u << 5)) != 0u;
    info.basic_frame = ((value & (1u << 4)) != 0u);
    info.use_psp = (value & (1u << 2)) != 0u;
    /* Bit3 distinguishes Thread (1) vs Handler (0) return (DDI0553 C2.4.5). */
    info.to_thread = (value & (1u << 3)) != 0u;
    /* Bit6 distinguishes Secure(1) from Non-secure(0) stack/return security. */
    info.target_sec = ((value & (1u << 6)) != 0u) ? MM_SECURE : MM_NONSECURE;
    info.return_sec = info.target_sec;
    /* Bit0 records the security state of the exception being returned from. */
    info.exception_sec = (value & 1u) != 0u ? MM_SECURE : MM_NONSECURE;
    info.valid = MM_TRUE;
    return info;
}

mm_u32 mm_exc_return_encode_ex(enum mm_sec_state stack_sec,
                               enum mm_sec_state exception_sec,
                               mm_bool use_psp,
                               mm_bool to_thread,
                               mm_bool basic_frame,
                               mm_bool default_callee_stacking)
{
    mm_u32 value = 0xFFFFFF80u;
    if (stack_sec == MM_SECURE) {
        value |= 1u << 6;
    }
    if (exception_sec == MM_SECURE) {
        value |= 1u;
    }
    if (default_callee_stacking) {
        value |= 1u << 5;
    }
    if (basic_frame) {
        value |= 1u << 4;
    }
    if (to_thread) {
        value |= 1u << 3;
        if (use_psp) {
            value |= 1u << 2;
        }
    }
    return value;
}

mm_u32 mm_exc_return_encode(enum mm_sec_state sec, mm_bool use_psp, mm_bool to_thread)
{
    return mm_exc_return_encode_ex(sec, sec, use_psp, to_thread, MM_TRUE,
                                   MM_TRUE);
}

/* deprecated; kept for compatibility: always false now */
mm_bool exc_return_trace_enabled(void)
{
    return MM_FALSE;
}
