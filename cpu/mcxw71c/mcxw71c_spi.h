#ifndef M33MU_MCXW71C_SPI_H
#define M33MU_MCXW71C_SPI_H

struct mmio_bus;
struct mm_nvic;

void mm_mcxw71c_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_mcxw71c_spi_poll(void);
void mm_mcxw71c_spi_reset(void);

#endif /* M33MU_MCXW71C_SPI_H */
