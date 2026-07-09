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

#define _XOPEN_SOURCE 600
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "m33mu/target_hal.h"

mm_bool mm_tui_is_active(void);

static mm_bool g_uart_stdout = MM_FALSE;
static int g_uart_rx_trace = -1;
static const struct mm_target_cfg *g_target_cfg = 0;

#define MM_UART_BACKEND_MAX 8

struct mm_uart_backend_binding {
    mm_bool used;
    mm_u32 base;
    char name[64];
    const struct mm_uart_backend_ops *ops;
    void *opaque;
};

static struct mm_uart_backend_binding g_uart_backends[MM_UART_BACKEND_MAX];

#define MM_TARGET_CALL0(field) \
    do { \
        if (cfg == 0 || cfg->field == 0) { \
            return; \
        } \
        cfg->field(); \
    } while (0)

#define MM_TARGET_CALL2(field, arg0, arg1) \
    do { \
        if (cfg == 0 || cfg->field == 0) { \
            return; \
        } \
        cfg->field((arg0), (arg1)); \
    } while (0)

static mm_bool uart_rx_trace_enabled(void)
{
    if (g_uart_rx_trace < 0) {
        const char *v = getenv("M33MU_UART_RX_TRACE");
        g_uart_rx_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_uart_rx_trace ? MM_TRUE : MM_FALSE;
}

static int uart_open_pty(char *out, size_t outlen)
{
    int fd;
    const char *name;
    int fl;
    struct termios tio;

    fd = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    if (grantpt(fd) != 0 || unlockpt(fd) != 0) {
        close(fd);
        return -1;
    }
    name = ptsname(fd);
    if (name == 0) {
        close(fd);
        return -1;
    }
    snprintf(out, outlen, "%s", name);

    fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    if (tcgetattr(fd, &tio) == 0) {
        tio.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

static const struct mm_uart_backend_binding *uart_find_backend(mm_u32 base)
{
    size_t i;
    for (i = 0; i < MM_UART_BACKEND_MAX; ++i) {
        if (g_uart_backends[i].used && g_uart_backends[i].base == base) {
            return &g_uart_backends[i];
        }
    }
    return 0;
}

void mm_uart_io_init(struct mm_uart_io *io)
{
    if (io == 0) return;
    memset(io, 0, sizeof(*io));
    io->fd = -1;
    io->rx_fd = -1;
    io->backend_ops = 0;
    io->backend_opaque = 0;
    io->stdout_only = MM_FALSE;
}

mm_bool mm_uart_io_open(struct mm_uart_io *io, mm_u32 base)
{
    const struct mm_uart_backend_binding *binding;
    if (io == 0) return MM_FALSE;
    binding = uart_find_backend(base);
    if (binding != 0) {
        io->fd = -2;
        io->backend_ops = binding->ops;
        io->backend_opaque = binding->opaque;
        io->stdout_only = MM_FALSE;
        snprintf(io->name, sizeof(io->name), "%s", binding->name);
        printf("[UART] %08lx attached to %s\n", (unsigned long)base, io->name);
        return MM_TRUE;
    }
    if (g_uart_stdout && !mm_tui_is_active()) {
        int rx;
        struct termios tio;
        io->fd = STDOUT_FILENO;
        io->stdout_only = MM_TRUE;
        g_uart_stdout = MM_FALSE; /* only first UART gets stdio */
        /* Open /dev/tty for RX — works even when stdin is closed */
        rx = open("/dev/tty", O_RDONLY | O_NOCTTY | O_NONBLOCK);
        if (rx < 0) {
            /* fallback to stdin */
            rx = fcntl(STDIN_FILENO, F_GETFL, 0) >= 0 ? STDIN_FILENO : -1;
        }
        if (rx >= 0 && tcgetattr(rx, &tio) == 0) {
            tio.c_lflag &= ~(ICANON | ECHO);
            tio.c_cc[VMIN] = 0;
            tio.c_cc[VTIME] = 0;
            tcsetattr(rx, TCSANOW, &tio);
        }
        io->rx_fd = rx;
        snprintf(io->name, sizeof(io->name), "stdio");
        printf("[UART] %08lx attached to %s\n", (unsigned long)base, io->name);
        return MM_TRUE;
    }
    io->fd = uart_open_pty(io->name, sizeof(io->name));
    if (io->fd >= 0) {
        printf("[UART] %08lx attached to %s\n", (unsigned long)base, io->name);
        return MM_TRUE;
    }
    return MM_FALSE;
}

void mm_uart_io_close(struct mm_uart_io *io)
{
    if (io == 0) return;
    if (io->backend_ops != 0 && io->backend_ops->close != 0) {
        io->backend_ops->close(io->backend_opaque);
    }
    if (io->fd >= 0 && !io->stdout_only) {
        close(io->fd);
        io->fd = -1;
    }
    if (io->rx_fd >= 0 && io->rx_fd != STDIN_FILENO) {
        close(io->rx_fd);
    }
    if (io->stdout_only) {
        /* Return the stdio claim: firmware toggling the UART off and on
         * again (e.g. across a secure/non-secure handover) must keep its
         * console on stdout instead of being demoted to a fresh PTY. */
        g_uart_stdout = MM_TRUE;
    }
    io->rx_fd = -1;
    io->rx_pending = MM_FALSE;
    io->tx_head = io->tx_tail = 0;
    io->backend_ops = 0;
    io->backend_opaque = 0;
    io->stdout_only = MM_FALSE;
}

void mm_uart_io_queue_tx(struct mm_uart_io *io, mm_u8 byte)
{
    size_t next_tail;
    if (io == 0) return;
    next_tail = (io->tx_tail + 1u) % sizeof(io->tx_buf);
    if (next_tail == io->tx_head) {
        io->tx_head = (io->tx_head + 1u) % sizeof(io->tx_buf);
    }
    io->tx_buf[io->tx_tail] = byte;
    io->tx_tail = next_tail;
}

mm_bool mm_uart_io_flush(struct mm_uart_io *io)
{
    if (io == 0) return MM_FALSE;
    if (io->backend_ops != 0) {
        while (io->tx_head != io->tx_tail) {
            size_t first_chunk;
            size_t consumed;
            if (io->backend_ops->write_tx == 0) {
                io->tx_head = io->tx_tail = 0;
                return MM_FALSE;
            }
            first_chunk = (io->tx_tail > io->tx_head)
                ? (io->tx_tail - io->tx_head)
                : (sizeof(io->tx_buf) - io->tx_head);
            consumed = io->backend_ops->write_tx(io->backend_opaque,
                                                 &io->tx_buf[io->tx_head],
                                                 first_chunk);
            if (consumed == 0) {
                return MM_FALSE;
            }
            if (consumed > first_chunk) {
                consumed = first_chunk;
            }
            io->tx_head = (io->tx_head + consumed) % sizeof(io->tx_buf);
        }
        return MM_TRUE;
    }
    if (io->fd < 0) return MM_FALSE;
    while (io->tx_head != io->tx_tail) {
        size_t first_chunk;
        size_t to_write;
        ssize_t n;
        first_chunk = (io->tx_tail > io->tx_head)
            ? (io->tx_tail - io->tx_head)
            : (sizeof(io->tx_buf) - io->tx_head);
        to_write = first_chunk;
        n = write(io->fd, &io->tx_buf[io->tx_head], to_write);
        if (n > 0) {
            io->tx_head = (io->tx_head + (size_t)n) % sizeof(io->tx_buf);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return MM_FALSE;
        } else {
            io->tx_head = io->tx_tail = 0;
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

mm_bool mm_uart_io_poll(struct mm_uart_io *io)
{
    mm_bool new_rx = MM_FALSE;
    if (io == 0) return MM_FALSE;
    (void)mm_uart_io_flush(io);
    if (!io->rx_pending) {
        mm_u8 b;
        if (io->backend_ops != 0) {
            if (io->backend_ops->read_rx == 0 ||
                !io->backend_ops->read_rx(io->backend_opaque, &b)) {
                return MM_FALSE;
            }
            io->rx_byte = b;
            io->rx_pending = MM_TRUE;
            new_rx = MM_TRUE;
            if (uart_rx_trace_enabled()) {
                printf("[UART_RX_POLL] fd=%d byte=0x%02x\n", io->fd, (unsigned)b);
            }
        } else {
            int rx_fd = io->stdout_only ? io->rx_fd : io->fd;
            ssize_t n;
            if (rx_fd < 0) return MM_FALSE;
            n = read(rx_fd, &b, 1);
            if (n == 1) {
                io->rx_byte = b;
                io->rx_pending = MM_TRUE;
                new_rx = MM_TRUE;
                if (uart_rx_trace_enabled()) {
                    printf("[UART_RX_POLL] fd=%d byte=0x%02x\n", io->fd, (unsigned)b);
                }
            }
        }
    }
    return new_rx;
}

mm_bool mm_uart_io_tx_empty(const struct mm_uart_io *io)
{
    if (io == 0) return MM_TRUE;
    return io->tx_head == io->tx_tail;
}

mm_bool mm_uart_io_has_rx(const struct mm_uart_io *io)
{
    if (io == 0) return MM_FALSE;
    return io->rx_pending;
}

mm_u8 mm_uart_io_peek(const struct mm_uart_io *io)
{
    if (io == 0) return 0;
    if (!io->rx_pending) return 0;
    return io->rx_byte;
}

mm_u8 mm_uart_io_read(struct mm_uart_io *io)
{
    mm_u8 v = 0;
    if (io == 0) return 0;
    if (io->rx_pending) {
        v = io->rx_byte;
        io->rx_pending = MM_FALSE;
        if (uart_rx_trace_enabled()) {
            printf("[UART_RX_READ] fd=%d byte=0x%02x\n", io->fd, (unsigned)v);
        }
    }
    return v;
}

void mm_uart_io_set_stdout(mm_bool enable)
{
    g_uart_stdout = enable ? MM_TRUE : MM_FALSE;
}

mm_bool mm_uart_backend_attach(mm_u32 base, const char *name,
                               const struct mm_uart_backend_ops *ops,
                               void *opaque)
{
    size_t i;
    if (ops == 0 || ops->write_tx == 0 || ops->read_rx == 0) {
        return MM_FALSE;
    }
    for (i = 0; i < MM_UART_BACKEND_MAX; ++i) {
        if (g_uart_backends[i].used && g_uart_backends[i].base == base) {
            return MM_FALSE;
        }
    }
    for (i = 0; i < MM_UART_BACKEND_MAX; ++i) {
        if (!g_uart_backends[i].used) {
            memset(&g_uart_backends[i], 0, sizeof(g_uart_backends[i]));
            g_uart_backends[i].used = MM_TRUE;
            g_uart_backends[i].base = base;
            g_uart_backends[i].ops = ops;
            g_uart_backends[i].opaque = opaque;
            if (name != 0 && name[0] != '\0') {
                snprintf(g_uart_backends[i].name, sizeof(g_uart_backends[i].name),
                         "%s", name);
            } else {
                snprintf(g_uart_backends[i].name, sizeof(g_uart_backends[i].name),
                         "uart-backend");
            }
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

void mm_uart_backend_detach(mm_u32 base)
{
    size_t i;
    for (i = 0; i < MM_UART_BACKEND_MAX; ++i) {
        if (g_uart_backends[i].used && g_uart_backends[i].base == base) {
            memset(&g_uart_backends[i], 0, sizeof(g_uart_backends[i]));
            return;
        }
    }
}

static mm_bool g_uart_break_on_macro = MM_FALSE;

void mm_uart_break_on_macro_set(void)
{
    g_uart_break_on_macro = MM_TRUE;
}

mm_bool mm_uart_break_on_macro_take(void)
{
    mm_bool v = g_uart_break_on_macro;
    g_uart_break_on_macro = MM_FALSE;
    return v;
}

void mm_target_soc_reset(const struct mm_target_cfg *cfg)
{
    if (cfg == 0 || cfg->soc_reset == 0) {
        return;
    }
    cfg->soc_reset();
}

mm_bool mm_target_register_mmio(const struct mm_target_cfg *cfg, struct mmio_bus *bus)
{
    g_target_cfg = cfg;
    if (cfg == 0 || cfg->soc_register_mmio == 0) {
        return MM_TRUE;
    }
    return cfg->soc_register_mmio(bus);
}

void mm_target_flash_bind(const struct mm_target_cfg *cfg,
                          struct mm_memmap *map,
                          mm_u8 *flash,
                          mm_u32 flash_size,
                          const struct mm_flash_persist *persist)
{
    g_target_cfg = cfg;
    if (cfg == 0 || cfg->flash_bind == 0) {
        return;
    }
    cfg->flash_bind(map, flash, flash_size, persist, cfg->flags);
}

const struct mm_target_cfg *mm_target_current_cfg(void)
{
    return g_target_cfg;
}

mm_u64 mm_target_cpu_hz(const struct mm_target_cfg *cfg)
{
    if (cfg == 0 || cfg->clock_get_hz == 0) {
        return 0;
    }
    return cfg->clock_get_hz();
}

void mm_target_usart_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic)
{
    MM_TARGET_CALL2(usart_init, bus, nvic);
}

void mm_target_usart_reset(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(usart_reset);
}

void mm_target_usart_poll(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(usart_poll);
}

void mm_target_spi_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic)
{
    MM_TARGET_CALL2(spi_init, bus, nvic);
}

void mm_target_spi_reset(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(spi_reset);
}

void mm_target_spi_poll(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(spi_poll);
}

void mm_target_eth_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic)
{
    MM_TARGET_CALL2(eth_init, bus, nvic);
}

void mm_target_eth_reset(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(eth_reset);
}

void mm_target_eth_poll(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(eth_poll);
}
