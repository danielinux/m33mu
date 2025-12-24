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

#ifndef TB_OPT_ATTR_W
#define TB_OPT_ATTR_W 32
#endif
#if TB_OPT_ATTR_W < 32
#error "TUI requires TB_OPT_ATTR_W >= 32 for truecolor"
#endif
#include "termbox2.h"
#include "widgets.h"

#include <fcntl.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include "m33mu/cpu.h"
#include "tui.h"

#define TUI_MAX_LINES 1024
#define TUI_MAX_COLS  512

#define TUI_RGB(r, g, b) (((uintattr_t)(r) << 16) | ((uintattr_t)(g) << 8) | (uintattr_t)(b))
#define TUI_COLOR_MASK 0x00ffffffu
#define TUI_FG_WHITE TUI_RGB(0xF5, 0xF5, 0xF5)
#define TUI_FG_DIM   TUI_RGB(0xCF, 0xCF, 0xCF)
#define TUI_FG_BLACK TB_HI_BLACK
#define TUI_BG_BLACK TB_HI_BLACK
#define TUI_BG_MENU  TUI_RGB(0x6A, 0x0D, 0xAD)
#define TUI_BG_STATUS TUI_RGB(0xFF, 0xD5, 0x4F)
#define TUI_BG_STOP  TUI_RGB(0xB8, 0x22, 0x22)
#define TUI_BG_RUN   TUI_RGB(0x1F, 0x8A, 0x3B)

static struct mm_tui *g_tui = 0;

static uintattr_t tui_attr(uintattr_t v)
{
    return (v & TUI_COLOR_MASK) | (v & TB_HI_BLACK);
}

static void tui_push_line(struct mm_tui *tui)
{
    size_t slot;
    if (tui->cur_len >= TUI_MAX_COLS) {
        tui->cur_len = TUI_MAX_COLS - 1;
    }
    tui->cur_line[tui->cur_len] = '\0';
    if (tui->line_count < TUI_MAX_LINES) {
        slot = tui->line_count++;
    } else {
        slot = tui->line_head;
        tui->line_head = (tui->line_head + 1u) % TUI_MAX_LINES;
    }
    memcpy(tui->lines[slot], tui->cur_line, tui->cur_len + 1u);
    tui->cur_len = 0;
}

static void tui_push_serial_line(struct mm_tui *tui)
{
    size_t slot;
    if (tui->serial_cur_len >= TUI_MAX_COLS) {
        tui->serial_cur_len = TUI_MAX_COLS - 1;
    }
    tui->serial_cur_line[tui->serial_cur_len] = '\0';
    if (tui->serial_line_count < TUI_MAX_LINES) {
        slot = tui->serial_line_count++;
    } else {
        slot = tui->serial_line_head;
        tui->serial_line_head = (tui->serial_line_head + 1u) % TUI_MAX_LINES;
    }
    memcpy(tui->serial_lines[slot], tui->serial_cur_line, tui->serial_cur_len + 1u);
    tui->serial_cur_len = 0;
}

static void tui_append_text(struct mm_tui *tui, const char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        char c = buf[i];
        if (c == '\n') {
            tui_push_line(tui);
        } else if (c == '\r') {
            /* ignore */
        } else {
            if (tui->cur_len + 1u < TUI_MAX_COLS) {
                tui->cur_line[tui->cur_len++] = c;
            }
        }
    }
}

static void tui_append_serial_text(struct mm_tui *tui, const char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        char c = buf[i];
        if (c == '\n') {
            tui_push_serial_line(tui);
        } else if (c == '\r') {
            /* ignore */
        } else {
            if (tui->serial_cur_len + 1u < TUI_MAX_COLS) {
                tui->serial_cur_line[tui->serial_cur_len++] = c;
            }
        }
    }
}

