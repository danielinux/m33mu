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

#define _XOPEN_SOURCE 700
#include <locale.h>
#include <wchar.h>
#include <stdint.h>
#include <time.h>
#if defined(M33MU_USE_NCURSESW)
#include <ncursesw/curses.h>
#else
#include <curses.h>
#endif

#include <fcntl.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include "m33mu/cpu.h"
#include "m33mu/gpio.h"
#include "m33mu/spiflash.h"
#include "m33mu/tpm_tis.h"
#include "m33mu/usbdev.h"
#include "m33mu/gpio.h"
#include "m33mu/memmap.h"
#include "tui.h"

typedef uint32_t uintattr_t;

#define TUI_MAX_LINES 1024
#define TUI_MAX_COLS  512

#define TUI_RGB(r, g, b) (((uintattr_t)(r) << 16) | ((uintattr_t)(g) << 8) | (uintattr_t)(b))
#define TUI_COLOR_MASK 0x00ffffffu
#define TUI_FG_WHITE TUI_RGB(0xF5, 0xF5, 0xF5)
#define TUI_FG_DIM   TUI_RGB(0xCF, 0xCF, 0xCF)
#define TUI_FG_GREY  TUI_RGB(0x88, 0x88, 0x88)
#define TUI_FG_CYAN  TUI_RGB(0x4D, 0xD0, 0xE1)
#define TUI_FG_RED   TUI_RGB(0xE5, 0x39, 0x35)
#define TUI_FG_GREEN TUI_RGB(0x43, 0xA0, 0x47)
#define TUI_FG_MAGENTA TUI_RGB(0xBA, 0x68, 0xC8)
#define TUI_FG_BLACK TUI_RGB(0x00, 0x00, 0x00)
#define TUI_BG_BLACK TUI_RGB(0x00, 0x00, 0x00)
#define TUI_BG_MENU  TUI_RGB(0x6A, 0x0D, 0xAD)
#define TUI_BG_STATUS TUI_RGB(0xFF, 0xD5, 0x4F)
#define TUI_BG_STOP  TUI_RGB(0xB8, 0x22, 0x22)
#define TUI_BG_RUN   TUI_RGB(0x1F, 0x8A, 0x3B)
#define TUI_BG_NS    TUI_RGB(0x1E, 0x5A, 0xB5)

static struct mm_tui *g_tui = 0;

enum tui_color_slot {
    TUI_COLOR_WHITE = 0,
    TUI_COLOR_DIM,
    TUI_COLOR_GREY,
    TUI_COLOR_CYAN,
    TUI_COLOR_RED,
    TUI_COLOR_GREEN,
    TUI_COLOR_MAGENTA,
    TUI_COLOR_BLACK,
    TUI_COLOR_MENU,
    TUI_COLOR_STATUS,
    TUI_COLOR_STOP,
    TUI_COLOR_RUN,
    TUI_COLOR_NS,
    TUI_COLOR_COUNT
};

static short tui_color_ids[TUI_COLOR_COUNT];
static short tui_pair_map[TUI_COLOR_COUNT][TUI_COLOR_COUNT];
static short tui_next_pair = 1;
static mm_bool tui_colors_ready = MM_FALSE;

static uintattr_t tui_attr(uintattr_t v)
{
    return (v & TUI_COLOR_MASK);
}

static short tui_rgb_to_curses(mm_u32 rgb)
{
    return (short)((rgb * 1000u) / 255u);
}

static int tui_color_index(uintattr_t rgb)
{
    switch (rgb & TUI_COLOR_MASK) {
        case TUI_FG_WHITE: return TUI_COLOR_WHITE;
        case TUI_FG_DIM: return TUI_COLOR_DIM;
        case TUI_FG_GREY: return TUI_COLOR_GREY;
        case TUI_FG_CYAN: return TUI_COLOR_CYAN;
        case TUI_FG_RED: return TUI_COLOR_RED;
        case TUI_FG_GREEN: return TUI_COLOR_GREEN;
        case TUI_FG_MAGENTA: return TUI_COLOR_MAGENTA;
        case TUI_FG_BLACK: return TUI_COLOR_BLACK;
        case TUI_BG_MENU: return TUI_COLOR_MENU;
        case TUI_BG_STATUS: return TUI_COLOR_STATUS;
        case TUI_BG_STOP: return TUI_COLOR_STOP;
        case TUI_BG_RUN: return TUI_COLOR_RUN;
        case TUI_BG_NS: return TUI_COLOR_NS;
        default: return TUI_COLOR_WHITE;
    }
}

