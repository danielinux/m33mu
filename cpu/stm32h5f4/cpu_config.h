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

#ifndef M33MU_CPU_STM32H5F4_CONFIG_H
#define M33MU_CPU_STM32H5F4_CONFIG_H

/* STM32H5F4 memory map (Secure / Non-secure aliases) */
#include "m33mu/target.h"

#define STM32H5F4_FLASH_BASE_S   0x0C000000u
#define STM32H5F4_FLASH_BASE_NS  0x08000000u
#define STM32H5F4_FLASH_SIZE     0x00400000u  /* 4 MB */

#define STM32H5F4_RAM_BASE_S     0x30000000u
#define STM32H5F4_RAM_BASE_NS    0x20000000u
#define STM32H5F4_RAM_SIZE       0x00180000u  /* 1.5 MB (SRAM1..5, contiguous) */

#define STM32H5F4_PERIPH_BASE_S  0x50000000u
#define STM32H5F4_PERIPH_BASE_NS 0x40000000u

static const struct mm_ram_region STM32H5F4_RAM_REGIONS[] = {
    { 0x0A000000u, 0x0A000000u, 0x00180000u, -1 }, /* SRAM1..5 code-region alias */
    { 0x30000000u, 0x20000000u, 0x00040000u, 0 }, /* SRAM1 256 KB */
    { 0x30040000u, 0x20040000u, 0x00020000u, 1 }, /* SRAM2 128 KB */
    { 0x30060000u, 0x20060000u, 0x00060000u, 2 }, /* SRAM3 384 KB */
    { 0x300C0000u, 0x200C0000u, 0x00060000u, 3 }, /* SRAM4 384 KB */
    { 0x30120000u, 0x20120000u, 0x00060000u, 4 }, /* SRAM5 384 KB */
    { 0x50036400u, 0x40036400u, 0x00001000u, -1 } /* BKPSRAM 4 KB */
};

#define STM32H5F4_RAM_REGION_COUNT (sizeof(STM32H5F4_RAM_REGIONS) / sizeof(STM32H5F4_RAM_REGIONS[0]))
#define STM32H5F4_MPCBB_BLOCK_SIZE 512u

#define STM32H5F4_SOC_RESET      mm_stm32h5f4_mmio_reset
#define STM32H5F4_SOC_REGISTER   mm_stm32h5f4_register_mmio
#define STM32H5F4_FLASH_BIND     mm_stm32h5f4_flash_bind
#define STM32H5F4_CLOCK_GET_HZ   mm_stm32h5f4_cpu_hz
#define STM32H5F4_USART_INIT     mm_stm32h5f4_usart_init
#define STM32H5F4_USART_RESET    mm_stm32h5f4_usart_reset
#define STM32H5F4_USART_POLL     mm_stm32h5f4_usart_poll

#define STM32H5F4_SPI_INIT       mm_stm32h5f4_spi_init
#define STM32H5F4_SPI_RESET      mm_stm32h5f4_spi_reset
#define STM32H5F4_SPI_POLL       mm_stm32h5f4_spi_poll

#define STM32H5F4_ETH_INIT       stm32h5_eth_init
#define STM32H5F4_ETH_RESET      stm32h5_eth_reset
#define STM32H5F4_ETH_POLL       stm32h5_eth_poll

#define STM32H5F4_TIMER_INIT  mm_stm32h5f4_timers_init
#define STM32H5F4_TIMER_RESET mm_stm32h5f4_timers_reset
#define STM32H5F4_TIMER_TICK  mm_stm32h5f4_timers_tick
#define STM32H5F4_TZ_ATTR     mm_stm32h5f4_tz_attr_for_addr

#define STM32H5F4_FLAGS (MM_TARGET_FLAG_NVM_WRITEONCE | MM_TARGET_FLAG_FPU)

#endif /* M33MU_CPU_STM32H5F4_CONFIG_H */