static mm_bool tui_read_log(struct mm_tui *tui)
{
    char buf[1024];
    ssize_t n;
    mm_bool changed = MM_FALSE;
    if (tui->log_read_fd < 0) return MM_FALSE;
    if (lseek(tui->log_read_fd, (off_t)tui->log_pos, SEEK_SET) < 0) {
        return MM_FALSE;
    }
    n = read(tui->log_read_fd, buf, (int)sizeof(buf));
    while (n > 0) {
        tui->log_pos += (mm_u64)n;
        tui_append_text(tui, buf, (size_t)n);
        changed = MM_TRUE;
        n = read(tui->log_read_fd, buf, (int)sizeof(buf));
    }
    return changed;
}

static mm_bool tui_read_serial(struct mm_tui *tui)
{
    char buf[1024];
    ssize_t n;
    mm_bool changed = MM_FALSE;
    if (tui->serial_fd < 0) return MM_FALSE;
    n = read(tui->serial_fd, buf, (int)sizeof(buf));
    while (n > 0) {
        tui_append_serial_text(tui, buf, (size_t)n);
        changed = MM_TRUE;
        n = read(tui->serial_fd, buf, (int)sizeof(buf));
    }
    return changed;
}

static void tui_draw_text(int x, int y, int max_x, uintattr_t fg, uintattr_t bg, const char *text)
{
    int cx = x;
    int i = 0;
    while (text[i] != '\0' && cx < max_x) {
        tb_set_cell(cx, y, (uint32_t)text[i], tui_attr(fg), tui_attr(bg));
        ++cx;
        ++i;
    }
}

static void tui_draw_filled(int x0, int y0, int x1, int y1, uintattr_t fg, uintattr_t bg)
{
    int x;
    int y;
    for (y = y0; y <= y1; ++y) {
        for (x = x0; x <= x1; ++x) {
            tb_set_cell(x, y, ' ', tui_attr(fg), tui_attr(bg));
        }
    }
}

static const char *tui_window1_title(const struct mm_tui *tui)
{
    if (tui->window1_mode == MM_TUI_WIN1_CPU) {
        return "CPU";
    }
    return "LOG";
}

static const char *tui_window2_title(const struct mm_tui *tui)
{
    switch (tui->window2_mode) {
        case MM_TUI_WIN2_SPI: return "SPI";
        case MM_TUI_WIN2_I2C: return "I2C";
        case MM_TUI_WIN2_GPIO: return "GPIO";
        default:
            if (tui->serial_label[0] != '\0') {
                return tui->serial_label;
            }
            return "UART";
    }
}

static void tui_handle_key(struct mm_tui *tui, int key, uint32_t ch, uint8_t mod)
{
    if (tui == 0) return;
    (void)mod;

    if (ch == 'a' || ch == 'A') {
        key = TB_KEY_ARROW_LEFT;
        ch = 0;
    } else if (ch == 'd' || ch == 'D') {
        key = TB_KEY_ARROW_RIGHT;
        ch = 0;
    }
    if (ch == 0x11) {
        tui->want_quit = MM_TRUE;
        tui->actions |= MM_TUI_ACTION_QUIT;
        return;
    }
    if (key == TB_KEY_ARROW_LEFT) {
        tui->window2_mode = (mm_u8)((tui->window2_mode + 3u) % 4u);
        return;
    }
    if (key == TB_KEY_ARROW_RIGHT) {
        tui->window2_mode = (mm_u8)((tui->window2_mode + 1u) % 4u);
        return;
    }
    if (key == TB_KEY_F2) {
        tui->actions |= tui->target_running ? MM_TUI_ACTION_PAUSE : MM_TUI_ACTION_CONTINUE;
        return;
    }
    if (key == TB_KEY_F3) {
        tui->window1_mode = (tui->window1_mode == MM_TUI_WIN1_LOG) ? MM_TUI_WIN1_CPU : MM_TUI_WIN1_LOG;
        return;
    }
    if (key == TB_KEY_F4) {
        tui->window2_mode = (mm_u8)((tui->window2_mode + 1u) % 4u);
        return;
    }
    if (key == TB_KEY_F5) {
        if (!tui->target_running) {
            tui->actions |= MM_TUI_ACTION_RELOAD;
        }
        return;
    }
    if (key == TB_KEY_F7) {
        if (!tui->target_running) {
            tui->actions |= MM_TUI_ACTION_STEP;
        }
        return;
    }
    if (key == TB_KEY_F8) {
        if (!tui->target_running) {
            tui->actions |= MM_TUI_ACTION_RESET;
        }
        return;
    }
}