static void tui_init_colors(void)
{
    int i;
    if (tui_colors_ready) return;
    tui_colors_ready = MM_TRUE;
    if (!has_colors()) return;
    start_color();
    (void)use_default_colors();

    if (can_change_color() && COLORS >= (int)(TUI_COLOR_COUNT + 1)) {
        int base = COLORS - (int)TUI_COLOR_COUNT;
        if (base < 1) base = 1;
        for (i = 0; i < (int)TUI_COLOR_COUNT; ++i) {
            tui_color_ids[i] = (short)(base + i);
        }
        init_color(tui_color_ids[TUI_COLOR_WHITE],
                   tui_rgb_to_curses(0xF5),
                   tui_rgb_to_curses(0xF5),
                   tui_rgb_to_curses(0xF5));
        init_color(tui_color_ids[TUI_COLOR_DIM],
                   tui_rgb_to_curses(0xCF),
                   tui_rgb_to_curses(0xCF),
                   tui_rgb_to_curses(0xCF));
        init_color(tui_color_ids[TUI_COLOR_GREY],
                   tui_rgb_to_curses(0x88),
                   tui_rgb_to_curses(0x88),
                   tui_rgb_to_curses(0x88));
        init_color(tui_color_ids[TUI_COLOR_CYAN],
                   tui_rgb_to_curses(0x4D),
                   tui_rgb_to_curses(0xD0),
                   tui_rgb_to_curses(0xE1));
        init_color(tui_color_ids[TUI_COLOR_RED],
                   tui_rgb_to_curses(0xE5),
                   tui_rgb_to_curses(0x39),
                   tui_rgb_to_curses(0x35));
        init_color(tui_color_ids[TUI_COLOR_GREEN],
                   tui_rgb_to_curses(0x43),
                   tui_rgb_to_curses(0xA0),
                   tui_rgb_to_curses(0x47));
        init_color(tui_color_ids[TUI_COLOR_MAGENTA],
                   tui_rgb_to_curses(0xBA),
                   tui_rgb_to_curses(0x68),
                   tui_rgb_to_curses(0xC8));
        init_color(tui_color_ids[TUI_COLOR_BLACK],
                   tui_rgb_to_curses(0x00),
                   tui_rgb_to_curses(0x00),
                   tui_rgb_to_curses(0x00));
        init_color(tui_color_ids[TUI_COLOR_MENU],
                   tui_rgb_to_curses(0x6A),
                   tui_rgb_to_curses(0x0D),
                   tui_rgb_to_curses(0xAD));
        init_color(tui_color_ids[TUI_COLOR_STATUS],
                   tui_rgb_to_curses(0xFF),
                   tui_rgb_to_curses(0xD5),
                   tui_rgb_to_curses(0x4F));
        init_color(tui_color_ids[TUI_COLOR_STOP],
                   tui_rgb_to_curses(0xB8),
                   tui_rgb_to_curses(0x22),
                   tui_rgb_to_curses(0x22));
        init_color(tui_color_ids[TUI_COLOR_RUN],
                   tui_rgb_to_curses(0x1F),
                   tui_rgb_to_curses(0x8A),
                   tui_rgb_to_curses(0x3B));
        init_color(tui_color_ids[TUI_COLOR_NS],
                   tui_rgb_to_curses(0x1E),
                   tui_rgb_to_curses(0x5A),
                   tui_rgb_to_curses(0xB5));
    } else {
        tui_color_ids[TUI_COLOR_WHITE] = COLOR_WHITE;
        tui_color_ids[TUI_COLOR_DIM] = COLOR_WHITE;
        tui_color_ids[TUI_COLOR_GREY] = COLOR_WHITE;
        tui_color_ids[TUI_COLOR_CYAN] = COLOR_CYAN;
        tui_color_ids[TUI_COLOR_RED] = COLOR_RED;
        tui_color_ids[TUI_COLOR_GREEN] = COLOR_GREEN;
        tui_color_ids[TUI_COLOR_MAGENTA] = COLOR_MAGENTA;
        tui_color_ids[TUI_COLOR_BLACK] = COLOR_BLACK;
        tui_color_ids[TUI_COLOR_MENU] = COLOR_MAGENTA;
        tui_color_ids[TUI_COLOR_STATUS] = COLOR_YELLOW;
        tui_color_ids[TUI_COLOR_STOP] = COLOR_RED;
        tui_color_ids[TUI_COLOR_RUN] = COLOR_GREEN;
        tui_color_ids[TUI_COLOR_NS] = COLOR_BLUE;
    }
}

static short tui_color_pair(uintattr_t fg, uintattr_t bg)
{
    int fg_idx = tui_color_index(fg);
    int bg_idx = tui_color_index(bg);
    short pair = tui_pair_map[fg_idx][bg_idx];
    if (pair != 0) return pair;
    if (!has_colors()) return 0;
    if (tui_next_pair >= COLOR_PAIRS) return 0;
    pair = tui_next_pair++;
    init_pair(pair, tui_color_ids[fg_idx], tui_color_ids[bg_idx]);
    tui_pair_map[fg_idx][bg_idx] = pair;
    return pair;
}

