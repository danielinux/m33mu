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

#ifndef M33MU_CPU_STM32U585_CONFIG_H
#define M33MU_CPU_STM32U585_CONFIG_H

/* STM32U585 memory map (Secure / Non-secure aliases) */
#include "m33mu/target.h"

#define STM32U585_FLASH_BASE_S   0x0C000000u
#define STM32U585_FLASH_BASE_NS  0x08000000u
#define STM32U585_FLASH_SIZE     0x00200000u  /* 2 MB */

#define STM32U585_RAM_BASE_S     0x30000000u
#define STM32U585_RAM_BASE_NS    0x20000000u
#define STM32U585_RAM_SIZE       0x000C0000u  /* 768 KB */

#define STM32U585_PERIPH_BASE_S  0x50000000u
#define STM32U585_PERIPH_BASE_NS 0x40000000u

static const struct mm_ram_region STM32U585_RAM_REGIONS[] = {
    { 0x30000000u, 0x20000000u, 0x00030000u, 0 }, /* SRAM1 192 KB */
    { 0x30030000u, 0x20030000u, 0x00010000u, 1 }, /* SRAM2 64 KB */
    { 0x30040000u, 0x20040000u, 0x00080000u, 2 }, /* SRAM3 512 KB */
    { 0x38000000u, 0x28000000u, 0x00004000u, 3 }  /* SRAM4 16 KB */
};

#define STM32U585_RAM_REGION_COUNT (sizeof(STM32U585_RAM_REGIONS) / sizeof(STM32U585_RAM_REGIONS[0]))
#define STM32U585_MPCBB_BLOCK_SIZE 512u

#define STM32U585_SOC_RESET      mm_stm32u585_mmio_reset
#define STM32U585_SOC_REGISTER   mm_stm32u585_register_mmio
#define STM32U585_FLASH_BIND     mm_stm32u585_flash_bind
#define STM32U585_CLOCK_GET_HZ   mm_stm32u585_cpu_hz
#define STM32U585_USART_INIT     mm_stm32u585_usart_init
#define STM32U585_USART_RESET    mm_stm32u585_usart_reset
#define STM32U585_USART_POLL     mm_stm32u585_usart_poll

#define STM32U585_SPI_INIT       mm_stm32u585_spi_init
#define STM32U585_SPI_RESET      mm_stm32u585_spi_reset
#define STM32U585_SPI_POLL       mm_stm32u585_spi_poll

#define STM32U585_TIMER_INIT  mm_stm32u585_timers_init
#define STM32U585_TIMER_RESET mm_stm32u585_timers_reset
#define STM32U585_TIMER_TICK  mm_stm32u585_timers_tick

#define STM32U585_FLAGS MM_TARGET_FLAG_NVM_WRITEONCE

#endif /* M33MU_CPU_STM32U585_CONFIG_H */
