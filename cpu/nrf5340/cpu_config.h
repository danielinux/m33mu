#ifndef M33MU_CPU_NRF5340_CONFIG_H
#define M33MU_CPU_NRF5340_CONFIG_H

/* Nordic nRF5340 (application core) memory map (Secure / Non-secure aliases) */
#define NRF5340_FLASH_BASE_S   0x10000000u
#define NRF5340_FLASH_BASE_NS  0x00000000u
#define NRF5340_FLASH_SIZE     0x00100000u  /* 1 MB */

#define NRF5340_RAM_BASE_S     0x30000000u
#define NRF5340_RAM_BASE_NS    0x20000000u
#define NRF5340_RAM_SIZE       0x00080000u  /* 512 KB */

#define NRF5340_RAM_REGIONS 0
#define NRF5340_RAM_REGION_COUNT 0u
#define NRF5340_MPCBB_BLOCK_SIZE 0u

#define NRF5340_SOC_RESET      mm_nrf5340_mmio_reset
#define NRF5340_SOC_REGISTER   mm_nrf5340_register_mmio
#define NRF5340_FLASH_BIND     mm_nrf5340_flash_bind
#define NRF5340_CLOCK_GET_HZ   mm_nrf5340_cpu_hz
#define NRF5340_USART_INIT     mm_nrf5340_usart_init
#define NRF5340_USART_RESET    mm_nrf5340_usart_reset
#define NRF5340_USART_POLL     mm_nrf5340_usart_poll
#define NRF5340_SPI_INIT       mm_nrf5340_spi_init
#define NRF5340_SPI_RESET      mm_nrf5340_spi_reset
#define NRF5340_SPI_POLL       mm_nrf5340_spi_poll
#define NRF5340_TIMER_INIT     mm_nrf5340_timers_init
#define NRF5340_TIMER_RESET    mm_nrf5340_timers_reset
#define NRF5340_TIMER_TICK     mm_nrf5340_timers_tick

#define NRF5340_FLAGS 0u

#endif /* M33MU_CPU_NRF5340_CONFIG_H */