static void tui_consume_raw(struct mm_tui *tui, const unsigned char *buf, size_t len)
{
    size_t i;
    if (tui == 0 || buf == 0 || len == 0) return;
    for (i = 0; i < len; ++i) {
        unsigned char b = buf[i];
        if (tui->esc_len == 0) {
            if (b == 0x1b) {
                tui->esc_buf[0] = (char)b;
                tui->esc_len = 1u;
                continue;
            }
            tui_handle_key(tui, 0, b, 0);
            continue;
        }
        if (tui->esc_len < sizeof(tui->esc_buf)) {
            tui->esc_buf[tui->esc_len++] = (char)b;
        } else {
            tui->esc_len = 0;
            continue;
        }

        if (tui->esc_len >= 3 && tui->esc_buf[0] == 0x1b && tui->esc_buf[1] == '[') {
            char c = tui->esc_buf[2];
            if (c == 'A') { tui_handle_key(tui, TB_KEY_ARROW_UP, 0, 0); tui->esc_len = 0; continue; }
            if (c == 'B') { tui_handle_key(tui, TB_KEY_ARROW_DOWN, 0, 0); tui->esc_len = 0; continue; }
            if (c == 'C') { tui_handle_key(tui, TB_KEY_ARROW_RIGHT, 0, 0); tui->esc_len = 0; continue; }
            if (c == 'D') { tui_handle_key(tui, TB_KEY_ARROW_LEFT, 0, 0); tui->esc_len = 0; continue; }
        }
        if (tui->esc_len >= 3 && tui->esc_buf[0] == 0x1b && tui->esc_buf[1] == 'O') {
            char c = tui->esc_buf[2];
            if (c == 'Q') { tui_handle_key(tui, TB_KEY_F2, 0, 0); tui->esc_len = 0; continue; }
            if (c == 'R') { tui_handle_key(tui, TB_KEY_F3, 0, 0); tui->esc_len = 0; continue; }
            if (c == 'S') { tui_handle_key(tui, TB_KEY_F4, 0, 0); tui->esc_len = 0; continue; }
        }
        if (tui->esc_len >= 5 && tui->esc_buf[0] == 0x1b && tui->esc_buf[1] == '[') {
            if (tui->esc_buf[2] == '1' && tui->esc_buf[3] == '5' && tui->esc_buf[4] == '~') {
                tui_handle_key(tui, TB_KEY_F5, 0, 0);
                tui->esc_len = 0;
                continue;
            }
            if (tui->esc_buf[2] == '1' && tui->esc_buf[3] == '8' && tui->esc_buf[4] == '~') {
                tui_handle_key(tui, TB_KEY_F7, 0, 0);
                tui->esc_len = 0;
                continue;
            }
            if (tui->esc_buf[2] == '1' && tui->esc_buf[3] == '9' && tui->esc_buf[4] == '~') {
                tui_handle_key(tui, TB_KEY_F8, 0, 0);
                tui->esc_len = 0;
                continue;
            }
            if (tui->esc_buf[2] == '2' && tui->esc_buf[3] == '1' && tui->esc_buf[4] == '~') {
                tui_handle_key(tui, TB_KEY_F10, 0, 0);
                tui->esc_len = 0;
                continue;
            }
        }
        if (tui->esc_len >= 6) {
            tui->esc_len = 0;
        }
    }
}

