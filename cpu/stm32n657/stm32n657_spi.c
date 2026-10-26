/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "stm32n657/stm32n657_spi.h"
#include "stm32_spi.h"

/* SPI1..SPI6: the STM32H7-style SPI of the H5 (Zephyr dts/arm/st/n6/
 * stm32n6.dtsi lists the secure aliases; non-secure is 0x10000000 lower). */
static struct stm32_spi_state g_spi;

static const mm_u32 g_bases[] = {
    0x42003000u, 0x40003800u, 0x40003C00u, 0x42003400u, 0x42005000u, 0x46001400u
};

static const mm_u32 g_bases_sec[] = {
    0x52003000u, 0x50003800u, 0x50003C00u, 0x52003400u, 0x52005000u, 0x56001400u
};

static const int g_irq_map[] = { 153, 154, 155, 156, 157, 158 };
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

void mm_stm32n657_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    stm32_spi_init(&g_spi, &g_cfg, bus, nvic);
}

void mm_stm32n657_spi_poll(void)
{
    stm32_spi_poll(&g_spi);
}

void mm_stm32n657_spi_reset(void)
{
    stm32_spi_reset(&g_spi);
}
