/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

/* STM32N657: the peripherals the neural-bass bring-up touches. RCC (clock
 * config + memory enable), PWR (voltage scaling), GPIOA..Q (LED + UART
 * pins), EXTI, SYSCFG, IWDG, WWDG, DBGMCU, and USART1 (debug console).
 * Register behaviour is a plain register file with reset values; the N6 has
 * no NPU entry in the SVD, so the NPU register file lands in phase 3.
 * Base addresses are from docs/n6/STM32N657.svd; the Secure view is the
 * non-secure base + 0x10000000. */

#include "stm32n657/stm32n657_mmio.h"
#include "stm32n657/stm32n657_usart.h"

#include <string.h>

#define N657_S_OFF 0x10000000u

/* Peripheral bases, non-secure view. */
#define N657_RCC_BASE    0x46028000u
#define N657_PWR_BASE    0x46024800u
#define N657_EXTI_BASE   0x46025000u
#define N657_GPIOA_BASE  0x46020000u
#define N657_SYSCFG_BASE 0x46008000u
#define N657_IWDG_BASE   0x46004800u
#define N657_WWDG_BASE   0x40002C00u
#define N657_DBGMCU_BASE 0x44001000u
#define N657_RAMCFG_BASE 0x42023100u

#define N657_RCC_WORDS   1024u  /* 4 KB: RCC register space runs to 0xFFC */
#define N657_BLK_WORDS   64u    /* 256 B, enough for the simple peripherals */
#define N657_RAMCFG_WORDS 128u  /* 512 B: 4 instances span 0x000-0x180 */
#define N657_GPIO_WORDS  16u    /* 64 B per GPIO bank */
#define N657_GPIO_COUNT  17u    /* GPIOA..Q */
#define N657_MAX_WORDS   N657_RCC_WORDS

struct n657_soc {
    mm_u32 rcc[N657_RCC_WORDS];
    mm_u32 pwr[N657_BLK_WORDS];
    mm_u32 exti[N657_BLK_WORDS];
    mm_u32 syscfg[N657_BLK_WORDS];
    mm_u32 iwdg[N657_BLK_WORDS];
    mm_u32 wwdg[N657_BLK_WORDS];
    mm_u32 dbgmcu[N657_BLK_WORDS];
    mm_u32 ramcfg[N657_RAMCFG_WORDS];
    mm_u32 gpio[N657_GPIO_COUNT][N657_GPIO_WORDS];
};

static struct n657_soc g_soc;

static mm_u32 n657_size_mask(mm_u32 size)
{
    if (size == 1u) {
        return 0xFFu;
    }
    if (size == 2u) {
        return 0xFFFFu;
    }
    return 0xFFFFFFFFu;
}

/* Temporary diagnostic: log MMIO accesses to see where the firmware hangs. */
#include <stdio.h>
static const char *n657_reg_name(void *opaque)
{
    if (opaque == g_soc.rcc)
        return "RCC";
    if (opaque == g_soc.ramcfg)
        return "RAMCFG";
    if (opaque == g_soc.pwr)
        return "PWR";
    if (opaque == g_soc.exti)
        return "EXTI";
    if (opaque == g_soc.syscfg)
        return "SYSCFG";
    if (opaque == g_soc.iwdg)
        return "IWDG";
    if (opaque == g_soc.wwdg)
        return "WWDG";
    if (opaque == g_soc.dbgmcu)
        return "DBGMCU";
    if (opaque >= (void *)g_soc.gpio[0] &&
        opaque <= (void *)g_soc.gpio[N657_GPIO_COUNT - 1]) {
        return "GPIO";
    }
    return "?";
}

static void n657_dbg_log(const char *rw, void *opaque, mm_u32 off, mm_u32 val)
{
    static long n = 0;
    if (n < 600) {
        fprintf(stderr, "[N657MMIO] %s %-6s off=0x%03x val=0x%08x\n",
                rw, n657_reg_name(opaque), off, val);
    }
    n++;
}

/* A plain register file: reads return the stored word, writes store it.
 * Byte and half accesses hit the containing word. The word bound is the
 * largest bank (RCC); smaller banks are a subset. */
