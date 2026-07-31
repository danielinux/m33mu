/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "stm32h5f4/stm32h5f4_spi.h"
#include "stm32_spi.h"

static struct stm32_spi_state g_spi;

static const mm_u32 g_bases[] = {
    0x40013000u, 0x40003800u, 0x40003C00u, 0x40014C00u, 0x44002000u, 0x40015000u
};

static const mm_u32 g_bases_sec[] = {
    0x50013000u, 0x50003800u, 0x50003C00u, 0x50014C00u, 0x54002000u, 0x50015000u
};

static const int g_irq_map[] = { 55, 56, 57, 82, 83, 84 };
static const char *const g_names[] = { "SPI1", "SPI2", "SPI3", "SPI4", "SPI5", "SPI6" };

static const struct stm32_spi_config g_cfg = {
    g_bases,
    g_bases_sec,
    g_irq_map,
    g_names,
    sizeof(g_bases) / sizeof(g_bases[0]),
    MM_TRUE,
    MM_FALSE
};

void mm_stm32h5f4_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    stm32_spi_init(&g_spi, &g_cfg, bus, nvic);
}

void mm_stm32h5f4_spi_poll(void)
{
    stm32_spi_poll(&g_spi);
}

void mm_stm32h5f4_spi_reset(void)
{
    stm32_spi_reset(&g_spi);
}
