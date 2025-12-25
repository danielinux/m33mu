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

#ifndef M33MU_CPU_STM32L552_CONFIG_H
#define M33MU_CPU_STM32L552_CONFIG_H

/* STM32L552 memory map (Secure / Non-secure aliases) */
#include "m33mu/target.h"

#define STM32L552_FLASH_BASE_S   0x0C000000u
#define STM32L552_FLASH_BASE_NS  0x08000000u
#define STM32L552_FLASH_SIZE     0x00080000u  /* 512 KB */

#define STM32L552_RAM_BASE_S     0x30000000u
#define STM32L552_RAM_BASE_NS    0x20000000u
#define STM32L552_RAM_SIZE       0x00040000u  /* 256 KB */

#define STM32L552_PERIPH_BASE_S  0x50000000u
#define STM32L552_PERIPH_BASE_NS 0x40000000u

#define STM32L552_RAM_REGIONS 0
#define STM32L552_RAM_REGION_COUNT 0u
#define STM32L552_MPCBB_BLOCK_SIZE 0u

#define STM32L552_SOC_RESET      mm_stm32l552_mmio_reset
#define STM32L552_SOC_REGISTER   mm_stm32l552_register_mmio
#define STM32L552_FLASH_BIND     mm_stm32l552_flash_bind
#define STM32L552_CLOCK_GET_HZ   mm_stm32l552_cpu_hz
#define STM32L552_USART_INIT     mm_stm32l552_usart_init
#define STM32L552_USART_RESET    mm_stm32l552_usart_reset
#define STM32L552_USART_POLL     mm_stm32l552_usart_poll

#define STM32L552_SPI_INIT       mm_stm32l552_spi_init
#define STM32L552_SPI_RESET      mm_stm32l552_spi_reset
#define STM32L552_SPI_POLL       mm_stm32l552_spi_poll

#define STM32L552_TIMER_INIT  mm_stm32l552_timers_init
#define STM32L552_TIMER_RESET mm_stm32l552_timers_reset
#define STM32L552_TIMER_TICK  mm_stm32l552_timers_tick

#define STM32L552_FLAGS MM_TARGET_FLAG_NVM_WRITEONCE

#endif /* M33MU_CPU_STM32L552_CONFIG_H */
