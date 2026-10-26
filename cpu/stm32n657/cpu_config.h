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

#ifndef M33MU_CPU_STM32N657_CONFIG_H
#define M33MU_CPU_STM32N657_CONFIG_H

/* STM32N657 memory map (Secure / Non-secure aliases). Values are from
 * RM0486 Table 1 and the neural-bass-n6 linker script. The N6 has no
 * internal flash: application code is linked into FLEXRAM, so the "flash"
 * region here is the FLEXRAM code area. */
#include "m33mu/target.h"
#include "stm32n657/stm32n657_spi.h"

/* FLEXRAM 400 KB, the code area. Boot ROM hands the core an IVT at 0x34000000. */
#define STM32N657_FLASH_BASE_S   0x34000000u
#define STM32N657_FLASH_BASE_NS  0x24000000u
#define STM32N657_FLASH_SIZE     0x00064000u  /* 400 KB */

/* Primary data RAM: the AXISRAM span from the end of FLEXRAM up to VENCRAM.
 * The NPU pool (0x34270000) and weights (0x342E0000) live inside this span. */
#define STM32N657_RAM_BASE_S     0x34064000u
#define STM32N657_RAM_BASE_NS    0x24064000u
#define STM32N657_RAM_SIZE       0x0039C000u  /* 3.6 MB (0x34064000..0x34400000) */

/* Every entry here gets its own slice of the RAM backing store, so two
 * entries never alias each other however their addresses relate. The S and
 * NS views of a bank share one backing slice (the memmap resolves the view
 * from base_s / base_ns). */
static const struct mm_ram_region STM32N657_RAM_REGIONS[] = {
    /* AXISRAM1..8 + CACHEAXI + VENCRAM as one contiguous span. Non-cached
     * and DMA-visible; the NPU pool and weights sit in it. */
    {0x34064000u, 0x24064000u, 0x0039C000u, -1, 0u},
    {0x00000000u, 0x10000000u, 0x00010000u, -1, 0u},   /* ITCM 64 KB */
    {0x20000000u, 0x30000000u, 0x00020000u, -1, 0u},   /* DTCM 128 KB */
    {0x38000000u, 0x28000000u, 0x00004000u, -1, 0u},   /* AHBSRAM1 16 KB */
    {0x38004000u, 0x28004000u, 0x00004000u, -1, 0u},   /* AHBSRAM2 16 KB */
    {0x3C000000u, 0x2C000000u, 0x00002000u, -1, 0u}    /* BKP SRAM 8 KB */
};

#define STM32N657_RAM_REGION_COUNT \
    (sizeof(STM32N657_RAM_REGIONS) / sizeof(STM32N657_RAM_REGIONS[0]))

/* The N6 has no MPCBB; the whole RAM is Secure by default. */
#define STM32N657_MPCBB_BLOCK_SIZE 512u
mm_bool mm_stm32n657_mpcbb_block_secure(int bank, mm_u32 block_index);

#define STM32N657_SOC_RESET      mm_stm32n657_mmio_reset
#define STM32N657_SOC_REGISTER   mm_stm32n657_register_mmio
#define STM32N657_CLOCK_GET_HZ   mm_stm32n657_cpu_hz
#define STM32N657_USART_INIT     mm_stm32n657_usart_init
#define STM32N657_USART_RESET    mm_stm32n657_usart_reset
#define STM32N657_USART_POLL     mm_stm32n657_usart_poll
#define STM32N657_SPI_INIT       mm_stm32n657_spi_init
#define STM32N657_SPI_RESET      mm_stm32n657_spi_reset
#define STM32N657_SPI_POLL       mm_stm32n657_spi_poll

/* FPU + Helium (MVE). No write-once NVM: code lives in FLEXRAM (RAM). */
#define STM32N657_FLAGS (MM_TARGET_FLAG_FPU)

#endif /* M33MU_CPU_STM32N657_CONFIG_H */