static void tui_format_size(char *buf, size_t buf_len, mm_u32 bytes)
{
    const char *unit = "B";
    double size = (double)bytes;
    if (buf == 0 || buf_len == 0) return;
    if (bytes >= (1024u * 1024u)) {
        unit = "MB";
        size = size / (1024.0 * 1024.0);
    } else if (bytes >= 1024u) {
        unit = "KB";
        size = size / 1024.0;
    }
    if (size >= 10.0 || unit[0] == 'B') {
        snprintf(buf, buf_len, "%.0f%s", size, unit);
    } else {
        snprintf(buf, buf_len, "%.1f%s", size, unit);
    }
}

static void tui_put_cell(int x, int y, uint32_t ch, uintattr_t fg, uintattr_t bg)
{
    short pair = tui_color_pair(fg, bg);
    attr_t attrs = COLOR_PAIR(pair);
    chtype out_ch = (chtype)ch;
#if !defined(M33MU_USE_NCURSESW)
    if (ch == 0x2550) out_ch = ACS_HLINE;
    else if (ch == 0x2551) out_ch = ACS_VLINE;
    else if (ch == 0x2554) out_ch = ACS_ULCORNER;
    else if (ch == 0x2557) out_ch = ACS_URCORNER;
    else if (ch == 0x255A) out_ch = ACS_LLCORNER;
    else if (ch == 0x255D) out_ch = ACS_LRCORNER;
#endif
#if defined(M33MU_USE_NCURSESW)
    if (ch > 0x7Fu) {
        cchar_t wc;
        wchar_t wch[2];
        wch[0] = (wchar_t)ch;
        wch[1] = L'\0';
        (void)setcchar(&wc, wch, attrs, 0, NULL);
        mvadd_wch(y, x, &wc);
        return;
    }
#endif
    attrset(attrs);
    mvaddch(y, x, out_ch);
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
        tui_put_cell(cx, y, (uint32_t)text[i], tui_attr(fg), tui_attr(bg));
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
            tui_put_cell(x, y, ' ', tui_attr(fg), tui_attr(bg));
        }
    }
}

static int tui_draw_gpio_label(int x, int y, int max_x, const char *label,
                               mm_bool clock_on, uintattr_t bg)
{
    char buf[16];
    int i = 0;
    int cx = x;
    uintattr_t fg = clock_on ? TUI_FG_DIM : TUI_FG_GREY;
    if (label == 0 || label[0] == '\0') {
        snprintf(buf, sizeof(buf), "?");
    } else {
        snprintf(buf, sizeof(buf), "%s", label);
    }
    while (buf[i] != '\0' && cx < max_x) {
        tui_put_cell(cx++, y, (uint32_t)buf[i], tui_attr(fg), tui_attr(bg));
        ++i;
    }
    if (cx < max_x) {
        tui_put_cell(cx++, y, ' ', tui_attr(fg), tui_attr(bg));
    }
    return cx;
}

static void tui_draw_gpio_line(int x, int y, int max_x, const char *label, int pins,
                               mm_u32 moder, mm_u32 odr, mm_bool clock_on, uintattr_t bg)
{
    int pin;
    int limit = (pins > 0 && pins < 32) ? pins : 32;
    int cx = tui_draw_gpio_label(x, y, max_x, label, clock_on, bg);
    if (cx >= max_x) return;
    for (pin = 0; pin < limit && cx < max_x; ++pin, ++cx) {
        mm_u32 mode = (moder >> (pin * 2)) & 0x3u;
        mm_u32 bit = (odr >> pin) & 0x1u;
        uintattr_t fg = TUI_FG_GREY;
        char ch = '-';
        if (mode == 0u) {
            ch = 'I';
            fg = TUI_FG_CYAN;
        } else if (mode == 1u) {
            ch = bit ? '1' : '0';
            fg = bit ? TUI_FG_GREEN : TUI_FG_RED;
        } else if (mode == 2u) {
            ch = 'P';
            fg = TUI_FG_MAGENTA;
        } else {
            ch = 'A';
            fg = TUI_FG_CYAN;
        }
        if (!clock_on) {
            fg = TUI_FG_GREY;
        }
        tui_put_cell(cx, y, (uint32_t)ch, tui_attr(fg), tui_attr(bg));
    }
}

static void tui_draw_gpio_sec_line(int x, int y, int max_x, const char *label, int pins,
                                   mm_u32 seccfgr, mm_bool clock_on, uintattr_t bg)
{
    int pin;
    int limit = (pins > 0 && pins < 32) ? pins : 32;
    int cx = tui_draw_gpio_label(x, y, max_x, label, clock_on, bg);
    if (cx >= max_x) return;
    for (pin = 0; pin < limit && cx < max_x; ++pin, ++cx) {
        mm_u32 bit = (seccfgr >> pin) & 0x1u;
        char ch = bit ? 'S' : 'N';
        uintattr_t fg = bit ? TUI_FG_GREEN : TUI_FG_CYAN;
        if (!clock_on) {
            fg = TUI_FG_GREY;
        }
        tui_put_cell(cx, y, (uint32_t)ch, tui_attr(fg), tui_attr(bg));
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
        case MM_TUI_WIN2_PERIPH: return "PERIPHERALS";
        case MM_TUI_WIN2_GPIO: return "GPIO";
        default:
            if (tui->serial_label[0] != '\0') {
                return tui->serial_label;
            }
            return "UART";
    }
}

