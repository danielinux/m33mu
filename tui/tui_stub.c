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

#include "tui.h"

#ifndef M33MU_HAS_NCURSES
mm_bool mm_tui_init(struct mm_tui *tui)
{
    (void)tui;
    return MM_FALSE;
}

void mm_tui_shutdown(struct mm_tui *tui)
{
    (void)tui;
}

mm_bool mm_tui_redirect_stdio(struct mm_tui *tui)
{
    (void)tui;
    return MM_FALSE;
}

void mm_tui_poll(struct mm_tui *tui)
{
    (void)tui;
}

mm_bool mm_tui_should_quit(const struct mm_tui *tui)
{
    (void)tui;
    return MM_FALSE;
}

mm_u32 mm_tui_take_actions(struct mm_tui *tui)
{
    (void)tui;
    return 0;
}

mm_u8 mm_tui_window1_mode(const struct mm_tui *tui)
{
    (void)tui;
    return MM_TUI_WIN1_LOG;
}

void mm_tui_set_target_running(struct mm_tui *tui, mm_bool running)
{
    (void)tui;
    (void)running;
}

void mm_tui_set_gdb_status(struct mm_tui *tui, mm_bool connected, int port)
{
    (void)tui;
    (void)connected;
    (void)port;
}

void mm_tui_set_capstone(struct mm_tui *tui, mm_bool supported, mm_bool enabled)
{
    (void)tui;
    (void)supported;
    (void)enabled;
}

void mm_tui_set_image0(struct mm_tui *tui, const char *path)
{
    (void)tui;
    (void)path;
}

void mm_tui_set_core_state(struct mm_tui *tui,
                           mm_u32 pc,
                           mm_u32 sp,
                           mm_u8 sec_state,
                           mm_u8 mode,
                           mm_u64 steps)
{
    (void)tui;
    (void)pc;
    (void)sp;
    (void)sec_state;
    (void)mode;
    (void)steps;
}

void mm_tui_set_registers(struct mm_tui *tui, const struct mm_cpu *cpu)
{
    (void)tui;
    (void)cpu;
}

void mm_tui_close_devices(struct mm_tui *tui)
{
    (void)tui;
}

mm_bool mm_tui_start_thread(struct mm_tui *tui)
{
    (void)tui;
    return MM_FALSE;
}

void mm_tui_stop_thread(struct mm_tui *tui)
{
    (void)tui;
}

void mm_tui_register(struct mm_tui *tui)
{
    (void)tui;
}

mm_bool mm_tui_is_active(void)
{
    return MM_FALSE;
}

void mm_tui_attach_uart(const char *label, const char *path)
{
    (void)label;
    (void)path;
}
#endif
