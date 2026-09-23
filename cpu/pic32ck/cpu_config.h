/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_CPU_PIC32CK_CONFIG_H
#define M33MU_CPU_PIC32CK_CONFIG_H

#include "m33mu/target.h"

/* PIC32CK2051SG memory map */
#define PIC32CK_FLASH_BASE_S    0x0C000000u
#define PIC32CK_FLASH_BASE_NS   0x0C000000u
#define PIC32CK_FLASH_SIZE      0x00200000u  /* 2 MB */
#define PIC32CK_BOOT_BASE       0x08000000u
#define PIC32CK_BOOT_SIZE       0x00020000u  /* 128 KB */
#define PIC32CK_CFM_BASE        0x0A000000u
#define PIC32CK_CFM_SIZE        0x00010000u  /* 64 KB */
#define PIC32CK_RAM_BASE_S      0x20000000u
#define PIC32CK_RAM_BASE_NS     0x20000000u
#define PIC32CK_RAM_SIZE        0x00080000u  /* 512 KB */

static const struct mm_ram_region PIC32CK_RAM_REGIONS[] = {
    { PIC32CK_RAM_BASE_S, PIC32CK_RAM_BASE_NS, PIC32CK_RAM_SIZE, -1, 0u }
};

#define PIC32CK_RAM_REGION_COUNT \
    (sizeof(PIC32CK_RAM_REGIONS) / sizeof(PIC32CK_RAM_REGIONS[0]))

#define PIC32CK_MPCBB_BLOCK_SIZE 0u

#define PIC32CK_SOC_RESET      mm_pic32ck_mmio_reset
#define PIC32CK_SOC_REGISTER   mm_pic32ck_register_mmio
#define PIC32CK_FLASH_BIND     mm_pic32ck_flash_bind
#define PIC32CK_CLOCK_GET_HZ   mm_pic32ck_cpu_hz
#define PIC32CK_USART_INIT     mm_pic32ck_sercom_init
#define PIC32CK_USART_RESET    mm_pic32ck_sercom_reset
#define PIC32CK_USART_POLL     mm_pic32ck_sercom_poll
#define PIC32CK_SPI_INIT       mm_pic32ck_sercom_init
#define PIC32CK_SPI_RESET      mm_pic32ck_sercom_reset
#define PIC32CK_SPI_POLL       mm_pic32ck_sercom_poll
#define PIC32CK_TIMER_INIT     mm_pic32ck_timers_init
#define PIC32CK_TIMER_RESET    mm_pic32ck_timers_reset
#define PIC32CK_TIMER_TICK     mm_pic32ck_timers_tick
#define PIC32CK_TZ_ATTR        mm_pic32ck_tz_attr_for_addr

#define PIC32CK_FLAGS (MM_TARGET_FLAG_FPU)

#endif /* M33MU_CPU_PIC32CK_CONFIG_H */