static void tui_serial_backspace(struct mm_tui *tui)
{
    if (tui->serial_cur_len > 0) {
        tui->serial_cur_len--;
        tui->serial_cur_line[tui->serial_cur_len] = '\0';
    }
}

static void tui_handle_key(struct mm_tui *tui, int key, uint32_t ch, uint8_t mod)
{
    if (tui == 0) return;
    (void)mod;

    if (key == 27 || ch == 27) {
        tui->want_quit = MM_TRUE;
        tui->actions |= MM_TUI_ACTION_QUIT;
        return;
    }
    if (tui->window2_mode == MM_TUI_WIN2_UART && tui->serial_fd >= 0) {
        mm_u8 b = 0;
        mm_bool send = MM_FALSE;
        mm_bool backspace = MM_FALSE;
        mm_bool enter = MM_FALSE;
        if (key == KEY_ENTER || ch == '\n' || ch == '\r') {
            b = '\r';
            send = MM_TRUE;
            enter = MM_TRUE;
        } else if (key == KEY_BACKSPACE || ch == 0x7Fu || ch == 0x08u) {
            b = 0x7Fu;
            send = MM_TRUE;
            backspace = MM_TRUE;
        } else if (ch != 0) {
            b = (mm_u8)ch;
            send = MM_TRUE;
        }
        if (send) {
            (void)write(tui->serial_fd, &b, 1);
            if (backspace) {
                tui_serial_backspace(tui);
            } else if (enter) {
                tui_append_serial_text(tui, "\n", 1);
            } else {
                tui_append_serial_text(tui, (const char *)&b, 1);
            }
            tui->input_dirty = MM_TRUE;
        }
    }
    if (key == KEY_LEFT) {
        tui->window2_mode = (mm_u8)((tui->window2_mode + 2u) % 3u);
        return;
    }
    if (key == KEY_RIGHT) {
        tui->window2_mode = (mm_u8)((tui->window2_mode + 1u) % 3u);
        return;
    }
    if (key == KEY_F(2)) {
        tui->actions |= tui->target_running ? MM_TUI_ACTION_PAUSE : MM_TUI_ACTION_CONTINUE;
        return;
    }
    if (key == KEY_F(3)) {
        tui->window1_mode = (tui->window1_mode == MM_TUI_WIN1_LOG) ? MM_TUI_WIN1_CPU : MM_TUI_WIN1_LOG;
        return;
    }
    if (key == KEY_F(4)) {
        tui->window2_mode = (mm_u8)((tui->window2_mode + 1u) % 3u);
        return;
    }
    if (key == KEY_F(5)) {
        if (!tui->target_running) {
            tui->actions |= MM_TUI_ACTION_RELOAD;
        }
        return;
    }
    if (key == KEY_F(7)) {
        if (!tui->target_running) {
            tui->actions |= MM_TUI_ACTION_STEP;
        }
        return;
    }
    if (key == KEY_F(8)) {
        if (!tui->target_running) {
            tui->actions |= MM_TUI_ACTION_RESET;
        }
        return;
    }
    if (key == KEY_F(6)) {
        tui->actions |= MM_TUI_ACTION_TOGGLE_CAPSTONE;
        return;
    }
    if (key == KEY_F(9)) {
        tui->actions |= MM_TUI_ACTION_LAUNCH_GDB;
        return;
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
    uintattr_t status_fg = TUI_FG_WHITE;
    uintattr_t title_bg = TUI_BG_STATUS;
    uintattr_t title_fg = TUI_FG_BLACK;
    uintattr_t control_bg = TUI_BG_RUN;
    uintattr_t control_fg = TUI_FG_WHITE;

    getmaxyx(stdscr, h, w);
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

    if (!tui->target_running) {
        status_bg = TUI_BG_STOP;
        control_bg = TUI_BG_STOP;
    } else if (tui->core_sec == MM_SECURE) {
        status_bg = TUI_BG_RUN;
        control_bg = TUI_BG_RUN;
    } else {
        status_bg = TUI_BG_NS;
        control_bg = TUI_BG_NS;
    }

    erase();

    /* Console border (Unicode box drawing) */
    for (i = console_x; i < console_w; ++i) {
        tui_put_cell(i, console_y, 0x2550, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
        tui_put_cell(i, console_h - 1, 0x2550, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    }
    for (i = console_y; i < console_h; ++i) {
        tui_put_cell(console_x, i, 0x2551, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
        tui_put_cell(console_w - 1, i, 0x2551, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    }
    tui_put_cell(console_x, console_y, 0x2554, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    tui_put_cell(console_w - 1, console_y, 0x2557, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    tui_put_cell(console_x, console_h - 1, 0x255A, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));
    tui_put_cell(console_w - 1, console_h - 1, 0x255D, tui_attr(TUI_FG_DIM), tui_attr(TUI_BG_BLACK));

    /* Menu panel */
    for (i = 0; i < console_h; ++i) {
        int x;
        for (x = console_w + 1; x < w; ++x) {
            tui_put_cell(x, i, ' ', tui_attr(menu_fg), tui_attr(menu_bg));
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
    if (tui->capstone_supported) {
        const char *cap = tui->capstone_enabled ? "Capstone: on (F6)" : "Capstone: off (F6)";
        tui_draw_text(console_w + 2, 15, w - 1, menu_fg, menu_bg, cap);
    } else {
        tui_draw_text(console_w + 2, 15, w - 1, menu_fg, menu_bg, "Capstone: n/a (F6)");
    }
    {
        uintattr_t step_fg = tui->target_running ? TUI_FG_DIM : menu_fg;
        tui_draw_text(console_w + 2, 11, w - 1, step_fg, menu_bg, "Step (F7)");
        tui_draw_text(console_w + 2, 13, w - 1, step_fg, menu_bg, "CPU Reset (F8)");
    }
    tui_draw_text(console_w + 2, 17, w - 1, menu_fg, menu_bg, "GDB TUI (F9)");
    tui_draw_text(console_w + 2, 19, w - 1, menu_fg, menu_bg, "Quit (Esc)");

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
            tui_put_cell(x, y, ' ', tui_attr(control_fg), tui_attr(control_bg));
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
            tui_put_cell(x, h - 1, ' ', tui_attr(status_fg), tui_attr(status_bg));
        }
        tui_draw_text(1, h - 1, w - 1, status_fg, status_bg, "m33mu --tui");
    }

    /* Console */
    {
        int y;
        int x;
        for (y = inner_y; y < inner_y + inner_h; ++y) {
            for (x = inner_x; x < inner_x + inner_w; ++x) {
                tui_put_cell(x, y, ' ', tui_attr(console_fg), tui_attr(console_bg));
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
        char buf[128];
        char size_buf[32];
        char range_buf[64];
        if (line < log_h) {
            const char *name = (tui->cpu_name[0] != '\0') ? tui->cpu_name : "unknown";
            tui_format_size(size_buf, sizeof(size_buf), tui->flash_total_size);
            snprintf(buf, sizeof(buf), "CPU: %s  Flash: %s", name, size_buf);
            tui_format_size(size_buf, sizeof(size_buf), tui->ram_total_size);
            if ((int)strlen(buf) + (int)strlen(size_buf) + 8 < log_w) {
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "  RAM: %s", size_buf);
            }
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (line < log_h) {
            line++;
        }
        if (line < log_h) {
            if (tui->flash_size_s > 0u) {
                snprintf(range_buf, sizeof(range_buf), "FLASH S 0x%08lx-0x%08lx",
                         (unsigned long)tui->flash_base_s,
                         (unsigned long)(tui->flash_base_s + tui->flash_size_s - 1u));
                tui_format_size(size_buf, sizeof(size_buf), tui->flash_size_s);
                snprintf(buf, sizeof(buf), "%s (%s)", range_buf, size_buf);
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                line++;
            }
        }
        if (line < log_h) {
            if (tui->flash_size_ns > 0u) {
                snprintf(range_buf, sizeof(range_buf), "FLASH NS 0x%08lx-0x%08lx",
                         (unsigned long)tui->flash_base_ns,
                         (unsigned long)(tui->flash_base_ns + tui->flash_size_ns - 1u));
                tui_format_size(size_buf, sizeof(size_buf), tui->flash_size_ns);
                snprintf(buf, sizeof(buf), "%s (%s)", range_buf, size_buf);
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                line++;
            }
        }
        if (line < log_h) {
            if (tui->ram_size_s > 0u) {
                snprintf(range_buf, sizeof(range_buf), "RAM S   0x%08lx-0x%08lx",
                         (unsigned long)tui->ram_base_s,
                         (unsigned long)(tui->ram_base_s + tui->ram_size_s - 1u));
                tui_format_size(size_buf, sizeof(size_buf), tui->ram_size_s);
                snprintf(buf, sizeof(buf), "%s (%s)", range_buf, size_buf);
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                line++;
            }
        }
        if (line < log_h) {
            if (tui->ram_size_ns > 0u) {
                snprintf(range_buf, sizeof(range_buf), "RAM NS  0x%08lx-0x%08lx",
                         (unsigned long)tui->ram_base_ns,
                         (unsigned long)(tui->ram_base_ns + tui->ram_size_ns - 1u));
                tui_format_size(size_buf, sizeof(size_buf), tui->ram_size_ns);
                snprintf(buf, sizeof(buf), "%s (%s)", range_buf, size_buf);
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                line++;
            }
        }
        if (line < log_h) {
            line++;
        }
        if (line < log_h) {
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, "CPU Registers:");
            line++;
        }
        if (line < log_h) {
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, "=============");
            line++;
        }
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
            snprintf(buf, sizeof(buf), "msplim_s 0x%08lx  psplim_s 0x%08lx",
                     (unsigned long)tui->msplim_s, (unsigned long)tui->psplim_s);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "msplim_ns 0x%08lx  psplim_ns 0x%08lx",
                     (unsigned long)tui->msplim_ns, (unsigned long)tui->psplim_ns);
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
        if (line < log_h) {
            line++;
        }
        if (line < log_h) {
            mm_u32 cur_used = 0;
            mm_u32 max_used = 0;
            if (tui->msp_top_s_valid) {
                if (tui->msp_top_s >= tui->msp_s) {
                    cur_used = tui->msp_top_s - tui->msp_s;
                }
                if (tui->msp_top_s >= tui->msp_min_s) {
                    max_used = tui->msp_top_s - tui->msp_min_s;
                }
                snprintf(buf, sizeof(buf), "Secure stack: current %lu max %lu",
                         (unsigned long)cur_used, (unsigned long)max_used);
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            } else {
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, TUI_FG_DIM, console_bg,
                              "Secure stack: Unused");
            }
            line++;
        }
        if (line < log_h) {
            if (tui->msp_top_ns_valid) {
                mm_u32 cur_used = 0;
                mm_u32 max_used = 0;
                if (tui->msp_top_ns >= tui->msp_ns) {
                    cur_used = tui->msp_top_ns - tui->msp_ns;
                }
                if (tui->msp_top_ns >= tui->msp_min_ns) {
                    max_used = tui->msp_top_ns - tui->msp_min_ns;
                }
                snprintf(buf, sizeof(buf), "Non-Secure stack: current %lu max %lu",
                         (unsigned long)cur_used, (unsigned long)max_used);
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            } else {
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, TUI_FG_DIM, console_bg,
                              "Non-Secure stack: Unused");
            }
            line++;
        }
    }

    if (split) {
        int split_x = inner_x + log_w;
        for (i = inner_y; i < inner_y + inner_h; ++i) {
            tui_put_cell(split_x, i, 0x2551, TUI_FG_DIM, TUI_BG_BLACK);
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
        } else if (tui->window2_mode == MM_TUI_WIN2_PERIPH) {
            int y = log_y;
            char buf[256];
            char size_buf[32];
            size_t i;
            size_t count;
            struct mm_spiflash_info flash_info;
            struct mm_tpm_tis_info tpm_info;
            struct mm_usbdev_status usb_status;

            count = mm_spiflash_count();
            if (count == 0u) {
                tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                              TUI_FG_DIM, console_bg, "SPI flash: None");
                y++;
            } else {
                for (i = 0; i < count && y < log_y + log_h; ++i) {
                    char cs_buf[16];
                    char mmap_buf[32];
                    if (!mm_spiflash_get_info(i, &flash_info)) {
                        continue;
                    }
                    tui_format_size(size_buf, sizeof(size_buf), flash_info.size);
                    snprintf(buf, sizeof(buf), "SPI flash SPI%d size=%s file=%.120s",
                             flash_info.bus, size_buf, flash_info.path);
                    tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                  console_fg, console_bg, buf);
                    y++;
                    if (y >= log_y + log_h) {
                        break;
                    }
                    if (flash_info.cs_valid) {
                        snprintf(cs_buf, sizeof(cs_buf), "P%c%d",
                                 (char)('A' + flash_info.cs_bank), flash_info.cs_pin);
                    } else {
                        snprintf(cs_buf, sizeof(cs_buf), "none");
                    }
                    if (flash_info.mmap) {
                        snprintf(mmap_buf, sizeof(mmap_buf), "0x%08lx",
                                 (unsigned long)flash_info.mmap_base);
                    } else {
                        snprintf(mmap_buf, sizeof(mmap_buf), "none");
                    }
                    snprintf(buf, sizeof(buf), "  mmap=%s  SPI settings: default  CS=%s",
                             mmap_buf, cs_buf);
                    tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                  console_fg, console_bg, buf);
                    y++;
                }
            }

            if (y < log_y + log_h) {
                y++;
            }
            count = mm_tpm_tis_count();
            if (count == 0u) {
                tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                              TUI_FG_DIM, console_bg, "TPM: None");
                y++;
            } else {
                for (i = 0; i < count && y < log_y + log_h; ++i) {
                    if (!mm_tpm_tis_get_info(i, &tpm_info)) {
                        continue;
                    }
                    snprintf(buf, sizeof(buf), "TPM SPI%d size=n/a nv=%.120s",
                             tpm_info.bus,
                             tpm_info.has_nv_path ? tpm_info.nv_path : "memory");
                    tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                  console_fg, console_bg, buf);
                    y++;
                    if (y >= log_y + log_h) {
                        break;
                    }
                    if (tpm_info.cs_valid) {
                        snprintf(buf, sizeof(buf), "  SPI settings: default  CS=P%c%d",
                                 (char)('A' + tpm_info.cs_bank),
                                 tpm_info.cs_pin);
                    } else {
                        snprintf(buf, sizeof(buf), "  SPI settings: default  CS=none");
                    }
                    tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                  console_fg, console_bg, buf);
                    y++;
                }
            }

            if (y < log_y + log_h) {
                y++;
            }
            mm_usbdev_get_status(&usb_status);
            if (!usb_status.running) {
                tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                              TUI_FG_DIM, console_bg, "USB: Not running");
                y++;
            } else {
                snprintf(buf, sizeof(buf), "USB: running=%s connected=%s imported=%s",
                         usb_status.running ? "yes" : "no",
                         usb_status.connected ? "yes" : "no",
                         usb_status.imported ? "yes" : "no");
                tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                              console_fg, console_bg, buf);
                y++;
                if (y < log_y + log_h) {
                    snprintf(buf, sizeof(buf), "  USBIP port=%d busid=%s devid=0x%08lx",
                             usb_status.port,
                             usb_status.busid,
                             (unsigned long)usb_status.devid);
                    tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                  console_fg, console_bg, buf);
                    y++;
                }
            }
        } else if (tui->window2_mode == MM_TUI_WIN2_GPIO) {
            int row;
            if (!mm_gpio_bank_reader_present()) {
                tui_draw_text(split_x + 2, log_y, inner_x + inner_w - 1,
                              console_fg, console_bg, "GPIO unavailable");
            } else {
                int y = log_y;
                for (row = 0; y < log_y + log_h; ++row) {
                    char name[16];
                    int pins = 0;
                    mm_u32 moder;
                    mm_u32 odr;
                    mm_bool clk;
                    if (!mm_gpio_bank_info(row, name, sizeof(name), &pins)) {
                        break;
                    }
                    if (pins <= 0) {
                        pins = 16;
                    }
                    moder = mm_gpio_bank_read_moder(row);
                    odr = mm_gpio_bank_read(row);
                    clk = mm_gpio_bank_clock_enabled(row);
                    tui_draw_gpio_line(split_x + 2, y, inner_x + inner_w - 1,
                                       name, pins, moder, odr, clk, console_bg);
                    y++;
                }
                if (y < log_y + log_h) {
                    y++;
                }
                for (row = 0; y < log_y + log_h; ++row) {
                    char name[16];
                    int pins = 0;
                    mm_u32 seccfgr;
                    mm_bool clk;
                    if (!mm_gpio_bank_info(row, name, sizeof(name), &pins)) {
                        break;
                    }
                    if (pins <= 0) {
                        pins = 16;
                    }
                    seccfgr = mm_gpio_bank_read_seccfgr(row);
                    clk = mm_gpio_bank_clock_enabled(row);
                    tui_draw_gpio_sec_line(split_x + 2, y, inner_x + inner_w - 1,
                                           name, pins, seccfgr, clk, console_bg);
                    y++;
                }
                if (y < log_y + log_h) {
                    y++;
                }
                if (y < log_y + log_h) {
                    y++;
                }
                if (mm_rcc_clock_list_present()) {
                    int line_idx = 0;
                    char linebuf[256];
                    tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                  TUI_FG_DIM, console_bg, "RCC clocks:");
                    y ++;
                    while (y < log_y + log_h && mm_rcc_clock_list_line(line_idx, linebuf, sizeof(linebuf))) {
                        tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                      console_fg, console_bg, linebuf);
                        y++;
                        line_idx++;
                    }
                } else if (y < log_y + log_h) {
                    tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                  TUI_FG_DIM, console_bg, "RCC clocks: unavailable");
                }
            }
        } else {
            const char *placeholder = "Not implemented";
            tui_draw_text(split_x + 2, log_y, inner_x + inner_w - 1,
                          console_fg, console_bg, placeholder);
        }
    }

    refresh();
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
    tui->window1_mode = MM_TUI_WIN1_CPU;
    tui->window2_mode = MM_TUI_WIN2_GPIO;
    tui->capstone_supported = MM_FALSE;
    tui->capstone_enabled = MM_FALSE;
    tui->image0_path[0] = '\0';
    tui->target_running = MM_TRUE;
    tui->gdb_connected = MM_FALSE;
    tui->gdb_port = 0;
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
    {
        int new_w;
        int new_h;
        getmaxyx(stdscr, new_h, new_w);
        if (new_w != tui->width || new_h != tui->height) {
            dirty = MM_TRUE;
        }
    }
    if (!dirty && tui->target_running) {
        timeout_ms = 0;
    } else {
        timeout_ms = 20;
    }
    timeout(timeout_ms);
    ev_res = getch();
    while (ev_res != ERR) {
        if (ev_res == KEY_RESIZE) {
            dirty = MM_TRUE;
        } else if (ev_res >= KEY_MIN) {
            tui_handle_key(tui, ev_res, 0, 0);
            dirty = MM_TRUE;
        } else {
            tui_handle_key(tui, 0, (uint32_t)ev_res, 0);
            dirty = MM_TRUE;
        }
        timeout(0);
        ev_res = getch();
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
    if (tui->target_running != running) {
        tui->target_running = running;
        tui->input_dirty = MM_TRUE;
    }
}

