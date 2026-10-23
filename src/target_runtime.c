/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/target_hal.h"

static const struct mm_target_cfg *g_target_cfg;

void mm_target_soc_reset(const struct mm_target_cfg *cfg)
{
    if (cfg != 0 && cfg->soc_reset != 0) cfg->soc_reset();
}

mm_bool mm_target_register_mmio(const struct mm_target_cfg *cfg, struct mmio_bus *bus)
{
    g_target_cfg = cfg;
    if (cfg == 0 || cfg->soc_register_mmio == 0) return MM_TRUE;
    return cfg->soc_register_mmio(bus);
}

void mm_target_flash_bind(const struct mm_target_cfg *cfg,
                          struct mm_memmap *map,
                          mm_u8 *flash,
                          mm_u32 flash_size,
                          const struct mm_flash_persist *persist)
{
    g_target_cfg = cfg;
    if (cfg != 0 && cfg->flash_bind != 0) {
        cfg->flash_bind(map, flash, flash_size, persist, cfg->flags);
    }
}

const struct mm_target_cfg *mm_target_current_cfg(void)
{
    return g_target_cfg;
}

mm_u64 mm_target_cpu_hz(const struct mm_target_cfg *cfg)
{
    if (cfg == 0 || cfg->clock_get_hz == 0) return 0;
    return cfg->clock_get_hz();
}

#define MM_TARGET_CALL0(field) \
    do { \
        if (cfg != 0 && cfg->field != 0) cfg->field(); \
    } while (0)

#define MM_TARGET_CALL2(field, arg0, arg1) \
    do { \
        if (cfg != 0 && cfg->field != 0) cfg->field((arg0), (arg1)); \
    } while (0)

void mm_target_usart_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic)
{
    MM_TARGET_CALL2(usart_init, bus, nvic);
}

void mm_target_usart_reset(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(usart_reset);
}

void mm_target_usart_poll(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(usart_poll);
}

void mm_target_spi_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic)
{
    MM_TARGET_CALL2(spi_init, bus, nvic);
}

void mm_target_spi_reset(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(spi_reset);
}

void mm_target_spi_poll(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(spi_poll);
}

void mm_target_eth_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic)
{
    MM_TARGET_CALL2(eth_init, bus, nvic);
}

void mm_target_eth_reset(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(eth_reset);
}

void mm_target_eth_poll(const struct mm_target_cfg *cfg)
{
    MM_TARGET_CALL0(eth_poll);
}
