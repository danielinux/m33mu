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
#include <curses.h>

#include <fcntl.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include "m33mu/cpu.h"
#include "m33mu/gpio.h"
#include "m33mu/spiflash.h"
#include "m33mu/tpm_tis.h"
#include "m33mu/usbdev.h"
#include "m33mu/gpio.h"
#include "m33mu/eth_backend.h"
#include "m33mu/memmap.h"
#include "stm32h5_eth.h"
#include "tui.h"

typedef uint32_t uintattr_t;

#define TUI_SERIAL_LABEL_NONE "NO UART"

#define TUI_RGB(r, g, b) (((uintattr_t)(r) << 16) | ((uintattr_t)(g) << 8) | (uintattr_t)(b))
#define TUI_COLOR_MASK 0x00ffffffu
#define TUI_FG_WHITE TUI_RGB(0xF5, 0xF5, 0xF5)
#define TUI_FG_DIM   TUI_RGB(0xCF, 0xCF, 0xCF)
#define TUI_FG_GREY  TUI_RGB(0x88, 0x88, 0x88)
#define TUI_FG_CYAN  TUI_RGB(0x4D, 0xD0, 0xE1)
#define TUI_FG_RED   TUI_RGB(0xE5, 0x39, 0x35)
#define TUI_FG_YELLOW TUI_RGB(0xF9, 0xA8, 0x25)
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
static mm_bool g_tui_sigint_saved = MM_FALSE;
static struct sigaction g_tui_sigint_old;

