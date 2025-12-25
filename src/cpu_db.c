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

#include <string.h>
#include "m33mu/cpu_db.h"
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h563/stm32h563_timers.h"
#include "stm32h563/stm32h563_usart.h"
#include "stm32h563/stm32h563_spi.h"
#include "stm32h563/cpu_config.h"
#include "stm32u585/stm32u585_mmio.h"
#include "stm32u585/stm32u585_timers.h"
#include "stm32u585/stm32u585_usart.h"
#include "stm32u585/stm32u585_spi.h"
#include "stm32u585/cpu_config.h"

struct mm_cpu_entry {
    const char *name;
    struct mm_target_cfg cfg;
};

static const struct mm_cpu_entry cpu_table[] = {
    {
        "stm32h563",
        {
            STM32H563_FLASH_BASE_S,
            STM32H563_FLASH_SIZE,
            STM32H563_FLASH_BASE_NS,
            STM32H563_FLASH_SIZE,
            STM32H563_RAM_BASE_S,
            STM32H563_RAM_SIZE,
            STM32H563_RAM_BASE_NS,
            STM32H563_RAM_SIZE,
            STM32H563_RAM_REGIONS,
            STM32H563_RAM_REGION_COUNT,
            STM32H563_MPCBB_BLOCK_SIZE,
            mm_stm32h563_mpcbb_block_secure,
            STM32H563_FLAGS,
            STM32H563_SOC_RESET,
            STM32H563_SOC_REGISTER,
            STM32H563_FLASH_BIND,
            STM32H563_CLOCK_GET_HZ,
            STM32H563_USART_INIT,
            STM32H563_USART_RESET,
            STM32H563_USART_POLL,
            STM32H563_SPI_INIT,
            STM32H563_SPI_RESET,
            STM32H563_SPI_POLL,
            STM32H563_TIMER_INIT,
            STM32H563_TIMER_RESET,
            STM32H563_TIMER_TICK
        }
    },
    {
        "stm32u585",
        {
            STM32U585_FLASH_BASE_S,
            STM32U585_FLASH_SIZE,
            STM32U585_FLASH_BASE_NS,
            STM32U585_FLASH_SIZE,
            STM32U585_RAM_BASE_S,
            STM32U585_RAM_SIZE,
            STM32U585_RAM_BASE_NS,
            STM32U585_RAM_SIZE,
            STM32U585_RAM_REGIONS,
            STM32U585_RAM_REGION_COUNT,
            STM32U585_MPCBB_BLOCK_SIZE,
            mm_stm32u585_mpcbb_block_secure,
            STM32U585_FLAGS,
            STM32U585_SOC_RESET,
            STM32U585_SOC_REGISTER,
            STM32U585_FLASH_BIND,
            STM32U585_CLOCK_GET_HZ,
            STM32U585_USART_INIT,
            STM32U585_USART_RESET,
            STM32U585_USART_POLL,
            STM32U585_SPI_INIT,
            STM32U585_SPI_RESET,
            STM32U585_SPI_POLL,
            STM32U585_TIMER_INIT,
            STM32U585_TIMER_RESET,
            STM32U585_TIMER_TICK
        }
    }
};

const char *mm_cpu_default_name(void)
{
    return cpu_table[0].name;
}

mm_bool mm_cpu_lookup(const char *name, struct mm_target_cfg *cfg_out)
{
    size_t i;
    if (name == 0 || cfg_out == 0) {
        return MM_FALSE;
    }
    for (i = 0; i < sizeof(cpu_table) / sizeof(cpu_table[0]); ++i) {
        if (strcmp(name, cpu_table[i].name) == 0) {
            *cfg_out = cpu_table[i].cfg;
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}