static mm_bool n657_blk_read(void *opaque, mm_u32 off, mm_u32 size,
                             mm_u32 *value_out)
{
    mm_u32 *regs = (mm_u32 *)opaque;
    mm_u32 word = off >> 2u;
    mm_u32 shift = (off & 3u) * 8u;
    if (word >= N657_MAX_WORDS) {
        return MM_FALSE;
    }
    *value_out = (regs[word] >> shift) & n657_size_mask(size);
    n657_dbg_log("R", opaque, off, *value_out);
    return MM_TRUE;
}

static mm_bool n657_blk_write(void *opaque, mm_u32 off, mm_u32 size,
                              mm_u32 value)
{
    mm_u32 *regs = (mm_u32 *)opaque;
    mm_u32 word = off >> 2u;
    mm_u32 shift = (off & 3u) * 8u;
    mm_u32 mask = n657_size_mask(size);
    if (word >= N657_MAX_WORDS) {
        return MM_FALSE;
    }
    regs[word] = (regs[word] & ~(mask << shift)) |
                 ((value & mask) << shift);
    n657_dbg_log("W", opaque, off, value);
    return MM_TRUE;
}

/* DBGMCU IDCODE/DEVID/DEVCAP report a plausible N657 identity so firmware
 * that fingerprints the part does not stall. */
static mm_bool n657_dbgmcu_read(void *opaque, mm_u32 off, mm_u32 size,
                                mm_u32 *value_out)
{
    if (off == 0x04u) {
        *value_out = 0x00001001u; /* DEVID: Cortex-M55 */
        return MM_TRUE;
    }
    return n657_blk_read(opaque, off, size, value_out);
}

/* RCC SR (offset 0x4) ready flags: LSIRDY(0) LSERDY(1) HSIRDY(3) PLL1RDY(8)
 * PLL2RDY(9) PLL3RDY(10) PLL4RDY(11). The HAL polls these after enabling an
 * oscillator/PLL; report them all set so the clock config completes. */
#define N657_RCC_SR_READY (0x1u | 0x2u | 0x8u | 0x100u | 0x200u | 0x400u | \
                           0x800u)

/* CFGR1 (offset 0x20): the clock-switch selection fields CPUSW [17:16] and
 * SYSSW [25:24] are mirrored into the status fields CPUSWS [21:20] and
 * SYSSWS [29:28] (selection << 4). The HAL polls the status fields until they
 * match the requested source; mirroring makes the switch complete at once. */
#define N657_CFGR1_CPUSW  0x00030000u
#define N657_CFGR1_SYSSW  0x03000000u
#define N657_CFGR1_CPUSWS 0x00300000u
#define N657_CFGR1_SYSSWS 0x30000000u

static mm_bool n657_rcc_read(void *opaque, mm_u32 off, mm_u32 size,
                             mm_u32 *value_out)
{
    mm_u32 *regs = (mm_u32 *)opaque;
    if (off == 0x04u) {
        *value_out = (regs[1u] | N657_RCC_SR_READY) & n657_size_mask(size);
        return MM_TRUE;
    }
    if (off == 0x20u) {
        mm_u32 v = regs[0x20u / 4u];
        v &= ~(N657_CFGR1_CPUSWS | N657_CFGR1_SYSSWS);
        v |= ((v & N657_CFGR1_CPUSW) << 4u) | ((v & N657_CFGR1_SYSSW) << 4u);
        *value_out = v & n657_size_mask(size);
        return MM_TRUE;
    }
    return n657_blk_read(opaque, off, size, value_out);
}

/* PWR VOSCR (offset 0x20) ready flags: VOSRDY(1) ACTVOSRDY(17). The HAL
 * polls VOSRDY after setting the voltage scaling. */
#define N657_PWR_VOSCR_READY (0x2u | 0x20000u)

static mm_bool n657_pwr_read(void *opaque, mm_u32 off, mm_u32 size,
                             mm_u32 *value_out)
{
    mm_u32 *regs = (mm_u32 *)opaque;
    if (off == 0x20u) {
        *value_out = (regs[0x20u / 4u] | N657_PWR_VOSCR_READY) &
                     n657_size_mask(size);
        return MM_TRUE;
    }
    return n657_blk_read(opaque, off, size, value_out);
}