enum tui_color_slot {
    TUI_COLOR_WHITE = 0,
    TUI_COLOR_DIM,
    TUI_COLOR_GREY,
    TUI_COLOR_CYAN,
    TUI_COLOR_RED,
    TUI_COLOR_YELLOW,
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

static void tui_put_cell(int x, int y, uint32_t ch, uintattr_t fg, uintattr_t bg);

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
        case TUI_FG_YELLOW: return TUI_COLOR_YELLOW;
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

static uintattr_t tui_color_rgb_from_slot(mm_u8 slot)
{
    switch (slot) {
        case TUI_COLOR_WHITE: return TUI_FG_WHITE;
        case TUI_COLOR_DIM: return TUI_FG_DIM;
        case TUI_COLOR_GREY: return TUI_FG_GREY;
        case TUI_COLOR_CYAN: return TUI_FG_CYAN;
        case TUI_COLOR_RED: return TUI_FG_RED;
        case TUI_COLOR_YELLOW: return TUI_FG_YELLOW;
        case TUI_COLOR_GREEN: return TUI_FG_GREEN;
        case TUI_COLOR_MAGENTA: return TUI_FG_MAGENTA;
        case TUI_COLOR_BLACK: return TUI_FG_BLACK;
        case TUI_COLOR_MENU: return TUI_BG_MENU;
        case TUI_COLOR_STATUS: return TUI_BG_STATUS;
        case TUI_COLOR_STOP: return TUI_BG_STOP;
        case TUI_COLOR_RUN: return TUI_BG_RUN;
        case TUI_COLOR_NS: return TUI_BG_NS;
        default: return TUI_FG_WHITE;
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
        init_color(tui_color_ids[TUI_COLOR_YELLOW],
                   tui_rgb_to_curses(0xF9),
                   tui_rgb_to_curses(0xA8),
                   tui_rgb_to_curses(0x25));
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
        tui_color_ids[TUI_COLOR_YELLOW] = COLOR_YELLOW;
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

static uintattr_t tui_stack_usage_color(mm_u32 used, mm_u32 total)
{
    mm_u64 pct;
    if (total == 0u) return TUI_FG_GREY;
    pct = ((mm_u64)used * 100u) / (mm_u64)total;
    if (pct >= 90u) return TUI_FG_RED;
    if (pct >= 75u) return TUI_FG_YELLOW;
    return TUI_FG_GREEN;
}

static void tui_draw_stack_bar(int x, int y, int max_x, mm_u32 used, mm_u32 total)
{
    int width;
    int inner;
    int fill;
    int i;
    uintattr_t fill_fg;

    width = max_x - x + 1;
    if (width < 3) return;

    inner = width - 2;
    fill = 0;
    if (total != 0u) {
        if (used > total) used = total;
        fill = (int)(((mm_u64)used * (mm_u64)inner) / (mm_u64)total);
        if (fill > inner) fill = inner;
    }

    fill_fg = tui_stack_usage_color(used, total);
    tui_put_cell(x, y, '[', TUI_FG_DIM, TUI_BG_BLACK);
    for (i = 0; i < inner; ++i) {
        tui_put_cell(x + 1 + i, y, (i < fill) ? '#' : '-',
                     (i < fill) ? fill_fg : TUI_FG_GREY, TUI_BG_BLACK);
    }
    tui_put_cell(x + width - 1, y, ']', TUI_FG_DIM, TUI_BG_BLACK);
}

static mm_u32 tui_stack_floor(mm_u32 sp_limit, mm_u32 ram_base, mm_u32 ram_size)
{
    mm_u32 ram_end;

    if (ram_size == 0u) return sp_limit;

    ram_end = ram_base + ram_size;
    if (ram_end < ram_base) {
        ram_end = 0xffffffffu;
    }

    if (sp_limit < ram_base || sp_limit >= ram_end) {
        return ram_base;
    }
    return (sp_limit > ram_base) ? sp_limit : ram_base;
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

static struct mm_tui_uart *tui_serial_current(struct mm_tui *tui)
{
    if (tui == 0) return 0;
    if (tui->serial_count <= 0) return 0;
    if (tui->serial_selected < 0 || tui->serial_selected >= tui->serial_count) {
        tui->serial_selected = 0;
    }
    return &tui->serials[tui->serial_selected];
}

static size_t tui_serial_max_scroll(const struct mm_tui_uart *uart, size_t visible)
{
    if (uart == 0 || visible == 0) return 0;
    if (uart->line_count <= visible) return 0;
    return uart->line_count - visible;
}

static void tui_push_serial_line(struct mm_tui_uart *uart)
{
    size_t slot;
    if (uart == 0) return;
    if (uart->cur_len >= TUI_MAX_COLS) {
        uart->cur_len = TUI_MAX_COLS - 1;
    }
    uart->cur_line[uart->cur_len] = '\0';
    if (uart->line_count < TUI_MAX_LINES) {
        slot = uart->line_count++;
    } else {
        slot = uart->line_head;
        uart->line_head = (uart->line_head + 1u) % TUI_MAX_LINES;
    }
    memcpy(uart->lines[slot], uart->cur_line, uart->cur_len + 1u);
    memcpy(uart->lines_fg[slot], uart->cur_fg, uart->cur_len);
    memcpy(uart->lines_bg[slot], uart->cur_bg, uart->cur_len);
    uart->line_len[slot] = uart->cur_len;
    uart->cur_len = 0;
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

static void tui_clear_serial(struct mm_tui_uart *uart)
{
    if (uart == 0) return;
    uart->line_count = 0;
    uart->line_head = 0;
    uart->cur_len = 0;
    uart->cur_line[0] = '\0';
    uart->scroll_offset = 0;
    uart->fg = (mm_u8)TUI_COLOR_WHITE;
    uart->bg = (mm_u8)TUI_COLOR_BLACK;
}

static void tui_serial_emit(struct mm_tui_uart *uart, char c)
{
    size_t pos;
    if (uart == 0) return;
    if (uart->cur_len + 1u >= TUI_MAX_COLS) return;
    pos = uart->cur_len;
    uart->cur_line[pos] = c;
    uart->cur_fg[pos] = uart->fg;
    uart->cur_bg[pos] = uart->bg;
    uart->cur_len = pos + 1u;
}

static void tui_serial_flush_escape(struct mm_tui_uart *uart)
{
    mm_u8 i;
    if (uart == 0) return;
    for (i = 0; i < uart->esc_len; ++i) {
        char c = uart->esc_buf[i];
        if (c == '\n') {
            tui_push_serial_line(uart);
        } else if (c == '\r') {
            /* ignore */
        } else {
            tui_serial_emit(uart, c);
        }
    }
}

static mm_u8 tui_ansi_basic_color(int idx, mm_bool bright)
{
    switch (idx) {
        case 0: return bright ? TUI_COLOR_GREY : TUI_COLOR_BLACK;
        case 1: return TUI_COLOR_RED;
        case 2: return TUI_COLOR_GREEN;
        case 3: return bright ? TUI_COLOR_WHITE : TUI_COLOR_DIM;
        case 4: return TUI_COLOR_CYAN;
        case 5: return TUI_COLOR_MAGENTA;
        case 6: return TUI_COLOR_CYAN;
        case 7: return TUI_COLOR_WHITE;
        default: return TUI_COLOR_WHITE;
    }
}

static void tui_serial_apply_sgr(struct mm_tui_uart *uart, const char *params, size_t len)
{
    int codes[12];
    int count = 0;
    size_t i = 0;
    if (uart == 0) return;
    while (i < len && count < (int)(sizeof(codes) / sizeof(codes[0]))) {
        int v = 0;
        mm_bool have = MM_FALSE;
        while (i < len && params[i] >= '0' && params[i] <= '9') {
            v = (v * 10) + (params[i] - '0');
            have = MM_TRUE;
            ++i;
        }
        if (have) {
            codes[count++] = v;
        }
        if (i < len && params[i] == ';') {
            ++i;
            continue;
        }
        if (i < len && params[i] != ';') {
            ++i;
        }
    }
    if (count == 0) {
        codes[count++] = 0;
    }
    for (i = 0; i < (size_t)count; ++i) {
        int code = codes[i];
        if (code == 0) {
            uart->fg = (mm_u8)TUI_COLOR_WHITE;
            uart->bg = (mm_u8)TUI_COLOR_BLACK;
        } else if (code == 39) {
            uart->fg = (mm_u8)TUI_COLOR_WHITE;
        } else if (code == 49) {
            uart->bg = (mm_u8)TUI_COLOR_BLACK;
        } else if (code >= 30 && code <= 37) {
            uart->fg = tui_ansi_basic_color(code - 30, MM_FALSE);
        } else if (code >= 90 && code <= 97) {
            uart->fg = tui_ansi_basic_color(code - 90, MM_TRUE);
        } else if (code >= 40 && code <= 47) {
            uart->bg = tui_ansi_basic_color(code - 40, MM_FALSE);
        } else if (code >= 100 && code <= 107) {
            uart->bg = tui_ansi_basic_color(code - 100, MM_TRUE);
        } else if (code == 38 || code == 48) {
            if (i + 1u < (size_t)count) {
                if (codes[i + 1] == 5 && (i + 2u) < (size_t)count) {
                    i += 2u;
                } else if (codes[i + 1] == 2 && (i + 4u) < (size_t)count) {
                    i += 4u;
                } else {
                    i += 1u;
                }
            }
        }
    }
}

static mm_bool tui_serial_handle_escape(struct mm_tui_uart *uart)
{
    size_t len;
    char final;
    if (uart == 0) return MM_FALSE;
    if (uart->esc_len < 2u) return MM_FALSE;
    if (uart->esc_buf[0] != '\x1b' || uart->esc_buf[1] != '[') return MM_FALSE;
    len = (size_t)uart->esc_len;
    final = uart->esc_buf[len - 1u];
    if (final == 'J') {
        if (len <= 3u || uart->esc_buf[2] == '2') {
            tui_clear_serial(uart);
            return MM_TRUE;
        }
        return MM_FALSE;
    }
    if (final == 'H' || final == 'f') {
        return MM_TRUE;
    }
    if (final == 'm') {
        if (len > 3u) {
            tui_serial_apply_sgr(uart, &uart->esc_buf[2], len - 3u);
        } else {
            tui_serial_apply_sgr(uart, "", 0);
        }
        return MM_TRUE;
    }
    return MM_FALSE;
}

static void tui_append_serial_text(struct mm_tui_uart *uart, const char *buf, size_t len)
{
    size_t i;
    if (uart == 0) return;
    for (i = 0; i < len; ++i) {
        char c = buf[i];
        if (uart->esc_active) {
            if (uart->esc_len + 1u < (mm_u8)sizeof(uart->esc_buf)) {
                uart->esc_buf[uart->esc_len++] = c;
            } else {
                tui_serial_flush_escape(uart);
                uart->esc_len = 0;
                uart->esc_active = MM_FALSE;
            }
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                uart->esc_buf[uart->esc_len] = '\0';
                if (!tui_serial_handle_escape(uart)) {
                    tui_serial_flush_escape(uart);
                }
                uart->esc_len = 0;
                uart->esc_active = MM_FALSE;
            }
            continue;
        }
        if (c == '\x1b') {
            uart->esc_active = MM_TRUE;
            uart->esc_len = 0;
            uart->esc_buf[uart->esc_len++] = c;
            continue;
        }
        if (c == '\n') {
            tui_push_serial_line(uart);
        } else if (c == '\r') {
            /* ignore */
        } else {
            tui_serial_emit(uart, c);
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
    int i;
    if (tui == 0) return MM_FALSE;
    for (i = 0; i < tui->serial_count; ++i) {
        struct mm_tui_uart *uart = &tui->serials[i];
        if (uart->fd < 0) continue;
        n = read(uart->fd, buf, (int)sizeof(buf));
        while (n > 0) {
            tui_append_serial_text(uart, buf, (size_t)n);
            changed = MM_TRUE;
            n = read(uart->fd, buf, (int)sizeof(buf));
        }
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

static void tui_draw_serial_line(int x, int y, int max_x, const char *text,
                                 const mm_u8 *fg, const mm_u8 *bg, size_t len)
{
    int cx = x;
    size_t i;
    for (i = 0; i < len && cx < max_x; ++i) {
        uintattr_t fg_rgb = tui_color_rgb_from_slot(fg[i]);
        uintattr_t bg_rgb = tui_color_rgb_from_slot(bg[i]);
        tui_put_cell(cx, y, (uint32_t)text[i], tui_attr(fg_rgb), tui_attr(bg_rgb));
        ++cx;
    }
}

static void tui_draw_filled(int x0, int y0, int x1, int y1, uintattr_t fg, uintattr_t bg);

static void tui_draw_quit_prompt(const struct mm_tui *tui, int w, int h)
{
    int box_w = 44;
    int box_h = 7;
    int x0;
    int y0;
    int i;
    int x;
    uintattr_t fg = TUI_FG_WHITE;
    uintattr_t bg = TUI_BG_MENU;
    uintattr_t sel_fg = TUI_FG_BLACK;
    uintattr_t sel_bg = TUI_BG_STATUS;
    int yes_x;
    int no_x;
    if (tui == 0) return;
    if (box_w > w - 2) box_w = w - 2;
    if (box_h > h - 2) box_h = h - 2;
    if (box_w < 20 || box_h < 5) return;
    x0 = (w - box_w) / 2;
    y0 = (h - box_h) / 2;
    tui_draw_filled(x0, y0, x0 + box_w - 1, y0 + box_h - 1, fg, bg);
    for (x = x0 + 1; x < x0 + box_w - 1; ++x) {
        tui_put_cell(x, y0, 0x2500, tui_attr(TUI_FG_DIM), tui_attr(bg));
        tui_put_cell(x, y0 + box_h - 1, 0x2500, tui_attr(TUI_FG_DIM), tui_attr(bg));
    }
    for (i = y0 + 1; i < y0 + box_h - 1; ++i) {
        tui_put_cell(x0, i, 0x2502, tui_attr(TUI_FG_DIM), tui_attr(bg));
        tui_put_cell(x0 + box_w - 1, i, 0x2502, tui_attr(TUI_FG_DIM), tui_attr(bg));
    }
    tui_put_cell(x0, y0, 0x250c, tui_attr(TUI_FG_DIM), tui_attr(bg));
    tui_put_cell(x0 + box_w - 1, y0, 0x2510, tui_attr(TUI_FG_DIM), tui_attr(bg));
    tui_put_cell(x0, y0 + box_h - 1, 0x2514, tui_attr(TUI_FG_DIM), tui_attr(bg));
    tui_put_cell(x0 + box_w - 1, y0 + box_h - 1, 0x2518, tui_attr(TUI_FG_DIM), tui_attr(bg));

    tui_draw_text(x0 + 2, y0 + 2, x0 + box_w - 2, fg, bg,
                  "Are you sure you want to quit m33mu?");
    yes_x = x0 + (box_w / 2) - 9;
    no_x = x0 + (box_w / 2) + 3;
    if (tui->quit_choice) {
        tui_draw_text(yes_x, y0 + 4, x0 + box_w - 2, sel_fg, sel_bg, "> Yes <");
        tui_draw_text(no_x, y0 + 4, x0 + box_w - 2, fg, bg, "  No  ");
    } else {
        tui_draw_text(yes_x, y0 + 4, x0 + box_w - 2, fg, bg, "  Yes ");
        tui_draw_text(no_x, y0 + 4, x0 + box_w - 2, sel_fg, sel_bg, "> No <");
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
            if (tui != 0 && tui->serial_count > 0) {
                int sel = tui->serial_selected;
                if (sel < 0 || sel >= tui->serial_count) {
                    sel = 0;
                }
                if (tui->serials[sel].label[0] != '\0') {
                    return tui->serials[sel].label;
                }
            }
            return TUI_SERIAL_LABEL_NONE;
    }
}

static void tui_handle_key(struct mm_tui *tui, int key, uint32_t ch, uint8_t mod)
{
    if (tui == 0) return;
    (void)mod;

    if (tui->quit_prompt) {
        if (key == KEY_LEFT || key == KEY_RIGHT || ch == '\t') {
            tui->quit_choice = (mm_u8)(tui->quit_choice ? 0u : 1u);
            tui->input_dirty = MM_TRUE;
        } else if (key == KEY_ENTER || ch == '\n' || ch == '\r') {
            if (tui->quit_choice) {
                tui->want_quit = MM_TRUE;
                tui->actions |= MM_TUI_ACTION_QUIT;
            } else {
                tui->quit_prompt = MM_FALSE;
                tui->input_dirty = MM_TRUE;
            }
        } else if (ch == 'y' || ch == 'Y') {
            tui->quit_choice = 1u;
            tui->want_quit = MM_TRUE;
            tui->actions |= MM_TUI_ACTION_QUIT;
        } else if (ch == 'n' || ch == 'N') {
            tui->quit_choice = 0u;
            tui->quit_prompt = MM_FALSE;
            tui->input_dirty = MM_TRUE;
        }
        return;
    }
    if (ch == 0x18u) {
        tui->quit_prompt = MM_TRUE;
        tui->quit_choice = 0;
        tui->input_dirty = MM_TRUE;
        return;
    }
    if (tui->window2_mode == MM_TUI_WIN2_UART) {
        struct mm_tui_uart *uart = tui_serial_current(tui);
        mm_u8 b = 0;
        mm_bool send = MM_FALSE;
        const char *seq = 0;
        size_t seq_len = 0;
        ssize_t w;
        if (key == KEY_ENTER || ch == '\n' || ch == '\r') {
            b = '\r';
            send = MM_TRUE;
        } else if (key == KEY_BACKSPACE || ch == 0x7Fu || ch == 0x08u) {
            b = 0x7Fu;
            send = MM_TRUE;
        } else if (key == KEY_UP) {
            seq = "\x1b[A";
            seq_len = 3u;
        } else if (key == KEY_DOWN) {
            seq = "\x1b[B";
            seq_len = 3u;
        } else if (key == KEY_RIGHT) {
            seq = "\x1b[C";
            seq_len = 3u;
        } else if (key == KEY_LEFT) {
            seq = "\x1b[D";
            seq_len = 3u;
        } else if (ch != 0) {
            b = (mm_u8)ch;
            send = MM_TRUE;
        }
        if (seq != 0 && uart != 0 && uart->fd >= 0) {
            w = write(uart->fd, seq, seq_len);
            (void)w;
            return;
        }
        if (send && uart != 0 && uart->fd >= 0) {
            w = write(uart->fd, &b, 1);
            (void)w;
        }
    }
    if (key == KEY_PPAGE || key == KEY_NPAGE) {
        if (tui->window2_mode == MM_TUI_WIN2_UART) {
            struct mm_tui_uart *uart = tui_serial_current(tui);
            if (uart != 0 && tui->window2_page_lines > 0) {
                size_t step = (size_t)tui->window2_page_lines;
                size_t max_scroll = tui_serial_max_scroll(uart, (size_t)tui->window2_page_lines);
                if (key == KEY_PPAGE) {
                    if (uart->scroll_offset + step > max_scroll) {
                        uart->scroll_offset = max_scroll;
                    } else {
                        uart->scroll_offset += step;
                    }
                } else {
                    if (uart->scroll_offset > step) {
                        uart->scroll_offset -= step;
                    } else {
                        uart->scroll_offset = 0;
                    }
                }
                tui->input_dirty = MM_TRUE;
            }
        }
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
        if (tui->serial_count > 0) {
            if (tui->window2_mode == MM_TUI_WIN2_UART) {
                tui->window2_mode = MM_TUI_WIN2_PERIPH;
            } else if (tui->window2_mode == MM_TUI_WIN2_PERIPH) {
                tui->window2_mode = MM_TUI_WIN2_GPIO;
            } else {
                tui->window2_mode = MM_TUI_WIN2_UART;
                if (tui->serial_count > 1) {
                    tui->serial_selected = (tui->serial_selected + 1) % tui->serial_count;
                } else {
                    tui->serial_selected = 0;
                }
            }
        } else {
            tui->window2_mode = (mm_u8)((tui->window2_mode + 1u) % 2u);
            if (tui->window2_mode == MM_TUI_WIN2_UART) {
                tui->window2_mode = MM_TUI_WIN2_PERIPH;
            }
        }
        tui->input_dirty = MM_TRUE;
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
    mm_bool show_nonsecure_stack;

    getmaxyx(stdscr, h, w);
    if (w <= 0 || h <= 3) return;
    show_nonsecure_stack = (tui->msp_top_ns_valid || tui->psp_top_ns_valid) ? MM_TRUE : MM_FALSE;
    tui->width = w;
    tui->height = h;
    tui_draw_filled(0, 0, w - 1, h - 1, TUI_FG_WHITE, TUI_BG_BLACK);
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
    tui_draw_text(console_w + 2, 19, w - 1, menu_fg, menu_bg, "Quit (Ctrl+X)");

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
        if (!tui->target_running && tui->func_valid && tui->func_name[0] != '\0') {
            tui_draw_text(1, h - 1, w - 1, status_fg, status_bg, tui->func_name);
        } else {
            tui_draw_text(1, h - 1, w - 1, status_fg, status_bg,
                          (tui->command_line[0] != '\0') ? tui->command_line : "m33mu --tui");
        }
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
        int reg_cell_w = 14;
        int reg_gap = 1;
        int reg_cols = 1;
        char buf[192];
        char size_buf[32];
        if (log_w >= ((reg_cell_w * 3) + (reg_gap * 2))) {
            reg_cols = 3;
        } else if (log_w >= ((reg_cell_w * 2) + reg_gap)) {
            reg_cols = 2;
        }
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
            buf[0] = '\0';
            if (tui->flash_size_s > 0u) {
                tui_format_size(size_buf, sizeof(size_buf), tui->flash_size_s);
                snprintf(buf, sizeof(buf), "FLASH S 0x%08lx-0x%08lx (%s)",
                         (unsigned long)tui->flash_base_s,
                         (unsigned long)(tui->flash_base_s + tui->flash_size_s - 1u),
                         size_buf);
            }
            if (tui->flash_size_ns > 0u) {
                size_t len = strlen(buf);
                tui_format_size(size_buf, sizeof(size_buf), tui->flash_size_ns);
                snprintf(buf + len, sizeof(buf) - len, "%sNS 0x%08lx-0x%08lx (%s)",
                         (len > 0u) ? ", " : "FLASH ",
                         (unsigned long)tui->flash_base_ns,
                         (unsigned long)(tui->flash_base_ns + tui->flash_size_ns - 1u),
                         size_buf);
            }
            if (buf[0] != '\0') {
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                line++;
            }
        }
        if (line < log_h) {
            buf[0] = '\0';
            if (tui->ram_size_s > 0u) {
                tui_format_size(size_buf, sizeof(size_buf), tui->ram_size_s);
                snprintf(buf, sizeof(buf), "RAM S 0x%08lx-0x%08lx (%s)",
                         (unsigned long)tui->ram_base_s,
                         (unsigned long)(tui->ram_base_s + tui->ram_size_s - 1u),
                         size_buf);
            }
            if (tui->ram_size_ns > 0u) {
                size_t len = strlen(buf);
                tui_format_size(size_buf, sizeof(size_buf), tui->ram_size_ns);
                snprintf(buf + len, sizeof(buf) - len, "%sNS 0x%08lx-0x%08lx (%s)",
                         (len > 0u) ? ", " : "RAM ",
                         (unsigned long)tui->ram_base_ns,
                         (unsigned long)(tui->ram_base_ns + tui->ram_size_ns - 1u),
                         size_buf);
            }
            if (buf[0] != '\0') {
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                line++;
            }
        }
        if (line < log_h) {
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
            tui_draw_text(inner_x, log_y + line, inner_x + log_w,
                          show_nonsecure_stack ? console_fg : TUI_FG_DIM, console_bg, buf);
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
            tui_draw_text(inner_x, log_y + line, inner_x + log_w,
                          show_nonsecure_stack ? console_fg : TUI_FG_DIM, console_bg, buf);
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
        /* Stack usage bars: show each SP bank that has been used.
         * Highlight the currently active bank.
         */
        {
            mm_bool use_psp;
            mm_bool active_ns;
            /* Determine currently active SP bank */
            active_ns = (tui->core_sec != MM_SECURE);
            if (tui->core_mode == MM_HANDLER) {
                use_psp = MM_FALSE;
            } else {
                mm_u32 ctrl = active_ns ? tui->control_ns : tui->control_s;
                use_psp = (ctrl & 0x2u) != 0u;
            }
            /* MSP_S */
            if (tui->msp_top_s_valid && line < log_h) {
                mm_u32 cur_used = 0, max_used = 0, total = 0;
                mm_u32 floor = tui_stack_floor(tui->msplim_s, tui->ram_base_s, tui->ram_size_s);
                mm_bool unbounded = (tui->msplim_s == 0u) ? MM_TRUE : MM_FALSE;
                mm_bool is_active = (!active_ns && !use_psp);
                char cur_buf[32], total_buf[32], max_buf[32];
                if (tui->msp_top_s >= tui->msp_s)
                    cur_used = tui->msp_top_s - tui->msp_s;
                if (tui->msp_top_s >= tui->msp_min_s)
                    max_used = tui->msp_top_s - tui->msp_min_s;
                if (tui->msp_top_s >= floor)
                    total = tui->msp_top_s - floor;
                tui_format_size(cur_buf, sizeof(cur_buf), cur_used);
                tui_format_size(total_buf, sizeof(total_buf), total);
                tui_format_size(max_buf, sizeof(max_buf), max_used);
                snprintf(buf, sizeof(buf), "%sMSP_S: %s used/%s max. %s total%s",
                         is_active ? "> " : "  ",
                         cur_buf, max_buf, total_buf, unbounded ? " (unbounded)" : "");
                tui_draw_text(inner_x, log_y + line, inner_x + log_w,
                              is_active ? console_fg : TUI_FG_DIM, console_bg, buf);
                line++;
                if (line < log_h) {
                    tui_draw_stack_bar(inner_x, log_y + line, inner_x + log_w - 1, cur_used, max_used);
                    line++;
                }
            }
            /* PSP_S */
            if (tui->psp_top_s_valid && line < log_h) {
                mm_u32 cur_used = 0, max_used = 0, total = 0;
                mm_u32 floor = tui_stack_floor(tui->psplim_s, tui->ram_base_s, tui->ram_size_s);
                mm_bool unbounded = (tui->psplim_s == 0u) ? MM_TRUE : MM_FALSE;
                mm_bool is_active = (!active_ns && use_psp);
                char cur_buf[32], total_buf[32], max_buf[32];
                if (tui->psp_top_s >= tui->psp_s)
                    cur_used = tui->psp_top_s - tui->psp_s;
                if (tui->psp_top_s >= tui->psp_min_s)
                    max_used = tui->psp_top_s - tui->psp_min_s;
                if (tui->psp_top_s >= floor)
                    total = tui->psp_top_s - floor;
                tui_format_size(cur_buf, sizeof(cur_buf), cur_used);
                tui_format_size(total_buf, sizeof(total_buf), total);
                tui_format_size(max_buf, sizeof(max_buf), max_used);
                snprintf(buf, sizeof(buf), "%sPSP_S: %s used/%s max. %s total%s",
                         is_active ? "> " : "  ",
                         cur_buf, max_buf, total_buf, unbounded ? " (unbounded)" : "");
                tui_draw_text(inner_x, log_y + line, inner_x + log_w,
                              is_active ? console_fg : TUI_FG_DIM, console_bg, buf);
                line++;
                if (line < log_h) {
                    tui_draw_stack_bar(inner_x, log_y + line, inner_x + log_w - 1, cur_used, max_used);
                    line++;
                }
            }
            /* MSP_NS */
            if (show_nonsecure_stack && tui->msp_top_ns_valid && line < log_h) {
                mm_u32 cur_used = 0, max_used = 0, total = 0;
                mm_u32 floor = tui_stack_floor(tui->msplim_ns, tui->ram_base_ns, tui->ram_size_ns);
                mm_bool unbounded = (tui->msplim_ns == 0u) ? MM_TRUE : MM_FALSE;
                mm_bool is_active = (active_ns && !use_psp);
                char cur_buf[32], total_buf[32], max_buf[32];
                if (tui->msp_top_ns >= tui->msp_ns)
                    cur_used = tui->msp_top_ns - tui->msp_ns;
                if (tui->msp_top_ns >= tui->msp_min_ns)
                    max_used = tui->msp_top_ns - tui->msp_min_ns;
                if (tui->msp_top_ns >= floor)
                    total = tui->msp_top_ns - floor;
                tui_format_size(cur_buf, sizeof(cur_buf), cur_used);
                tui_format_size(total_buf, sizeof(total_buf), total);
                tui_format_size(max_buf, sizeof(max_buf), max_used);
                snprintf(buf, sizeof(buf), "%sMSP_NS: %s used/%s max. %s total%s",
                         is_active ? "> " : "  ",
                         cur_buf, max_buf, total_buf, unbounded ? " (unbounded)" : "");
                tui_draw_text(inner_x, log_y + line, inner_x + log_w,
                              is_active ? console_fg : TUI_FG_DIM, console_bg, buf);
                line++;
                if (line < log_h) {
                    tui_draw_stack_bar(inner_x, log_y + line, inner_x + log_w - 1, cur_used, max_used);
                    line++;
                }
            }
            /* PSP_NS */
            if (show_nonsecure_stack && tui->psp_top_ns_valid && line < log_h) {
                mm_u32 cur_used = 0, max_used = 0, total = 0;
                mm_u32 floor = tui_stack_floor(tui->psplim_ns, tui->ram_base_ns, tui->ram_size_ns);
                mm_bool unbounded = (tui->psplim_ns == 0u) ? MM_TRUE : MM_FALSE;
                mm_bool is_active = (active_ns && use_psp);
                char cur_buf[32], total_buf[32], max_buf[32];
                if (tui->psp_top_ns >= tui->psp_ns)
                    cur_used = tui->psp_top_ns - tui->psp_ns;
                if (tui->psp_top_ns >= tui->psp_min_ns)
                    max_used = tui->psp_top_ns - tui->psp_min_ns;
                if (tui->psp_top_ns >= floor)
                    total = tui->psp_top_ns - floor;
                tui_format_size(cur_buf, sizeof(cur_buf), cur_used);
                tui_format_size(total_buf, sizeof(total_buf), total);
                tui_format_size(max_buf, sizeof(max_buf), max_used);
                snprintf(buf, sizeof(buf), "%sPSP_NS: %s used/%s max. %s total%s",
                         is_active ? "> " : "  ",
                         cur_buf, max_buf, total_buf, unbounded ? " (unbounded)" : "");
                tui_draw_text(inner_x, log_y + line, inner_x + log_w,
                              is_active ? console_fg : TUI_FG_DIM, console_bg, buf);
                line++;
                if (line < log_h) {
                    tui_draw_stack_bar(inner_x, log_y + line, inner_x + log_w - 1, cur_used, max_used);
                    line++;
                }
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
        {
            int reg_rows = (16 + reg_cols - 1) / reg_cols;
            int reg;
            for (i = 0; i < reg_rows && line < log_h; ++i, ++line) {
                int c;
                for (c = 0; c < reg_cols; ++c) {
                    int x = inner_x + c * (reg_cell_w + reg_gap);
                    reg = i + (c * reg_rows);
                    if (reg >= 16 || x >= inner_x + log_w) continue;
                    snprintf(buf, sizeof(buf), "r%-2d 0x%08lx", reg, (unsigned long)tui->regs[reg]);
                    tui_draw_text(x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                }
            }
        }
        if (line < log_h) {
            snprintf(buf, sizeof(buf), "xpsr 0x%08lx", (unsigned long)tui->xpsr);
            tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
            line++;
        }
        if (tui->fpu_enabled) {
            if (line < log_h) {
                line++;
            }
            if (line < log_h) {
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, "FPU Registers:");
                line++;
            }
            if (line < log_h) {
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, "==============");
                line++;
            }
            if (line < log_h) {
                snprintf(buf, sizeof(buf), "fpscr 0x%08lx", (unsigned long)tui->fpscr);
                tui_draw_text(inner_x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                line++;
            }
            {
                int fpu_rows = (32 + reg_cols - 1) / reg_cols;
                int reg;
                for (i = 0; i < fpu_rows && line < log_h; ++i, ++line) {
                    int c;
                    for (c = 0; c < reg_cols; ++c) {
                        int x = inner_x + c * (reg_cell_w + reg_gap);
                        reg = i + (c * fpu_rows);
                        if (reg >= 32 || x >= inner_x + log_w) continue;
                        snprintf(buf, sizeof(buf), "s%-2d 0x%08lx", reg, (unsigned long)tui->fpu_regs[reg]);
                        tui_draw_text(x, log_y + line, inner_x + log_w, console_fg, console_bg, buf);
                    }
                }
            }
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
            struct mm_tui_uart *uart = tui_serial_current(tui);
            tui->window2_page_lines = log_h;
            if (uart != 0) {
                available = (size_t)log_h;
                if (uart->line_count > available) {
                    size_t max_scroll = tui_serial_max_scroll(uart, available);
                    if (uart->scroll_offset > max_scroll) {
                        uart->scroll_offset = max_scroll;
                    }
                    start = uart->line_count - available - uart->scroll_offset;
                } else {
                    start = 0;
                }
                for (i = 0; i < log_h; ++i) {
                    size_t idx = start + (size_t)i;
                    if (idx < uart->line_count) {
                        size_t slot = (uart->line_head + idx) % TUI_MAX_LINES;
                        tui_draw_serial_line(split_x + 1, log_y + i, inner_x + inner_w - 1,
                                             uart->lines[slot], uart->lines_fg[slot],
                                             uart->lines_bg[slot], uart->line_len[slot]);
                    }
                }
                if (uart->cur_len > 0 && log_h > 0) {
                    uart->cur_line[uart->cur_len] = '\0';
                    tui_draw_serial_line(split_x + 1, log_y + log_h - 1, inner_x + inner_w - 1,
                                         uart->cur_line, uart->cur_fg, uart->cur_bg, uart->cur_len);
                }
            }
        } else if (tui->window2_mode == MM_TUI_WIN2_PERIPH) {
            int y = log_y;
            char buf[256];
            char size_buf[32];
            char mac_buf[32];
            size_t i;
            size_t count;
            struct mm_spiflash_info flash_info;
#ifdef M33MU_HAS_LIBTPMS
            struct mm_tpm_tis_info tpm_info;
#endif
            struct mm_usbdev_status usb_status;
            enum mm_eth_backend_type eth_backend;
            mm_u8 eth_mac[6];
            mm_bool eth_mac_ok;
            mm_bool eth_link;
            tui->window2_page_lines = 0;

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
#ifdef M33MU_HAS_LIBTPMS
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
#else
            tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                          TUI_FG_DIM, console_bg, "TPM: Not built");
            y++;
#endif

            if (y < log_y + log_h) {
                y++;
            }
            mm_usbdev_get_status(&usb_status);
            if (!usb_status.running) {
                tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                              TUI_FG_DIM, console_bg, "USB: Not running");
                y++;
            } else {
                snprintf(buf, sizeof(buf), "USB: running=%s connected=%s configured=%s",
                         usb_status.running ? "yes" : "no",
                         usb_status.connected ? "yes" : "no",
                         usb_status.configured ? "yes" : "no");
                tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                              console_fg, console_bg, buf);
                y++;
                if (y < log_y + log_h) {
                    snprintf(buf, sizeof(buf), "  UDC=%s", usb_status.udc);
                    tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                                  console_fg, console_bg, buf);
                    y++;
                }
            }

            if (y < log_y + log_h) {
                y++;
            }
            eth_backend = mm_eth_backend_type_get();
            eth_link = mm_eth_backend_link_up();
            eth_mac_ok = stm32h5_eth_get_mac(eth_mac);
            if (eth_mac_ok) {
                snprintf(mac_buf, sizeof(mac_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                         eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
            } else {
                snprintf(mac_buf, sizeof(mac_buf), "n/a");
            }
            if (eth_backend == MM_ETH_BACKEND_NONE) {
                tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                              TUI_FG_DIM, console_bg, "ETH: Disabled");
                y++;
            } else {
                const char *spec = mm_eth_backend_spec();
                const char *backend_name = (eth_backend == MM_ETH_BACKEND_TAP) ? "tap" : "vde";
                const char *spec_label = (eth_backend == MM_ETH_BACKEND_TAP) ? "iface" : "sock";
                snprintf(buf, sizeof(buf), "ETH: backend=%s %s=%s conn=%s link=%s mac=%s",
                         backend_name,
                         spec_label,
                         (spec != 0 && spec[0] != '\0') ? spec : "n/a",
                         mm_eth_backend_is_up() ? "yes" : "no",
                         eth_link ? "up" : "down",
                         mac_buf);
                tui_draw_text(split_x + 2, y, inner_x + inner_w - 1,
                              console_fg, console_bg, buf);
                y++;
            }
        } else if (tui->window2_mode == MM_TUI_WIN2_GPIO) {
            int row;
            tui->window2_page_lines = 0;
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
            tui->window2_page_lines = 0;
            tui_draw_text(split_x + 2, log_y, inner_x + inner_w - 1,
                          console_fg, console_bg, placeholder);
        }
    }

    if (tui->quit_prompt) {
        tui_draw_quit_prompt(tui, w, h);
    }
    refresh();
}

mm_bool mm_tui_init(struct mm_tui *tui)
{
    int ttyfd;
    int i;
    if (tui == 0) return MM_FALSE;
    memset(tui, 0, sizeof(*tui));
    tui->log_fd = -1;
    tui->log_read_fd = -1;
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
    tui->serial_count = 0;
    tui->serial_selected = 0;
    tui->window2_page_lines = 0;
    tui->quit_prompt = MM_FALSE;
    tui->quit_choice = 0;
    for (i = 0; i < TUI_MAX_UARTS; ++i) {
        tui->serials[i].fd = -1;
    }
    return MM_TRUE;
}

void mm_tui_shutdown(struct mm_tui *tui)
{
    if (tui == 0) return;
    mm_tui_stop_thread(tui);
    tui->active = MM_FALSE;
    mm_tui_close_devices(tui);
    if (tui->log_fd >= 0) close(tui->log_fd);
    if (tui->log_read_fd >= 0) close(tui->log_read_fd);
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
    tui->func_pc = 0u;
    tui->func_valid = MM_FALSE;
    tui->func_name[0] = '\0';
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

void mm_tui_set_command_line(struct mm_tui *tui, const char *cmdline)
{
    size_t len;
    if (tui == 0) return;
    if (cmdline == 0 || cmdline[0] == '\0') {
        tui->command_line[0] = '\0';
        return;
    }
    len = strlen(cmdline);
    if (len >= sizeof(tui->command_line)) {
        len = sizeof(tui->command_line) - 1u;
    }
    memcpy(tui->command_line, cmdline, len);
    tui->command_line[len] = '\0';
}

void mm_tui_set_function(struct mm_tui *tui, mm_u32 pc, const char *name)
{
    size_t len;
    if (tui == 0) return;
    tui->func_pc = pc;
    if (name == 0 || name[0] == '\0') {
        tui->func_name[0] = '\0';
        tui->func_valid = MM_FALSE;
        return;
    }
    len = strlen(name);
    if (len >= sizeof(tui->func_name)) {
        len = sizeof(tui->func_name) - 1u;
    }
    memcpy(tui->func_name, name, len);
    tui->func_name[len] = '\0';
    tui->func_valid = MM_TRUE;
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

void mm_tui_set_registers(struct mm_tui *tui, const struct mm_cpu *cpu, mm_bool fpu_enabled)
{
    int i;
    if (tui == 0 || cpu == 0) return;
    for (i = 0; i < 16; ++i) {
        tui->regs[i] = cpu->r[i];
    }
    for (i = 0; i < 32; ++i) {
        tui->fpu_regs[i] = cpu->s[i];
    }
    tui->xpsr = cpu->xpsr;
    tui->fpscr = cpu->fpscr;
    tui->fpu_enabled = fpu_enabled ? MM_TRUE : MM_FALSE;
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
    tui->psp_top_s = cpu->psp_top_s;
    tui->psp_min_s = cpu->psp_min_s;
    tui->psp_top_ns = cpu->psp_top_ns;
    tui->psp_min_ns = cpu->psp_min_ns;
    tui->psp_top_s_valid = cpu->psp_top_s_valid;
    tui->psp_top_ns_valid = cpu->psp_top_ns_valid;
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
    int i;
    if (tui == 0) return;
    for (i = 0; i < tui->serial_count; ++i) {
        struct mm_tui_uart *uart = &tui->serials[i];
        if (uart->fd >= 0) {
            close(uart->fd);
            uart->fd = -1;
        }
        uart->line_count = 0;
        uart->line_head = 0;
        uart->cur_len = 0;
        uart->cur_line[0] = '\0';
        uart->fg = (mm_u8)TUI_COLOR_WHITE;
        uart->bg = (mm_u8)TUI_COLOR_BLACK;
    }
    tui->serial_count = 0;
    tui->serial_selected = 0;
    if (tui->window2_mode == MM_TUI_WIN2_UART) {
        tui->window2_mode = MM_TUI_WIN2_GPIO;
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
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        if (sigaction(SIGINT, &sa, &g_tui_sigint_old) == 0) {
            g_tui_sigint_saved = MM_TRUE;
        }
    }
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
        if (g_tui_sigint_saved) {
            (void)sigaction(SIGINT, &g_tui_sigint_old, NULL);
            g_tui_sigint_saved = MM_FALSE;
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
    const char *want = getenv("M33MU_TUI_UART");
    int i;
    struct mm_tui_uart *uart;
    if (g_tui == 0 || !g_tui->active) return;
    if (path == 0) return;
    if (want && label && strcmp(want, label) != 0) {
        return;
    }
    for (i = 0; i < g_tui->serial_count; ++i) {
        if (strcmp(g_tui->serials[i].path, path) == 0) {
            return;
        }
    }
    if (g_tui->serial_count >= TUI_MAX_UARTS) return;
    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return;
    if (tcgetattr(fd, &tio) == 0) {
        tio.c_iflag &= ~(ICRNL | INLCR | IGNCR);
        tio.c_lflag &= ~(ICANON | ECHO);
        tio.c_oflag &= ~(OPOST | ONLCR);
        (void)tcsetattr(fd, TCSANOW, &tio);
    }
    uart = &g_tui->serials[g_tui->serial_count];
    memset(uart, 0, sizeof(*uart));
    uart->fd = fd;
    uart->fg = (mm_u8)TUI_COLOR_WHITE;
    uart->bg = (mm_u8)TUI_COLOR_BLACK;
    if (label != 0 && label[0] != '\0') {
        snprintf(uart->label, sizeof(uart->label), "%s", label);
    } else {
        snprintf(uart->label, sizeof(uart->label), "%s", TUI_SERIAL_LABEL_NONE);
    }
    snprintf(uart->path, sizeof(uart->path), "%s", path);
    g_tui->serial_count++;
    if (g_tui->serial_count == 1) {
        g_tui->serial_selected = 0;
    }
    g_tui->input_dirty = MM_TRUE;
}