static void tui_draw(struct mm_tui *tui)
{
    int w, h;
    int menu_w;
    int console_w;
    int console_h;
    int console_x;
    int console_y;
    int inner_x;
    int inner_y;
    int inner_w;
    int inner_h;
    int split = 0;
    int log_w;
    int title_h = 1;
    int log_h;
    int log_y;
    int i;
    size_t start;
    size_t available;
    uintattr_t console_fg = TUI_FG_WHITE;
    uintattr_t console_bg = TUI_BG_BLACK;
    uintattr_t menu_bg = TUI_BG_MENU;
    uintattr_t menu_fg = TUI_FG_WHITE;
    uintattr_t status_bg = TUI_BG_STATUS;
    uintattr_t status_fg = TUI_FG_BLACK;
    uintattr_t title_bg = TUI_BG_STATUS;
    uintattr_t title_fg = TUI_FG_BLACK;
    uintattr_t control_bg = tui->target_running ? TUI_BG_RUN : TUI_BG_STOP;
    uintattr_t control_fg = TUI_FG_WHITE;

    w = tb_width();
    h = tb_height();
    if (w <= 0 || h <= 3) return;
    tui->width = w;
    tui->height = h;
    menu_w = w / 5;
    if (menu_w < 12) menu_w = 12;
    if (menu_w > w - 8) menu_w = w / 5;
    console_w = w - menu_w;
    console_h = h - 2;
    console_x = 0;
    console_y = 0;
    inner_x = console_x + 1;
    inner_y = console_y + 1;
    inner_w = console_w - 2;
    inner_h = console_h - 2;
    if (inner_w < 1) inner_w = 1;
    if (inner_h < 1) inner_h = 1;
    split = (inner_w >= 20) ? 1 : 0;
    log_w = inner_w;
    if (split) {
        log_w = (inner_w - 1) / 2;
        if (log_w < 20) log_w = inner_w / 2;
    }
    log_h = inner_h - title_h;
    if (log_h < 1) log_h = 1;
    log_y = inner_y + title_h;

    tb_clear();

    /* Console border (Unicode box drawing) */
    for (i = console_x; i < console_w; ++i) {
        tb_set_cell(i, console_y, 0x2550, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
        tb_set_cell(i, console_h - 1, 0x2550, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    }
    for (i = console_y; i < console_h; ++i) {
        tb_set_cell(console_x, i, 0x2551, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
        tb_set_cell(console_w - 1, i, 0x2551, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    }
    tb_set_cell(console_x, console_y, 0x2554, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    tb_set_cell(console_w - 1, console_y, 0x2557, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    tb_set_cell(console_x, console_h - 1, 0x255A, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    tb_set_cell(console_w - 1, console_h - 1, 0x255D, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));

    /* Menu panel */
    for (i = 0; i < console_h; ++i) {
        int x;
        for (x = console_w + 1; x < w; ++x) {
            tb_set_cell(x, i, ' ', tui_attr(menu_fg), tui_attr(menu_bg));
        }
    }
    tui_draw_text(console_w + 2, 1, w - 1, menu_fg, menu_bg, "Menu");
    tui_draw_text(console_w + 2, 3, w - 1, menu_fg, menu_bg, "Stop/Continue (F2)");
    tui_draw_text(console_w + 2, 5, w - 1, menu_fg, menu_bg,
                  (tui->window1_mode == MM_TUI_WIN1_LOG) ? "CPU (F3)" : "LOG (F3)");
    tui_draw_text(console_w + 2, 7, w - 1, menu_fg, menu_bg, "Next peripheral (F4)");
    {
        uintattr_t reload_fg = tui->target_running ? TUI_FG_DIM : menu_fg;
        tui_draw_text(console_w + 2, 9, w - 1, reload_fg, menu_bg, "Reload images (F5)");
    }
    {
        uintattr_t step_fg = tui->target_running ? TUI_FG_DIM : menu_fg;
        tui_draw_text(console_w + 2, 11, w - 1, step_fg, menu_bg, "Step (F7)");
        tui_draw_text(console_w + 2, 13, w - 1, step_fg, menu_bg, "CPU Reset (F8)");
    }
    tui_draw_text(console_w + 2, 15, w - 1, menu_fg, menu_bg, "Quit (Ctrl+Q)");

    /* Control bar */
    {
        int x;
        int y = h - 2;
        char info[160];
        const char *sec = (tui->core_sec == MM_SECURE) ? "Secure" : "Nonsecure";
        mm_u32 control = (tui->core_sec == MM_SECURE) ? tui->control_s : tui->control_ns;
        const char *mode = ((control & 0x1u) == 0u) ? "Handler" : "Thread";
        const char *run_mode = tui->target_running ? mode : "Stopped";
        for (x = 0; x < w; ++x) {
            tb_set_cell(x, y, ' ', tui_attr(control_fg), tui_attr(control_bg));
        }
        if (tui->target_running) {
            snprintf(info, sizeof(info),
                     "PC=0x%08lx  SP=0x%08lx  Mode=%s %s  Steps=%llu",
                     (unsigned long)tui->core_pc,
                     (unsigned long)tui->core_sp,
                     sec,
                     run_mode,
                     (unsigned long long)tui->core_steps);
        } else {
            snprintf(info, sizeof(info),
                     "PC=0x%08lx  SP=0x%08lx  Mode=Stopped  Steps=%llu",
                     (unsigned long)tui->core_pc,
                     (unsigned long)tui->core_sp,
                     (unsigned long long)tui->core_steps);
        }
        tui_draw_text(1, y, w - 2, control_fg, control_bg, info);
    }

    /* Status bar */
    {
        int x;
        for (x = 0; x < w; ++x) {
            tb_set_cell(x, h - 1, ' ', tui_attr(status_fg), tui_attr(status_bg));
        }
        tui_draw_text(1, h - 1, w - 1, status_fg, status_bg, "m33mu --tui");
    }

    /* Console */
    {
        int y;
        int x;
        for (y = inner_y; y < inner_y + inner_h; ++y) {
            for (x = inner_x; x < inner_x + inner_w; ++x) {
                tb_set_cell(x, y, ' ', tui_attr(console_fg), tui_attr(console_bg));
            }
        }
    }
    tui_draw_filled(inner_x, inner_y, inner_x + log_w - 1, inner_y + title_h - 1, title_fg, title_bg);
    tui_draw_text(inner_x + 1, inner_y, inner_x + log_w - 2, title_fg, title_bg, tui_window1_title(tui));
    if (tui->window1_mode == MM_TUI_WIN1_LOG) {
        available = (size_t)log_h;
        if (tui->line_count > available) {
            start = tui->line_count - available;
        } else {
            start = 0;
        }
        for (i = 0; i < log_h; ++i) {
            size_t idx = start + (size_t)i;
            if (idx < tui->line_count) {
                size_t slot = (tui->line_head + idx) % TUI_MAX_LINES;
                tui_draw_text(inner_x, log_y + i, inner_x + log_w, console_fg, console_bg, tui->lines[slot]);
            }
        }
        if (tui->cur_len > 0 && log_h > 0) {
            tui->cur_line[tui->cur_len] = '\0';
            tui_draw_text(inner_x, log_y + log_h - 1, inner_x + log_w, console_fg, console_bg, tui->cur_line);
        }
    } else {
        int line = 0;
        int col = (log_w >= 30) ? (log_w / 2) : log_w;
        char buf[64];
        for (i = 0; i < 8 && line < log_h; ++i, ++line) {
            snprintf(buf, sizeof(buf), "r%-2d 0x%08lx", i, (unsigned long)tui->regs[i]);
            tui_draw_text(inner_x, log_y + line, inner_x + col - 1, console_fg, console_bg, buf);
            if (col < log_w && (i + 8) < 16) {
                snprintf(buf, sizeof(buf), "r%-2d 0x%08lx", i + 8, (unsigned long)tui->regs[i + 8]);
                tui_draw_text(inner_x + col, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            }
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "xpsr 0x%08lx", (unsigned long)tui->xpsr);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "msp_s 0x%08lx  psp_s 0x%08lx",
                     (unsigned long)tui->msp_s, (unsigned long)tui->psp_s);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "msp_ns 0x%08lx  psp_ns 0x%08lx",
                     (unsigned long)tui->msp_ns, (unsigned long)tui->psp_ns);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "control_s 0x%08lx  control_ns 0x%08lx",
                     (unsigned long)tui->control_s, (unsigned long)tui->control_ns);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "primask_s 0x%08lx  primask_ns 0x%08lx",
                     (unsigned long)tui->primask_s, (unsigned long)tui->primask_ns);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "basepri_s 0x%08lx  basepri_ns 0x%08lx",
                     (unsigned long)tui->basepri_s, (unsigned long)tui->basepri_ns);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "faultmask_s 0x%08lx  faultmask_ns 0x%08lx",
                     (unsigned long)tui->faultmask_s, (unsigned long)tui->faultmask_ns);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
    }

    if (split) {
        int split_x = inner_x + log_w;
        for (i = inner_y; i < inner_y + inner_h; ++i) {
            tb_set_cell(split_x, i, 0x2551, TUI_FG_DIM, TUI_BG_BLACK);
        }
        tui_draw_filled(split_x + 1, inner_y, inner_x + inner_w - 1, inner_y + title_h - 1, title_fg, title_bg);
        tui_draw_text(split_x + 2, inner_y, inner_x + inner_w - 1, title_fg, title_bg, tui_window2_title(tui));
        if (tui->window2_mode == MM_TUI_WIN2_UART) {
            available = (size_t)log_h;
            if (tui->serial_line_count > available) {
                start = tui->serial_line_count - available;
            } else {
                start = 0;
            }
            for (i = 0; i < log_h; ++i) {
                size_t idx = start + (size_t)i;
                if (idx < tui->serial_line_count) {
                    size_t slot = (tui->serial_line_head + idx) % TUI_MAX_LINES;
                    tui_draw_text(split_x + 1, log_y + i, inner_x + inner_w - 1,
                                  console_fg, console_bg, tui->serial_lines[slot]);
                }
            }
            if (tui->serial_cur_len > 0 && log_h > 0) {
                tui->serial_cur_line[tui->serial_cur_len] = '\0';
                tui_draw_text(split_x + 1, log_y + log_h - 1, inner_x + inner_w - 1,
                              console_fg, console_bg, tui->serial_cur_line);
            }
        } else {
            const char *placeholder = "Not implemented";
            tui_draw_text(split_x + 2, log_y, inner_x + inner_w - 1,
                          console_fg, console_bg, placeholder);
        }
    }

    tb_present();
}

mm_bool mm_tui_init(struct mm_tui *tui)
{
    int ttyfd;
    if (tui == 0) return MM_FALSE;
    memset(tui, 0, sizeof(*tui));
    tui->log_fd = -1;
    tui->log_read_fd = -1;
    tui->serial_fd = -1;
    tui->input_fd = -1;
    tui->thread_running = MM_FALSE;
    tui->thread_stop = MM_FALSE;
    tui->thread_id = 0;
    if (!isatty(STDIN_FILENO)) {
        ttyfd = open("/dev/tty", O_RDONLY);
        if (ttyfd >= 0) {
            (void)dup2(ttyfd, STDIN_FILENO);
            close(ttyfd);
        }
    }
    tui->window1_mode = MM_TUI_WIN1_LOG;
    tui->window2_mode = MM_TUI_WIN2_UART;
    tui->target_running = MM_TRUE;
    tui->gdb_connected = MM_FALSE;
    tui->gdb_port = 0;
    if (isatty(STDIN_FILENO)) {
        ttyfd = open("/dev/tty", O_RDONLY);
        if (ttyfd >= 0) {
            struct termios tio;
            if (tcgetattr(ttyfd, &tio) == 0) {
                tio.c_iflag &= ~(ICRNL | INLCR | IGNCR);
                tio.c_lflag &= ~(ICANON | ECHO);
                tio.c_oflag &= ~(OPOST | ONLCR);
                tio.c_cc[VMIN] = 0;
                tio.c_cc[VTIME] = 1;
                (void)tcsetattr(ttyfd, TCSANOW, &tio);
            }
            tui->input_fd = ttyfd;
        }
    }
    tui->active = MM_FALSE;
    return MM_TRUE;
}

void mm_tui_shutdown(struct mm_tui *tui)
{
    if (tui == 0) return;
    mm_tui_stop_thread(tui);
    tui->active = MM_FALSE;
    if (tui->log_fd >= 0) close(tui->log_fd);
    if (tui->log_read_fd >= 0) close(tui->log_read_fd);
    if (tui->serial_fd >= 0) close(tui->serial_fd);
    if (tui->input_fd >= 0) close(tui->input_fd);
    if (g_tui == tui) {
        g_tui = 0;
    }
}

mm_bool mm_tui_redirect_stdio(struct mm_tui *tui)
{
    int fd;
    int read_fd;
    char tmpl[] = "/tmp/m33mu_tui_XXXXXX";
    if (tui == 0) return MM_FALSE;
    fd = mkstemp(tmpl);
    if (fd < 0) return MM_FALSE;
    read_fd = open(tmpl, O_RDONLY);
    if (read_fd < 0) {
        close(fd);
        return MM_FALSE;
    }
    snprintf(tui->log_path, sizeof(tui->log_path), "%s", tmpl);
    tui->log_fd = fd;
    tui->log_read_fd = read_fd;
    tui->log_pos = 0;

    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);
    return MM_TRUE;
}

