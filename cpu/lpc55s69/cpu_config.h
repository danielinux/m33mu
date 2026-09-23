/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_CPU_LPC55S69_CONFIG_H
#define M33MU_CPU_LPC55S69_CONFIG_H

#include "m33mu/target.h"

/* LPC55S69 memory map (Secure / Non-secure aliases) */
#define LPC55S69_FLASH_BASE_S   0x10000000u
#define LPC55S69_FLASH_BASE_S2  0x0C000000u  /* alternate secure alias */
#define LPC55S69_FLASH_BASE_NS  0x00000000u
#define LPC55S69_FLASH_SIZE     0x000A0000u  /* 640 KB */

#define LPC55S69_RAM_BASE_S     0x30000000u
#define LPC55S69_RAM_BASE_NS    0x20000000u
#define LPC55S69_RAM_SIZE       0x00044000u  /* 272 KB: SRAM0-3 (4x64KB) + SRAM4 (16KB) */

#define LPC55S69_PERIPH_BASE_S  0x50000000u
#define LPC55S69_PERIPH_BASE_NS 0x40000000u

static const struct mm_ram_region LPC55S69_RAM_REGIONS[] = {
    { LPC55S69_RAM_BASE_S, LPC55S69_RAM_BASE_NS, LPC55S69_RAM_SIZE, 0, 0u }
};

#define LPC55S69_RAM_REGION_COUNT \
    (sizeof(LPC55S69_RAM_REGIONS) / sizeof(LPC55S69_RAM_REGIONS[0]))
#define LPC55S69_MPCBB_BLOCK_SIZE 4096u  /* 4 KB per AHB_SECURE_CTRL block */

#define LPC55S69_SOC_RESET      mm_lpc55s69_mmio_reset
#define LPC55S69_SOC_REGISTER   mm_lpc55s69_register_mmio
#define LPC55S69_FLASH_BIND     mm_lpc55s69_flash_bind
#define LPC55S69_CLOCK_GET_HZ   mm_lpc55s69_cpu_hz
#define LPC55S69_MPCBB_SECURE   mm_lpc55s69_mpcbb_block_secure
#define LPC55S69_USART_INIT     mm_lpc55s69_flexcomm_init
#define LPC55S69_USART_RESET    mm_lpc55s69_flexcomm_reset
#define LPC55S69_USART_POLL     mm_lpc55s69_flexcomm_poll
#define LPC55S69_SPI_INIT       mm_lpc55s69_flexcomm_init
#define LPC55S69_SPI_RESET      mm_lpc55s69_flexcomm_reset
#define LPC55S69_SPI_POLL       mm_lpc55s69_flexcomm_poll
#define LPC55S69_TIMER_INIT     mm_lpc55s69_timers_init
#define LPC55S69_TIMER_RESET    mm_lpc55s69_timers_reset
#define LPC55S69_TIMER_TICK     mm_lpc55s69_timers_tick

#define LPC55S69_FLAGS (MM_TARGET_FLAG_FPU | MM_TARGET_FLAG_CASPER_CP)

#endif /* M33MU_CPU_LPC55S69_CONFIG_H */
