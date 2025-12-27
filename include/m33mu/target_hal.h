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

#ifndef M33MU_TARGET_HAL_H
#define M33MU_TARGET_HAL_H

#include "m33mu/target.h"
#include "m33mu/types.h"

struct mm_uart_io {
    int fd;
    char name[64];
    mm_u8 tx_buf[1024];
    size_t tx_head;
    size_t tx_tail;
    mm_bool rx_pending;
    mm_u8 rx_byte;
    mm_bool stdout_only;
};

void mm_uart_io_init(struct mm_uart_io *io);
mm_bool mm_uart_io_open(struct mm_uart_io *io, mm_u32 base);
void mm_uart_io_close(struct mm_uart_io *io);
void mm_uart_io_queue_tx(struct mm_uart_io *io, mm_u8 byte);
mm_bool mm_uart_io_flush(struct mm_uart_io *io);
mm_bool mm_uart_io_poll(struct mm_uart_io *io);
mm_bool mm_uart_io_tx_empty(const struct mm_uart_io *io);
mm_bool mm_uart_io_has_rx(const struct mm_uart_io *io);
mm_u8 mm_uart_io_read(struct mm_uart_io *io);
void mm_uart_io_set_stdout(mm_bool enable);
void mm_uart_break_on_macro_set(void);
mm_bool mm_uart_break_on_macro_take(void);

void mm_target_soc_reset(const struct mm_target_cfg *cfg);
mm_bool mm_target_register_mmio(const struct mm_target_cfg *cfg, struct mmio_bus *bus);
void mm_target_flash_bind(const struct mm_target_cfg *cfg,
                          struct mm_memmap *map,
                          mm_u8 *flash,
                          mm_u32 flash_size,
                          const struct mm_flash_persist *persist);
mm_u64 mm_target_cpu_hz(const struct mm_target_cfg *cfg);
void mm_target_usart_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_target_usart_reset(const struct mm_target_cfg *cfg);
void mm_target_usart_poll(const struct mm_target_cfg *cfg);
void mm_target_spi_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_target_spi_reset(const struct mm_target_cfg *cfg);
void mm_target_spi_poll(const struct mm_target_cfg *cfg);

void mm_target_eth_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_target_eth_reset(const struct mm_target_cfg *cfg);
void mm_target_eth_poll(const struct mm_target_cfg *cfg);

mm_bool mm_tui_is_active(void);
void mm_tui_attach_uart(const char *label, const char *path);

#endif /* M33MU_TARGET_HAL_H */