void mm_tui_poll(struct mm_tui *tui)
{
    struct tb_event ev;
    mm_bool dirty = MM_FALSE;
    int timeout_ms;
    int ev_res;
    if (tui == 0 || !tui->active) return;
    if (tui_read_log(tui)) dirty = MM_TRUE;
    if (tui_read_serial(tui)) dirty = MM_TRUE;
    if (tui->input_dirty) {
        tui->input_dirty = MM_FALSE;
        dirty = MM_TRUE;
    }
    if (!dirty && tui->target_running) {
        timeout_ms = 0;
    } else {
        timeout_ms = 20;
    }
    ev_res = tb_peek_event(&ev, timeout_ms);
    while (ev_res > 0) {
        if (ev.type == TB_EVENT_KEY) {
            tui_handle_key(tui, ev.key, ev.ch, ev.mod);
            dirty = MM_TRUE;
        } else if (ev.type == TB_EVENT_RESIZE) {
            dirty = MM_TRUE;
        }
        timeout_ms = 0;
        ev_res = tb_peek_event(&ev, timeout_ms);
    }
    if (dirty) {
        tui_draw(tui);
    }
}

mm_bool mm_tui_should_quit(const struct mm_tui *tui)
{
    if (tui == 0) return MM_FALSE;
    return tui->want_quit;
}