static mm_bool n657_register(struct mmio_bus *bus, mm_u32 base, mm_u32 size,
                             void *opaque, mmio_read_fn read,
                             mmio_write_fn write, const char *name)
{
    struct mmio_region reg;
    memset(&reg, 0, sizeof(reg));
    reg.base = base;
    reg.size = size;
    reg.opaque = opaque;
    reg.read = read;
    reg.write = write;
    reg.name = name;
    if (!mmio_bus_register_region(bus, &reg)) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

/* Register a peripheral at both its non-secure and secure base, sharing one
 * register file (the S and NS views act on the same hardware). */
static mm_bool n657_register_both(struct mmio_bus *bus, mm_u32 ns_base,
                                  mm_u32 size, void *opaque, mmio_read_fn read,
                                  mmio_write_fn write, const char *name)
{
    if (!n657_register(bus, ns_base, size, opaque, read, write, name)) {
        return MM_FALSE;
    }
    return n657_register(bus, ns_base + N657_S_OFF, size, opaque, read,
                         write, name);
}

void mm_stm32n657_mmio_reset(void)
{
    memset(&g_soc, 0, sizeof(g_soc));
    /* DBGMCU DEVCAP: report an N657 device. */
    g_soc.dbgmcu[0x0c / 4u] = 0x00000000u;
}

mm_bool mm_stm32n657_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    (void)bank;
    (void)block_index;
    return MM_TRUE;
}

mm_u32 *mm_stm32n657_rcc_regs(void)
{
    return g_soc.rcc;
}

mm_u64 mm_stm32n657_cpu_hz(void)
{
    /* The N657 runs its Cortex-M55 at up to 320 MHz; the bring-up reports
     * this as the core clock. */
    return 320000000u;
}

mm_bool mm_stm32n657_tz_attr_for_addr(mm_u32 addr,
                                      enum mm_sau_attr *attr_out,
                                      mm_u32 *region_out)
{
    (void)addr;
    (void)attr_out;
    (void)region_out;
    return MM_FALSE;
}

mm_bool mm_stm32n657_register_mmio(struct mmio_bus *bus)
{
    mm_u32 i;

    if (!n657_register_both(bus, N657_RCC_BASE, N657_RCC_WORDS * 4u,
                            g_soc.rcc, n657_rcc_read, n657_blk_write,
                            "RCC")) {
        return MM_FALSE;
    }
    if (!n657_register_both(bus, N657_PWR_BASE, N657_BLK_WORDS * 4u,
                            g_soc.pwr, n657_pwr_read, n657_blk_write,
                            "PWR")) {
        return MM_FALSE;
    }
    if (!n657_register_both(bus, N657_EXTI_BASE, N657_BLK_WORDS * 4u,
                            g_soc.exti, n657_blk_read, n657_blk_write,
                            "EXTI")) {
        return MM_FALSE;
    }
    if (!n657_register_both(bus, N657_SYSCFG_BASE, N657_BLK_WORDS * 4u,
                            g_soc.syscfg, n657_blk_read, n657_blk_write,
                            "SYSCFG")) {
        return MM_FALSE;
    }
    if (!n657_register_both(bus, N657_RAMCFG_BASE, N657_RAMCFG_WORDS * 4u,
                            g_soc.ramcfg, n657_blk_read, n657_blk_write,
                            "RAMCFG")) {
        return MM_FALSE;
    }
    if (!n657_register_both(bus, N657_IWDG_BASE, N657_BLK_WORDS * 4u,
                            g_soc.iwdg, n657_blk_read, n657_blk_write,
                            "IWDG")) {
        return MM_FALSE;
    }
    if (!n657_register_both(bus, N657_WWDG_BASE, N657_BLK_WORDS * 4u,
                            g_soc.wwdg, n657_blk_read, n657_blk_write,
                            "WWDG")) {
        return MM_FALSE;
    }
    if (!n657_register_both(bus, N657_DBGMCU_BASE, N657_BLK_WORDS * 4u,
                            g_soc.dbgmcu, n657_dbgmcu_read, n657_blk_write,
                            "DBGMCU")) {
        return MM_FALSE;
    }
    for (i = 0u; i < N657_GPIO_COUNT; ++i) {
        if (!n657_register_both(bus, N657_GPIOA_BASE + (i * 0x400u),
                                N657_GPIO_WORDS * 4u, g_soc.gpio[i],
                                n657_blk_read, n657_blk_write,
                                "GPIO")) {
            return MM_FALSE;
        }
    }
    mm_stm32n657_usart_init(bus, 0);
    return MM_TRUE;
}
