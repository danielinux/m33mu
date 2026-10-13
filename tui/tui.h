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

#define TUI_MAX_LINES 1024
#define TUI_MAX_COLS  512
#define TUI_MAX_UARTS 8
#define TUI_DEBUG_SLOTS 6
#define TUI_DEBUG_BYTES 256

struct mm_tui_debug_slot {
    mm_u32 addr;
    mm_u8 size;
    mm_u8 kind; /* 1 = breakpoint, 2 = write watchpoint */
    mm_u8 sample[4];
    mm_bool valid;
    char label[64];
};

struct mm_tui_uart {
    int fd;
    char label[32];
    char path[128];
    char lines[TUI_MAX_LINES][TUI_MAX_COLS];
    size_t line_len[TUI_MAX_LINES];
    mm_u8 lines_fg[TUI_MAX_LINES][TUI_MAX_COLS];
    mm_u8 lines_bg[TUI_MAX_LINES][TUI_MAX_COLS];
    size_t line_count;
    size_t line_head;
    char cur_line[TUI_MAX_COLS];
    mm_u8 cur_fg[TUI_MAX_COLS];
    mm_u8 cur_bg[TUI_MAX_COLS];
    size_t cur_len;
    mm_u8 fg;
    mm_u8 bg;
    char esc_buf[16];
    mm_u8 esc_len;
    mm_bool esc_active;
    size_t scroll_offset;
};

struct mm_tui {
    volatile mm_bool active;
    volatile mm_bool want_quit;
    volatile mm_bool target_running;
    volatile mm_bool gdb_connected;
    volatile mm_bool debugger_view;
    volatile mm_bool debugger_disasm_ready;
    volatile mm_bool debugger_command_ready;
    char debugger_input[256];
    char debugger_text_input[16];
    char debugger_data_input[16];
    mm_u8 debugger_focus;
    char debugger_command[256];
    char debugger_last_command[256];
    char debugger_message[256];
    char debugger_source_path[256];
    char debugger_source[64][160];
    int debugger_source_line;
    int debugger_source_start;
    int debugger_source_count;
    mm_bool debugger_source_valid;
    mm_u32 debugger_source_pc;
    int debugger_code_scroll;
    mm_u32 debugger_frame_pc[8];
    mm_u8 debugger_frame_count;
    mm_u8 debugger_frame_selected;
    mm_bool debugger_frame_has_lr;
    mm_bool debugger_show_backtrace;
    mm_bool debugger_next_pending;
    mm_u32 debugger_next_pc;
    mm_u32 debugger_next_sp;
    mm_u8 debugger_next_sec;
    mm_u64 debugger_step_cycle;
    mm_u32 debugger_text_addr;
    mm_u32 debugger_code_addr;
    mm_u64 debugger_snapshot_ns;
    mm_u32 debugger_data_addr;
    mm_bool debugger_text_pinned;
    mm_bool debugger_data_pinned;
    mm_u8 debugger_text[TUI_DEBUG_BYTES];
    mm_u8 debugger_code[TUI_DEBUG_BYTES];
    mm_u8 debugger_data[TUI_DEBUG_BYTES];
    mm_u8 debugger_text_valid[TUI_DEBUG_BYTES];
    mm_u8 debugger_code_valid[TUI_DEBUG_BYTES];
    mm_u8 debugger_data_valid[TUI_DEBUG_BYTES];
    char debugger_text_symbols[TUI_DEBUG_BYTES / 2][64];
    char debugger_text_targets[TUI_DEBUG_BYTES / 2][64];
    mm_u32 debugger_text_symbol_base;
    mm_bool debugger_text_symbols_ready;
    struct mm_tui_debug_slot debugger_slots[TUI_DEBUG_SLOTS];
    mm_bool (*debugger_resolve_symbol)(void *opaque, const char *name, mm_u32 *addr);
    void *debugger_resolve_opaque;
    void *debugger_monitor_opaque;
    mm_u8 debugger_watch_hit;
    mm_u32 debugger_watch_addr;
    volatile int gdb_port;
    volatile mm_u8 window1_mode;
    volatile mm_u8 window2_mode;
    volatile mm_u32 actions;
    volatile mm_u32 core_pc;
    volatile mm_u32 core_sp;
    volatile mm_u64 core_steps;
    volatile mm_u8 core_sec;
    volatile mm_u8 core_mode;
    volatile mm_u32 func_pc;
    volatile mm_bool func_valid;
    char func_name[128];
    char command_line[512];
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
    mm_u32 fpu_regs[32];
    mm_u32 xpsr;
    mm_u32 fpscr;
    mm_bool fpu_enabled;
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
    mm_u32 psp_top_s;
    mm_u32 psp_min_s;
    mm_u32 psp_top_ns;
    mm_u32 psp_min_ns;
    mm_bool psp_top_s_valid;
    mm_bool psp_top_ns_valid;
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
    int serial_count;
    int serial_selected;
    struct mm_tui_uart serials[TUI_MAX_UARTS];
    int window2_page_lines;
    int width;
    int height;
    mm_bool quit_prompt;
    mm_u8 quit_choice;
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
    MM_TUI_ACTION_DEBUG_COMMAND = 1u << 7
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
void mm_tui_set_command_line(struct mm_tui *tui, const char *cmdline);
void mm_tui_set_function(struct mm_tui *tui, mm_u32 pc, const char *name);
void mm_tui_set_core_state(struct mm_tui *tui,
                           mm_u32 pc,
                           mm_u32 sp,
                           mm_u8 sec_state,
                           mm_u8 mode,
                           mm_u64 steps);
void mm_tui_set_registers(struct mm_tui *tui, const struct mm_cpu *cpu, mm_bool fpu_enabled);
void mm_tui_set_memory_map(struct mm_tui *tui, const struct mm_memmap *map);
void mm_tui_close_devices(struct mm_tui *tui);
mm_bool mm_tui_start_thread(struct mm_tui *tui);
void mm_tui_stop_thread(struct mm_tui *tui);
void mm_tui_register(struct mm_tui *tui);
mm_bool mm_tui_is_active(void);
void mm_tui_attach_uart(const char *label, const char *path);

#endif /* M33MU_TUI_H */