mm_u32 mm_tui_take_actions(struct mm_tui *tui)
{
    mm_u32 actions;
    if (tui == 0) return 0;
    actions = tui->actions;
    tui->actions = 0;
    return actions;
}

mm_u8 mm_tui_window1_mode(const struct mm_tui *tui)
{
    if (tui == 0) return MM_TUI_WIN1_LOG;
    return tui->window1_mode;
}

void mm_tui_set_target_running(struct mm_tui *tui, mm_bool running)
{
    if (tui == 0) return;
    tui->target_running = running;
}

void mm_tui_set_gdb_status(struct mm_tui *tui, mm_bool connected, int port)
{
    if (tui == 0) return;
    tui->gdb_connected = connected;
    tui->gdb_port = port;
}

void mm_tui_set_core_state(struct mm_tui *tui,
                           mm_u32 pc,
                           mm_u32 sp,
                           mm_u8 sec_state,
                           mm_u8 mode,
                           mm_u64 steps)
{
    if (tui == 0) return;
    tui->core_pc = pc;
    tui->core_sp = sp;
    tui->core_sec = sec_state;
    tui->core_mode = mode;
    tui->core_steps = steps;
}

void mm_tui_set_registers(struct mm_tui *tui, const struct mm_cpu *cpu)
{
    int i;
    if (tui == 0 || cpu == 0) return;
    for (i = 0; i < 16; ++i) {
        tui->regs[i] = cpu->r[i];
    }
    tui->xpsr = cpu->xpsr;
    tui->msp_s = cpu->msp_s;
    tui->psp_s = cpu->psp_s;
    tui->msp_ns = cpu->msp_ns;
    tui->psp_ns = cpu->psp_ns;
    tui->control_s = cpu->control_s;
    tui->control_ns = cpu->control_ns;
    tui->primask_s = cpu->primask_s;
    tui->primask_ns = cpu->primask_ns;
    tui->basepri_s = cpu->basepri_s;
    tui->basepri_ns = cpu->basepri_ns;
    tui->faultmask_s = cpu->faultmask_s;
    tui->faultmask_ns = cpu->faultmask_ns;
}

