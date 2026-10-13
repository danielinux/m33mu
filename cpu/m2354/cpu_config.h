/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_CPU_M2354_CONFIG_H
#define M33MU_CPU_M2354_CONFIG_H

#include "m33mu/target.h"

/*
 * Nuvoton NuMicro M2354 (Cortex-M23, ARMv8-M Baseline).
 *
 * The alias direction is the opposite of the NXP parts: on M2354 the base
 * address is the Secure view and base + 0x10000000 is the Non-secure alias.
 * That applies uniformly to flash, SRAM and the peripheral window.
 *
 * m33mu models ARMv8-M Mainline only.  Baseline is an instruction subset, so
 * the core runs M23 code unchanged; the only concession here is that the FPU
 * flag is clear (M23 has no floating-point unit).
 */
#define M2354_NS_OFFSET         0x10000000u

/* APROM: 1 MB, two 512 KB banks, 2 KB erase pages */
#define M2354_FLASH_BASE_S      0x00000000u
#define M2354_FLASH_BASE_NS     (M2354_FLASH_BASE_S + M2354_NS_OFFSET)
#define M2354_FLASH_SIZE        0x00100000u
#define M2354_FLASH_PAGE_SIZE   0x800u

/* SRAM: 256 KB across banks 0/1/2 */
#define M2354_RAM_BASE_S        0x20000000u
#define M2354_RAM_BASE_NS       (M2354_RAM_BASE_S + M2354_NS_OFFSET)
#define M2354_RAM_SIZE          0x00040000u

/* Peripheral window */
#define M2354_PERIPH_BASE_S     0x40000000u
#define M2354_PERIPH_BASE_NS    (M2354_PERIPH_BASE_S + M2354_NS_OFFSET)
#define M2354_PERIPH_SIZE       0x10000000u

static const struct mm_ram_region M2354_RAM_REGIONS[] = {
    { M2354_RAM_BASE_S, M2354_RAM_BASE_NS, M2354_RAM_SIZE, -1, 0u }
};

#define M2354_RAM_REGION_COUNT \
    (sizeof(M2354_RAM_REGIONS) / sizeof(M2354_RAM_REGIONS[0]))

/* SRAM security attribution is SCU_SRAMNSSET, modelled in m2354_mmio.c */
#define M2354_MPCBB_BLOCK_SIZE 0u

#define M2354_SOC_RESET        mm_m2354_mmio_reset
#define M2354_SOC_REGISTER     mm_m2354_register_mmio
#define M2354_FLASH_BIND       mm_m2354_flash_bind
#define M2354_CLOCK_GET_HZ     mm_m2354_cpu_hz
#define M2354_USART_INIT       mm_m2354_uart_init
#define M2354_USART_RESET      mm_m2354_uart_reset
#define M2354_USART_POLL       mm_m2354_uart_poll
#define M2354_TIMER_INIT       mm_m2354_timers_init
#define M2354_TIMER_RESET      mm_m2354_timers_reset
#define M2354_TIMER_TICK       mm_m2354_timers_tick
#define M2354_TZ_ATTR          mm_m2354_tz_attr_for_addr

/* Cortex-M23: no FPU, no dual-bank swap controller, no CASPER coprocessor. */
#define M2354_FLAGS (0u)

#endif /* M33MU_CPU_M2354_CONFIG_H */
