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

static mm_bool g_uart_stdout = MM_FALSE;

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

void mm_uart_io_init(struct mm_uart_io *io)
{
    if (io == 0) return;
    memset(io, 0, sizeof(*io));
    io->fd = -1;
    io->stdout_only = MM_FALSE;
}

mm_bool mm_uart_io_open(struct mm_uart_io *io, mm_u32 base)
{
    if (io == 0) return MM_FALSE;
    if (g_uart_stdout) {
        io->fd = STDOUT_FILENO;
        io->stdout_only = MM_TRUE;
        snprintf(io->name, sizeof(io->name), "stdout");
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
    if (io->fd >= 0 && !io->stdout_only) {
        close(io->fd);
        io->fd = -1;
    }
    io->rx_pending = MM_FALSE;
    io->tx_head = io->tx_tail = 0;
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
    if (io == 0 || io->fd < 0) return MM_FALSE;
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
    if (io == 0 || io->fd < 0) return MM_FALSE;
    if (io->stdout_only) return MM_FALSE;
    (void)mm_uart_io_flush(io);
    if (!io->rx_pending) {
        mm_u8 b;
        ssize_t n = read(io->fd, &b, 1);
        if (n == 1) {
            io->rx_byte = b;
            io->rx_pending = MM_TRUE;
            new_rx = MM_TRUE;
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

mm_u8 mm_uart_io_read(struct mm_uart_io *io)
{
    mm_u8 v = 0;
    if (io == 0) return 0;
    if (io->stdout_only) return 0;
    if (io->rx_pending) {
        v = io->rx_byte;
        io->rx_pending = MM_FALSE;
    }
    return v;
}

void mm_uart_io_set_stdout(mm_bool enable)
{
    g_uart_stdout = enable ? MM_TRUE : MM_FALSE;
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
    if (cfg == 0 || cfg->flash_bind == 0) {
        return;
    }
    cfg->flash_bind(map, flash, flash_size, persist, cfg->flags);
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
    if (cfg == 0 || cfg->usart_init == 0) {
        return;
    }
    cfg->usart_init(bus, nvic);
}

void mm_target_usart_reset(const struct mm_target_cfg *cfg)
{
    if (cfg == 0 || cfg->usart_reset == 0) {
        return;
    }
    cfg->usart_reset();
}

void mm_target_usart_poll(const struct mm_target_cfg *cfg)
{
    if (cfg == 0 || cfg->usart_poll == 0) {
        return;
    }
    cfg->usart_poll();
}

void mm_target_spi_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic)
{
    if (cfg == 0 || cfg->spi_init == 0) {
        return;
    }
    cfg->spi_init(bus, nvic);
}

void mm_target_spi_reset(const struct mm_target_cfg *cfg)
{
    if (cfg == 0 || cfg->spi_reset == 0) {
        return;
    }
    cfg->spi_reset();
}

void mm_target_spi_poll(const struct mm_target_cfg *cfg)
{
    if (cfg == 0 || cfg->spi_poll == 0) {
        return;
    }
    cfg->spi_poll();
}