void mm_tui_set_gdb_status(struct mm_tui *tui, mm_bool connected, int port)
{
    if (tui == 0) return;
    tui->gdb_connected = connected;
    tui->gdb_port = port;
}

void mm_tui_set_capstone(struct mm_tui *tui, mm_bool supported, mm_bool enabled)
{
    if (tui == 0) return;
    tui->capstone_supported = supported;
    tui->capstone_enabled = enabled;
}

void mm_tui_set_image0(struct mm_tui *tui, const char *path)
{
    if (tui == 0 || path == 0) return;
    snprintf(tui->image0_path, sizeof(tui->image0_path), "%s", path);
}

void mm_tui_set_cpu_name(struct mm_tui *tui, const char *name)
{
    if (tui == 0) return;
    if (name == 0 || name[0] == '\0') {
        return;
    }
    if (tui->cpu_name[0] != '\0') {
        return;
    }
    snprintf(tui->cpu_name, sizeof(tui->cpu_name), "%s", name);
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
    tui->msp_top_s = cpu->msp_top_s;
    tui->msp_min_s = cpu->msp_min_s;
    tui->msp_top_ns = cpu->msp_top_ns;
    tui->msp_min_ns = cpu->msp_min_ns;
    tui->msp_top_s_valid = cpu->msp_top_s_valid;
    tui->msp_top_ns_valid = cpu->msp_top_ns_valid;
    tui->msplim_s = cpu->msplim_s;
    tui->psplim_s = cpu->psplim_s;
    tui->msplim_ns = cpu->msplim_ns;
    tui->psplim_ns = cpu->psplim_ns;
    tui->control_s = cpu->control_s;
    tui->control_ns = cpu->control_ns;
    tui->primask_s = cpu->primask_s;
    tui->primask_ns = cpu->primask_ns;
    tui->basepri_s = cpu->basepri_s;
    tui->basepri_ns = cpu->basepri_ns;
    tui->faultmask_s = cpu->faultmask_s;
    tui->faultmask_ns = cpu->faultmask_ns;
}

