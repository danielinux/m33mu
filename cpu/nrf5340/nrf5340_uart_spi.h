#ifndef M33MU_NRF5340_UART_SPI_H
#define M33MU_NRF5340_UART_SPI_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

void mm_nrf5340_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_nrf5340_usart_reset(void);
void mm_nrf5340_usart_poll(void);

void mm_nrf5340_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_nrf5340_spi_reset(void);
void mm_nrf5340_spi_poll(void);

#endif /* M33MU_NRF5340_UART_SPI_H */
