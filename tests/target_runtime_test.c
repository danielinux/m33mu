/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "m33mu/target_hal.h"

static mm_u32 calls[13];
static mm_u32 seen_flags;
static mm_u64 test_clock(void) { calls[0]++; return 64000000u; }
static void test_reset(void) { calls[1]++; }
static mm_bool test_register(struct mmio_bus *bus) { calls[2]++; return bus != 0; }
static void test_flash_bind(struct mm_memmap *map, mm_u8 *flash, mm_u32 size,
                            const struct mm_flash_persist *persist, mm_u32 flags)
{
    calls[3]++;
    seen_flags = flags;
    (void)map;
    (void)flash;
    (void)size;
    (void)persist;
}
static void test_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic) { calls[4]++; (void)bus; (void)nvic; }
static void test_usart_reset(void) { calls[5]++; }
static void test_usart_poll(void) { calls[6]++; }
static void test_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic) { calls[7]++; (void)bus; (void)nvic; }
static void test_spi_reset(void) { calls[8]++; }
static void test_spi_poll(void) { calls[9]++; }
static void test_eth_init(struct mmio_bus *bus, struct mm_nvic *nvic) { calls[10]++; (void)bus; (void)nvic; }
static void test_eth_reset(void) { calls[11]++; }
static void test_eth_poll(void) { calls[12]++; }

int main(void)
{
    struct mm_target_cfg cfg;
    struct mmio_bus bus;
    struct mm_nvic nvic;
    mm_u8 flash[4];
    int i;

    memset(&cfg, 0, sizeof(cfg));
    memset(&bus, 0, sizeof(bus));
    memset(&nvic, 0, sizeof(nvic));
    memset(calls, 0, sizeof(calls));
    cfg.flags = MM_TARGET_FLAG_FPU;
    cfg.soc_reset = test_reset;
    cfg.soc_register_mmio = test_register;
    cfg.flash_bind = test_flash_bind;
    cfg.clock_get_hz = test_clock;
    cfg.usart_init = test_usart_init;
    cfg.usart_reset = test_usart_reset;
    cfg.usart_poll = test_usart_poll;
    cfg.spi_init = test_spi_init;
    cfg.spi_reset = test_spi_reset;
    cfg.spi_poll = test_spi_poll;
    cfg.eth_init = test_eth_init;
    cfg.eth_reset = test_eth_reset;
    cfg.eth_poll = test_eth_poll;

    mm_target_soc_reset(&cfg);
    if (!mm_target_register_mmio(&cfg, &bus) || mm_target_current_cfg() != &cfg) return 1;
    mm_target_flash_bind(&cfg, 0, flash, sizeof(flash), 0);
    if (seen_flags != MM_TARGET_FLAG_FPU || mm_target_cpu_hz(&cfg) != 64000000u) return 1;
    mm_target_usart_init(&cfg, &bus, &nvic);
    mm_target_usart_reset(&cfg);
    mm_target_usart_poll(&cfg);
    mm_target_spi_init(&cfg, &bus, &nvic);
    mm_target_spi_reset(&cfg);
    mm_target_spi_poll(&cfg);
    mm_target_eth_init(&cfg, &bus, &nvic);
    mm_target_eth_reset(&cfg);
    mm_target_eth_poll(&cfg);
    for (i = 0; i < 13; ++i) {
        if (calls[i] != 1u) {
            printf("target_runtime_test: callback %d called %lu times\n", i, (unsigned long)calls[i]);
            return 1;
        }
    }
    printf("target_runtime_test: callbacks dispatched\n");
    return 0;
}
