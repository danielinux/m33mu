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

#include "m33mu/system_reset.h"

static volatile mm_bool g_system_reset_pending = MM_FALSE;
static volatile mm_bool g_boot_override_pending = MM_FALSE;
static volatile mm_u32 g_boot_override_mode = 0u;

void mm_system_request_reset(void)
{
    g_system_reset_pending = MM_TRUE;
}

void mm_system_request_reset_boot(int mode)
{
    g_boot_override_mode = (mm_u32)mode;
    g_boot_override_pending = MM_TRUE;
    g_system_reset_pending = MM_TRUE;
}

mm_bool mm_system_reset_pending(void)
{
    return g_system_reset_pending;
}

void mm_system_clear_reset(void)
{
    g_system_reset_pending = MM_FALSE;
}

mm_bool mm_system_reset_take_boot_mode(mm_u32 *mode_out)
{
    if (!g_boot_override_pending) {
        return MM_FALSE;
    }
    if (mode_out != 0) {
        *mode_out = g_boot_override_mode;
    }
    g_boot_override_pending = MM_FALSE;
    return MM_TRUE;
}