void mm_tui_close_devices(struct mm_tui *tui)
{
    if (tui == 0) return;
    if (tui->serial_fd >= 0) {
        close(tui->serial_fd);
        tui->serial_fd = -1;
        tui->serial_line_count = 0;
        tui->serial_line_head = 0;
        tui->serial_cur_len = 0;
        tui->serial_cur_line[0] = '\0';
    }
}

static void *tui_thread_main(void *arg)
{
    struct mm_tui *tui = (struct mm_tui *)arg;
    if (tui == 0) return 0;
    if (tb_init() != 0) {
        fprintf(stderr, "[TUI] tb_init failed errno=%d\n", tb_last_errno());
        tui->actions |= MM_TUI_ACTION_QUIT;
        tui->want_quit = MM_TRUE;
        return 0;
    }
    tb_set_input_mode(TB_INPUT_ESC);
    tb_set_output_mode(TB_OUTPUT_TRUECOLOR);
    tb_set_clear_attrs(tui_attr(TUI_FG_WHITE), tui_attr(TUI_BG_BLACK));
    tb_invalidate();
    tb_clear();
    tb_present();
    tui->active = MM_TRUE;
    while (!tui->thread_stop) {
        struct pollfd pfd;
        int pres;

        mm_tui_poll(tui);
        if (tui->input_fd >= 0) {
            pfd.fd = tui->input_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            pres = poll(&pfd, 1, 500);
            if (pres > 0 && (pfd.revents & POLLIN) != 0) {
                unsigned char buf[64];
                ssize_t n = read(tui->input_fd, buf, sizeof(buf));
                if (n > 0) {
                    tui_consume_raw(tui, buf, (size_t)n);
                    tui->input_dirty = MM_TRUE;
                }
            }
        } else {
            usleep(500000);
        }
        tui->input_dirty = MM_TRUE;
    }
    if (tui->active) {
        tb_shutdown();
        tui->active = MM_FALSE;
    }
    return 0;
}

