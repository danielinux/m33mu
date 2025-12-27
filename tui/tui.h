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

#ifndef M33MU_TUI_H
#define M33MU_TUI_H

#include "m33mu/types.h"
#include "m33mu/cpu.h"

struct mm_memmap;

struct mm_tui {
    volatile mm_bool active;
    volatile mm_bool want_quit;
    volatile mm_bool target_running;
    volatile mm_bool gdb_connected;
    volatile int gdb_port;
    volatile mm_u8 window1_mode;
    volatile mm_u8 window2_mode;
    volatile mm_u32 actions;
    volatile mm_u32 core_pc;
    volatile mm_u32 core_sp;
    volatile mm_u64 core_steps;
    volatile mm_u8 core_sec;
    volatile mm_u8 core_mode;
    volatile mm_bool capstone_supported;
    volatile mm_bool capstone_enabled;
    char cpu_name[64];
    char image0_path[256];
    int input_fd;
    char esc_buf[16];
    mm_u8 esc_len;
    volatile mm_bool input_dirty;
    volatile mm_bool thread_running;
    volatile mm_bool thread_stop;
    volatile unsigned long thread_id;
    int log_fd;
    int log_read_fd;
    mm_u64 log_pos;
    char log_path[128];
    char lines[1024][512];
    size_t line_count;
    size_t line_head;
    char cur_line[512];
    size_t cur_len;
    mm_u32 regs[16];
    mm_u32 xpsr;
    mm_u32 msp_s;
    mm_u32 psp_s;
    mm_u32 msp_ns;
    mm_u32 psp_ns;
    mm_u32 msp_top_s;
    mm_u32 msp_min_s;
    mm_u32 msp_top_ns;
    mm_u32 msp_min_ns;
    mm_bool msp_top_s_valid;
    mm_bool msp_top_ns_valid;
    mm_u32 msplim_s;
    mm_u32 psplim_s;
    mm_u32 msplim_ns;
    mm_u32 psplim_ns;
    mm_u32 control_s;
    mm_u32 control_ns;
    mm_u32 primask_s;
    mm_u32 primask_ns;
    mm_u32 basepri_s;
    mm_u32 basepri_ns;
    mm_u32 faultmask_s;
    mm_u32 faultmask_ns;
    mm_u32 flash_base_s;
    mm_u32 flash_size_s;
    mm_u32 flash_base_ns;
    mm_u32 flash_size_ns;
    mm_u32 ram_base_s;
    mm_u32 ram_size_s;
    mm_u32 ram_base_ns;
    mm_u32 ram_size_ns;
    mm_u32 flash_total_size;
    mm_u32 ram_total_size;
    int serial_fd;
    char serial_label[32];
    char serial_lines[1024][512];
    size_t serial_line_count;
    size_t serial_line_head;
    char serial_cur_line[512];
    size_t serial_cur_len;
    int width;
    int height;
};

enum mm_tui_action {
    MM_TUI_ACTION_NONE = 0u,
    MM_TUI_ACTION_QUIT = 1u << 0,
    MM_TUI_ACTION_RESET = 1u << 1,
    MM_TUI_ACTION_PAUSE = 1u << 2,
    MM_TUI_ACTION_CONTINUE = 1u << 3,
    MM_TUI_ACTION_STEP = 1u << 4,
    MM_TUI_ACTION_RELOAD = 1u << 5,
    MM_TUI_ACTION_TOGGLE_CAPSTONE = 1u << 6,
    MM_TUI_ACTION_LAUNCH_GDB = 1u << 7
};

enum mm_tui_window1_mode {
    MM_TUI_WIN1_LOG = 0,
    MM_TUI_WIN1_CPU = 1
};

enum mm_tui_window2_mode {
    MM_TUI_WIN2_UART = 0,
    MM_TUI_WIN2_PERIPH = 1,
    MM_TUI_WIN2_GPIO = 2
};

mm_bool mm_tui_init(struct mm_tui *tui);
void mm_tui_shutdown(struct mm_tui *tui);
mm_bool mm_tui_redirect_stdio(struct mm_tui *tui);
void mm_tui_poll(struct mm_tui *tui);
mm_bool mm_tui_should_quit(const struct mm_tui *tui);
mm_u32 mm_tui_take_actions(struct mm_tui *tui);
mm_u8 mm_tui_window1_mode(const struct mm_tui *tui);
void mm_tui_set_target_running(struct mm_tui *tui, mm_bool running);
void mm_tui_set_gdb_status(struct mm_tui *tui, mm_bool connected, int port);
void mm_tui_set_capstone(struct mm_tui *tui, mm_bool supported, mm_bool enabled);
void mm_tui_set_image0(struct mm_tui *tui, const char *path);
void mm_tui_set_cpu_name(struct mm_tui *tui, const char *name);
void mm_tui_set_core_state(struct mm_tui *tui,
                           mm_u32 pc,
                           mm_u32 sp,
                           mm_u8 sec_state,
                           mm_u8 mode,
                           mm_u64 steps);
void mm_tui_set_registers(struct mm_tui *tui, const struct mm_cpu *cpu);
void mm_tui_set_memory_map(struct mm_tui *tui, const struct mm_memmap *map);
void mm_tui_close_devices(struct mm_tui *tui);
mm_bool mm_tui_start_thread(struct mm_tui *tui);
void mm_tui_stop_thread(struct mm_tui *tui);
void mm_tui_register(struct mm_tui *tui);
mm_bool mm_tui_is_active(void);
void mm_tui_attach_uart(const char *label, const char *path);

#endif /* M33MU_TUI_H */
