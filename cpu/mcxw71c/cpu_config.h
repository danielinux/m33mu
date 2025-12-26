#ifndef M33MU_CPU_MCXW71C_CONFIG_H
#define M33MU_CPU_MCXW71C_CONFIG_H

/* NXP MCXW716C memory map (Secure / Non-secure aliases) */
#define MCXW71C_FLASH_BASE_S   0x10000000u
#define MCXW71C_FLASH_BASE_NS  0x00000000u
#define MCXW71C_FLASH_SIZE     0x00100000u  /* 1 MB */

#define MCXW71C_RAM_BASE_S     0x30000000u
#define MCXW71C_RAM_BASE_NS    0x20000000u
#define MCXW71C_RAM_SIZE       0x0001C000u  /* 112 KB (STCM) */

#define MCXW71C_PERIPH_BASE_S  0x50000000u
#define MCXW71C_PERIPH_BASE_NS 0x40000000u

#define MCXW71C_RAM_REGIONS 0
#define MCXW71C_RAM_REGION_COUNT 0u
#define MCXW71C_MPCBB_BLOCK_SIZE 0u

#define MCXW71C_SOC_RESET      mm_mcxw71c_mmio_reset
#define MCXW71C_SOC_REGISTER   mm_mcxw71c_register_mmio
#define MCXW71C_FLASH_BIND     mm_mcxw71c_flash_bind
#define MCXW71C_CLOCK_GET_HZ   mm_mcxw71c_cpu_hz
#define MCXW71C_USART_INIT     mm_mcxw71c_usart_init
#define MCXW71C_USART_RESET    mm_mcxw71c_usart_reset
#define MCXW71C_USART_POLL     mm_mcxw71c_usart_poll
#define MCXW71C_SPI_INIT       mm_mcxw71c_spi_init
#define MCXW71C_SPI_RESET      mm_mcxw71c_spi_reset
#define MCXW71C_SPI_POLL       mm_mcxw71c_spi_poll
#define MCXW71C_TIMER_INIT     mm_mcxw71c_timers_init
#define MCXW71C_TIMER_RESET    mm_mcxw71c_timers_reset
#define MCXW71C_TIMER_TICK     mm_mcxw71c_timers_tick

#define MCXW71C_FLAGS 0u

#endif /* M33MU_CPU_MCXW71C_CONFIG_H */