mm_bool mm_tui_start_thread(struct mm_tui *tui)
{
    pthread_t tid;
    if (tui == 0 || tui->thread_running) return MM_FALSE;
    tui->thread_stop = MM_FALSE;
    if (pthread_create(&tid, 0, tui_thread_main, tui) != 0) {
        return MM_FALSE;
    }
    tui->thread_id = (unsigned long)tid;
    tui->thread_running = MM_TRUE;
    return MM_TRUE;
}

void mm_tui_stop_thread(struct mm_tui *tui)
{
    pthread_t tid;
    if (tui == 0 || !tui->thread_running) return;
    tui->thread_stop = MM_TRUE;
    tid = (pthread_t)tui->thread_id;
    if (tid) {
        pthread_join(tid, 0);
    }
    tui->thread_running = MM_FALSE;
    tui->thread_id = 0;
}

void mm_tui_register(struct mm_tui *tui)
{
    g_tui = tui;
}

mm_bool mm_tui_is_active(void)
{
    return g_tui != 0 && g_tui->active;
}

void mm_tui_attach_uart(const char *label, const char *path)
{
    int fd;
    struct termios tio;
    if (g_tui == 0 || !g_tui->active) return;
    if (path == 0) return;
    if (g_tui->serial_fd >= 0) return;
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return;
    if (tcgetattr(fd, &tio) == 0) {
        tio.c_iflag &= ~(ICRNL | INLCR | IGNCR);
        tio.c_lflag &= ~(ICANON | ECHO);
        tio.c_oflag &= ~(OPOST | ONLCR);
        (void)tcsetattr(fd, TCSANOW, &tio);
    }
    g_tui->serial_fd = fd;
    if (label != 0) {
        snprintf(g_tui->serial_label, sizeof(g_tui->serial_label), "%s", label);
    } else {
        snprintf(g_tui->serial_label, sizeof(g_tui->serial_label), "USART");
    }
}