void mm_tui_set_memory_map(struct mm_tui *tui, const struct mm_memmap *map)
{
    if (tui == 0 || map == 0) return;
    tui->flash_base_s = map->flash_base_s;
    tui->flash_size_s = map->flash_size_s;
    tui->flash_base_ns = map->flash_base_ns;
    tui->flash_size_ns = map->flash_size_ns;
    tui->ram_base_s = map->ram_base_s;
    tui->ram_size_s = map->ram_size_s;
    tui->ram_base_ns = map->ram_base_ns;
    tui->ram_size_ns = map->ram_size_ns;
    tui->flash_total_size = map->flash_size_s + map->flash_size_ns;
    tui->ram_total_size = (map->ram_total_size != 0u) ? map->ram_total_size :
                          (map->ram_size_s + map->ram_size_ns);
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
    SCREEN *screen = NULL;
    FILE *tty = NULL;
    if (tui == 0) return 0;
    setlocale(LC_ALL, "");
    tty = fopen("/dev/tty", "r+");
    if (tty != NULL) {
        screen = newterm(NULL, tty, tty);
        if (screen != NULL) {
            set_term(screen);
        }
    }
    if (screen == NULL && initscr() == NULL) {
        fprintf(stderr, "[TUI] initscr failed\n");
        tui->actions |= MM_TUI_ACTION_QUIT;
        tui->want_quit = MM_TRUE;
        if (tty != NULL) {
            fclose(tty);
        }
        return 0;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    tui_init_colors();
    erase();
    refresh();
    tui->active = MM_TRUE;
    while (!tui->thread_stop) {
        mm_tui_poll(tui);
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 20000000L;
            nanosleep(&ts, NULL);
        }
        tui->input_dirty = MM_TRUE;
    }
    if (tui->active) {
        endwin();
        if (screen != NULL) {
            delscreen(screen);
        }
        if (tty != NULL) {
            fclose(tty);
        }
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
