#ifndef M33MU_CPU_MCXN947_CONFIG_H
#define M33MU_CPU_MCXN947_CONFIG_H

/* NXP MCXN947 memory map (Secure / Non-secure aliases) */
#include "m33mu/target.h"
#define MCXN947_FLASH_BASE_S   0x10000000u
#define MCXN947_FLASH_BASE_NS  0x00000000u
#define MCXN947_FLASH_SIZE     0x00200000u  /* 2 MB */

#define MCXN947_RAM_BASE_S     0x30000000u
#define MCXN947_RAM_BASE_NS    0x20000000u
#define MCXN947_RAM_SIZE       0x00080000u  /* 512 KB */

#define MCXN947_PERIPH_BASE_S  0x50000000u
#define MCXN947_PERIPH_BASE_NS 0x40000000u

static const struct mm_ram_region MCXN947_RAM_REGIONS[] = {
    { MCXN947_RAM_BASE_S, MCXN947_RAM_BASE_NS, MCXN947_RAM_SIZE, 0, 0u }
};

#define MCXN947_RAM_REGION_COUNT (sizeof(MCXN947_RAM_REGIONS) / sizeof(MCXN947_RAM_REGIONS[0]))
#define MCXN947_MPCBB_BLOCK_SIZE 32768u

#define MCXN947_SOC_RESET      mm_mcxn947_mmio_reset
#define MCXN947_SOC_REGISTER   mm_mcxn947_register_mmio
#define MCXN947_FLASH_BIND     mm_mcxn947_flash_bind
#define MCXN947_CLOCK_GET_HZ   mm_mcxn947_cpu_hz
#define MCXN947_USART_INIT     mm_mcxn947_flexcomm_init
#define MCXN947_USART_RESET    mm_mcxn947_flexcomm_reset
#define MCXN947_USART_POLL     mm_mcxn947_flexcomm_poll
#define MCXN947_SPI_INIT       mm_mcxn947_flexcomm_init
#define MCXN947_SPI_RESET      mm_mcxn947_flexcomm_reset
#define MCXN947_SPI_POLL       mm_mcxn947_flexcomm_poll
#define MCXN947_TIMER_INIT     mm_mcxn947_timers_init
#define MCXN947_TIMER_RESET    mm_mcxn947_timers_reset
#define MCXN947_TIMER_TICK     mm_mcxn947_timers_tick

#define MCXN947_FLAGS (MM_TARGET_FLAG_FPU | MM_TARGET_FLAG_DUALBANK)

#endif /* M33MU_CPU_MCXN947_CONFIG_H */
