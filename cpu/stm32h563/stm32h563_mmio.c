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

#include <string.h>
#include <sys/random.h>
#include <stdlib.h>
#include <stdio.h>
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h563/stm32h563_usb.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/gpio.h"

extern void mm_system_request_reset(void);

/* RCC base addresses (system domain) */
#define RCC_BASE     0x44020c00u
#define RCC_SEC_BASE 0x54020c00u
#define RCC_SIZE     0x400u

#define RCC_CR       0x000u
#define RCC_CFGR1    0x01cu
#define RCC_CFGR2    0x020u
#define RCC_PLL1CFGR 0x028u
#define RCC_PLL1DIVR 0x034u

/* PWR base (system domain) */
#define PWR_BASE     0x44020800u
#define PWR_SEC_BASE 0x54020800u
#define PWR_SIZE     0x400u

/* FLASH controller base */
#define FLASH_BASE   0x40022000u
#define FLASH_SEC_BASE 0x50022000u
#define FLASH_SIZE   0x400u

/* GTZC TZSC/TZIC (secure / non-secure aliases) */
#define GTZC_TZSC_S_BASE 0x50032400u
#define GTZC_TZSC_NS_BASE 0x40032400u
#define GTZC_TZIC_S_BASE 0x50032800u
#define GTZC_TZIC_NS_BASE 0x40032800u
#define GTZC_TZSC_SIZE 0x400u
#define GTZC_TZIC_SIZE 0x400u
#define GTZC_BLK_SIZE 0x1000u

/* RNG base addresses */
#define RNG_BASE     0x420c0800u
#define RNG_SEC_BASE 0x520c0800u
#define RNG_SIZE     0x400u

/* MPCBB base addresses */
#define MPCBB1_BASE     0x40032c00u
#define MPCBB1_SEC_BASE 0x50032c00u
#define MPCBB2_BASE     0x40033000u
#define MPCBB2_SEC_BASE 0x50033000u
#define MPCBB3_BASE     0x40033400u
#define MPCBB3_SEC_BASE 0x50033400u
#define MPCBB_SIZE      0x400u

/* EXTI base addresses */
#define EXTI_BASE     0x44022000u
#define EXTI_SEC_BASE 0x54022000u
#define EXTI_SIZE     0x400u

/* IWDG/WWDG base addresses */
#define IWDG_BASE     0x40003000u
#define IWDG_SEC_BASE 0x50003000u
#define IWDG_SIZE     0x400u
#define WWDG_BASE     0x40002C00u
#define WWDG_SEC_BASE 0x50002C00u
#define WWDG_SIZE     0x400u

#define RNG_CR_OFFSET   0x0u
#define RNG_SR_OFFSET   0x4u
#define RNG_DR_OFFSET   0x8u
#define RNG_NSCR_OFFSET 0xcu
#define RNG_HTCR_OFFSET 0x10u

/* EXTI offsets (subset) */
#define EXTI_RTSR1  0x000u
#define EXTI_FTSR1  0x004u
#define EXTI_SWIER1 0x008u
#define EXTI_RPR1   0x00Cu
#define EXTI_FPR1   0x010u
#define EXTI_EXTICR1 0x060u
#define EXTI_EXTICR2 0x064u
#define EXTI_EXTICR3 0x068u
#define EXTI_EXTICR4 0x06Cu
#define EXTI_IMR1   0x080u
#define EXTI_EMR1   0x084u

/* WWDG offsets */
#define WWDG_CR  0x000u
#define WWDG_CFR 0x004u
#define WWDG_SR  0x008u

/* IWDG offsets */
#define IWDG_KR   0x000u
#define IWDG_PR   0x004u
#define IWDG_RLR  0x008u
#define IWDG_SR   0x00Cu
#define IWDG_WINR 0x010u
#define IWDG_EWCR 0x014u

#define GPIO_IDR_OFFSET 0x10u
#define GPIO_ODR_OFFSET 0x14u

/* MPCBB offsets */
#define MPCBB_CR_OFFSET      0x000u
#define MPCBB_SECCFGR_OFFSET 0x100u
#define MPCBB_CR_INVSECSTATE (1u << 30)

/* FLASH register offsets */
#define FLASH_ACR      0x000u
#define FLASH_NSKEYR   0x004u
#define FLASH_SECKEYR  0x008u
#define FLASH_NSSR     0x020u
#define FLASH_SECSR    0x024u
#define FLASH_NSCR     0x028u
#define FLASH_SECCR    0x02cu
#define FLASH_NSCCR    0x030u
#define FLASH_SECCCR   0x034u

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu

#define FLASH_FLAG_BSY (1u << 0)
#define FLASH_FLAG_EOP (1u << 16)

#define FLASH_CR_LOCK (1u << 0)
#define FLASH_CR_PG   (1u << 1)
#define FLASH_CR_SER  (1u << 2)
#define FLASH_CR_BER  (1u << 3)
#define FLASH_CR_STRT (1u << 5)
#define FLASH_CR_SNB_SHIFT 6
#define FLASH_CR_SNB_MASK (0x7fu << FLASH_CR_SNB_SHIFT)

#define FLASH_SECTOR_COUNT 128u
#define FLASH_BANK_COUNT   2u
#define FLASH_CR_BKSEL     (1u << 31)

struct rcc_state {
    mm_u32 regs[RCC_SIZE / 4];
    mm_u64 cpu_hz;
};

struct pwr_state {
    mm_u32 regs[PWR_SIZE / 4];
};

struct simple_blk {
    mm_u32 regs[GTZC_BLK_SIZE / 4];
};

struct mpcbb_state {
    mm_u32 regs[MPCBB_SIZE / 4];
};

struct rng_state {
    mm_u32 regs[RNG_SIZE / 4];
    mm_u32 dr;
    mm_bool dr_valid;
};

struct exti_state {
    mm_u32 regs[EXTI_SIZE / 4];
};

struct iwdg_state {
    mm_u32 regs[IWDG_SIZE / 4];
    mm_u32 counter;
    mm_u32 prescaler;
    mm_bool running;
    mm_bool write_access;
    mm_u64 accum;
};

struct wwdg_state {
    mm_u32 regs[WWDG_SIZE / 4];
    mm_u32 counter;
    mm_u64 accum;
};

struct flash_state {
    mm_u32 regs[FLASH_SIZE / 4];
    mm_u8 *flash;
    mm_u32 flash_size;
    mm_u32 base_s;
    mm_u32 base_ns;
    const struct mm_flash_persist *persist;
    mm_u32 flags;
    mm_u8 ns_key_stage;
    mm_u8 sec_key_stage;
};

struct gpio_state {
    mm_u32 regs[0x34 / 4]; /* up to SECCFGR */
};

struct gpdma_state {
    mm_u32 regs[0x1000 / 4];
};

/* uintptr_t substitute for C90 */
typedef unsigned long mm_uptr;

static void rcc_update_ready(struct rcc_state *r);
static void rcc_update_sysclk(struct rcc_state *r);
static void pwr_update_vos(struct pwr_state *p);
static void mpcbb_init_defaults(void);

static struct rcc_state rcc;
static struct pwr_state pwr;
static struct simple_blk tzsc_s;
static struct simple_blk tzsc_ns;
static struct simple_blk tzic_s;
static struct simple_blk tzic_ns;
static struct mpcbb_state mpcbb[3];
static struct rng_state rng;
static struct exti_state exti;
static struct iwdg_state iwdg;
static struct wwdg_state wwdg;
static struct flash_state flash_ctl;
static struct gpio_state gpio[9]; /* A..I */
static struct gpdma_state gpdma1;
static struct gpdma_state gpdma2;
static void *gpio_ctx[18][4];
static void *rng_ctx[2][4];
static struct mm_nvic *g_rng_nvic = 0;
static struct mm_nvic *g_exti_nvic = 0;
static struct mm_nvic *g_wdg_nvic = 0;

#define RNG_IRQ 114
static const mm_u32 mpcbb_words[] = { 32u, 32u, 32u };

static mm_u32 stm32h563_gpio_bank_read(void *opaque, int bank);
static mm_u32 stm32h563_gpio_bank_read_moder(void *opaque, int bank);
static mm_bool stm32h563_gpio_bank_clock(void *opaque, int bank);
static mm_u32 stm32h563_gpio_bank_read_seccfgr(void *opaque, int bank);
static void exti_gpio_update(int bank, mm_u32 old_level, mm_u32 new_level);

static mm_bool flash_trace_enabled(void)
{
    static mm_bool init = MM_FALSE;
    static mm_bool enabled = MM_FALSE;
    if (!init) {
        const char *env = getenv("M33MU_FLASH_TRACE");
        enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
        init = MM_TRUE;
    }
    return enabled;
}

static mm_bool stm32h563_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len);

void mm_stm32h563_mmio_reset(void)
{
    size_t i;
    memset(&rcc, 0, sizeof(rcc));
    memset(&pwr, 0, sizeof(pwr));
    memset(&tzsc_s, 0, sizeof(tzsc_s));
    memset(&tzsc_ns, 0, sizeof(tzsc_ns));
    memset(&tzic_s, 0, sizeof(tzic_s));
    memset(&tzic_ns, 0, sizeof(tzic_ns));
    memset(&rng, 0, sizeof(rng));
    memset(&exti, 0, sizeof(exti));
    memset(&iwdg, 0, sizeof(iwdg));
    memset(&wwdg, 0, sizeof(wwdg));
    memset(&flash_ctl, 0, sizeof(flash_ctl));
    mpcbb_init_defaults();
    memset(&gpdma1, 0, sizeof(gpdma1));
    memset(&gpdma2, 0, sizeof(gpdma2));
    for (i = 0; i < sizeof(gpio) / sizeof(gpio[0]); ++i) {
        memset(&gpio[i], 0, sizeof(gpio[i]));
    }
    mm_stm32h563_usb_reset();
    mm_gpio_bank_set_reader(stm32h563_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(stm32h563_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(stm32h563_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(stm32h563_gpio_bank_read_seccfgr, 0);
    mm_rcc_set_clock_list_reader(stm32h563_rcc_clock_list_line, 0);
    /* Enable HSI by default and mark ready flags. */
    rcc.regs[0] |= 1u;
    rcc_update_ready(&rcc);
    rcc_update_sysclk(&rcc);
    iwdg.regs[IWDG_RLR / 4u] = 0x00000FFFu;
    iwdg.regs[IWDG_WINR / 4u] = 0x00000FFFu;
    wwdg.regs[WWDG_CR / 4u] = 0x0000007Fu;
    wwdg.regs[WWDG_CFR / 4u] = 0x0000007Fu;
    wwdg.counter = 0x7Fu;
    exti.regs[EXTI_IMR1 / 4u] = 0xFFFE0000u;
    /* Power ready flags. */
    pwr_update_vos(&pwr);

    /* RNG reset values */
    rng.regs[RNG_CR_OFFSET / 4] = 0x00871f00u;
    rng.regs[RNG_HTCR_OFFSET / 4] = 0x000072acu;

    /* FLASH reset values */
    flash_ctl.regs[FLASH_ACR / 4] = 0x00000013u;
    flash_ctl.regs[FLASH_NSCR / 4] = 0x00000001u;
    flash_ctl.regs[FLASH_SECCR / 4] = 0x00000001u;
}

mm_u32 *mm_stm32h563_tzsc_regs(void)
{
    return tzsc_s.regs;
}

mm_bool mm_stm32h563_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    mm_u32 word;
    mm_u32 bit;
    mm_u32 val;
    mm_bool sec;
    if (bank < 0 || bank >= (int)(sizeof(mpcbb) / sizeof(mpcbb[0]))) {
        return MM_FALSE;
    }
    word = block_index / 32u;
    if (word >= mpcbb_words[bank]) {
        return MM_FALSE;
    }
    bit = block_index % 32u;
    val = mpcbb[bank].regs[(MPCBB_SECCFGR_OFFSET / 4u) + word];
    sec = ((val >> bit) & 1u) != 0u;
    if ((mpcbb[bank].regs[MPCBB_CR_OFFSET / 4u] & MPCBB_CR_INVSECSTATE) != 0u) {
        sec = !sec;
    }
    return sec;
}

static mm_bool gpio_clock_enabled(const struct rcc_state *rcc, int index)
{
    /* AHB2ENR offset 0x8C, GPIOAEN bit0..GPIOIEN bit8 */
    mm_u32 ahb2enr = rcc->regs[0x8c / 4];
    return ((ahb2enr >> index) & 1u) != 0u;
}

enum rcc_bus_kind {
    RCC_BUS_AHB2 = 0,
    RCC_BUS_APB1L = 1,
    RCC_BUS_APB1H = 2,
    RCC_BUS_APB2 = 3,
    RCC_BUS_APB3 = 4
};

struct rcc_clk_name {
    const char *name;
    enum rcc_bus_kind bus;
    mm_u32 bit;
};

static const struct rcc_clk_name rcc_clk_names[] = {
    { "GPIOA", RCC_BUS_AHB2, 0u },
    { "GPIOB", RCC_BUS_AHB2, 1u },
    { "GPIOC", RCC_BUS_AHB2, 2u },
    { "GPIOD", RCC_BUS_AHB2, 3u },
    { "GPIOE", RCC_BUS_AHB2, 4u },
    { "GPIOF", RCC_BUS_AHB2, 5u },
    { "GPIOG", RCC_BUS_AHB2, 6u },
    { "GPIOH", RCC_BUS_AHB2, 7u },
    { "GPIOI", RCC_BUS_AHB2, 8u },
    { "RNG", RCC_BUS_AHB2, 18u },
    { "TIM2", RCC_BUS_APB1L, 0u },
    { "TIM3", RCC_BUS_APB1L, 1u },
    { "TIM4", RCC_BUS_APB1L, 2u },
    { "TIM5", RCC_BUS_APB1L, 3u },
    { "USART2", RCC_BUS_APB1L, 17u },
    { "USART3", RCC_BUS_APB1L, 18u },
    { "UART4", RCC_BUS_APB1L, 19u },
    { "UART5", RCC_BUS_APB1L, 20u },
    { "USART6", RCC_BUS_APB1L, 25u },
    { "UART7", RCC_BUS_APB1L, 30u },
    { "UART8", RCC_BUS_APB1L, 31u },
    { "UART9", RCC_BUS_APB1H, 0u },
    { "UART12", RCC_BUS_APB1H, 1u },
    { "USART1", RCC_BUS_APB2, 14u }
};

static mm_u32 rcc_bus_reg(const struct rcc_state *r, enum rcc_bus_kind bus)
{
    if (r == 0) {
        return 0u;
    }
    switch (bus) {
    case RCC_BUS_AHB2:
        return r->regs[0x8c / 4];
    case RCC_BUS_APB1L:
        return r->regs[0x9c / 4];
    case RCC_BUS_APB1H:
        return r->regs[0xa0 / 4];
    case RCC_BUS_APB2:
        return r->regs[0xa4 / 4];
    case RCC_BUS_APB3:
        return r->regs[0xa8 / 4];
    default:
        return 0u;
    }
}

static const char *rcc_bus_name(enum rcc_bus_kind bus)
{
    switch (bus) {
    case RCC_BUS_AHB2: return "AHB2";
    case RCC_BUS_APB1L: return "APB1L";
    case RCC_BUS_APB1H: return "APB1H";
    case RCC_BUS_APB2: return "APB2";
    case RCC_BUS_APB3: return "APB3";
    default: return "RCC";
    }
}

static mm_bool stm32h563_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len)
{
    enum rcc_bus_kind bus;
    int line_idx = 0;
    size_t i;
    (void)opaque;
    if (out == 0 || out_len == 0u) {
        return MM_FALSE;
    }
    for (bus = RCC_BUS_AHB2; bus <= RCC_BUS_APB3; ++bus) {
        mm_u32 reg = rcc_bus_reg(&rcc, bus);
        char buf[256];
        size_t pos = 0;
        mm_bool have = MM_FALSE;
        if (reg == 0u) {
            continue;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s:", rcc_bus_name(bus));
        for (i = 0; i < sizeof(rcc_clk_names) / sizeof(rcc_clk_names[0]); ++i) {
            if (rcc_clk_names[i].bus != bus) {
                continue;
            }
            if ((reg & (1u << rcc_clk_names[i].bit)) == 0u) {
                continue;
            }
            if (pos + 2u < sizeof(buf)) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", rcc_clk_names[i].name);
                have = MM_TRUE;
            }
        }
        if (!have) {
            continue;
        }
        if (line_idx == line) {
            snprintf(out, out_len, "%s", buf);
            return MM_TRUE;
        }
        line_idx++;
    }
    return MM_FALSE;
}

static mm_u32 stm32h563_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[0x14u / 4];
}

static mm_u32 stm32h563_gpio_bank_read_moder(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[0x00u / 4];
}

static mm_bool stm32h563_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    return gpio_clock_enabled(&rcc, bank);
}

static mm_u32 stm32h563_gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[0x30u / 4];
}

static mm_bool rng_clock_enabled(const struct rcc_state *rcc)
{
    /* RCC_AHB2ENR offset 0x8C, RNGEN bit18 */
    mm_u32 ahb2enr = rcc->regs[0x8c / 4];
    return ((ahb2enr >> 18) & 1u) != 0u;
}

static mm_bool rng_requires_secure(const struct simple_blk *tzsc)
{
    /* GTZC1_TZSC_SECCFGR2 offset 0x14, RNGSEC bit18 */
    mm_u32 seccfgr2 = tzsc->regs[0x14u / 4];
    return ((seccfgr2 >> 18) & 1u) != 0u;
}

static void rng_fill(struct rng_state *r)
{
    mm_u32 v = 0;
    ssize_t n = getrandom(&v, sizeof(v), GRND_NONBLOCK);
    if (n != (ssize_t)sizeof(v)) {
        v = 0;
    }
    r->dr = v;
    r->dr_valid = MM_TRUE;
    r->regs[RNG_SR_OFFSET / 4] |= 1u;
    if (g_rng_nvic != 0 && (r->regs[RNG_CR_OFFSET / 4] & (1u << 3)) != 0u) {
        mm_nvic_set_pending(g_rng_nvic, (mm_u32)RNG_IRQ, MM_TRUE);
    }
}

static mm_u32 flash_sector_size(void)
{
    mm_u32 banks = FLASH_BANK_COUNT;
    if (flash_ctl.flash_size == 0u) {
        return 0u;
    }
    if (banks == 0u || (flash_ctl.flash_size % banks) != 0u) {
        banks = 1u;
    }
    return (FLASH_SECTOR_COUNT == 0u) ? 0u
        : ((flash_ctl.flash_size / banks) / FLASH_SECTOR_COUNT);
}

static void flash_set_busy(mm_u32 reg_offset, mm_bool busy)
{
    mm_u32 idx = reg_offset / 4u;
    if (busy) {
        flash_ctl.regs[idx] |= FLASH_FLAG_BSY;
    } else {
        flash_ctl.regs[idx] &= ~FLASH_FLAG_BSY;
    }
}

static void flash_set_eop(mm_u32 reg_offset)
{
    flash_ctl.regs[reg_offset / 4u] |= FLASH_FLAG_EOP;
}

static void flash_clear_eop(mm_u32 reg_offset)
{
    flash_ctl.regs[reg_offset / 4u] &= ~FLASH_FLAG_EOP;
}

static mm_bool flash_is_unlocked(mm_u32 reg_offset)
{
    return (flash_ctl.regs[reg_offset / 4u] & FLASH_CR_LOCK) == 0u;
}

static void flash_handle_key(mm_u32 offset, mm_u32 value)
{
    mm_u8 *stage = (offset == FLASH_NSKEYR) ? &flash_ctl.ns_key_stage : &flash_ctl.sec_key_stage;
    mm_u32 cr_off = (offset == FLASH_NSKEYR) ? FLASH_NSCR : FLASH_SECCR;
    if (*stage == 0u) {
        if (value == FLASH_KEY1) {
            *stage = 1u;
        } else {
            *stage = 0u;
        }
        return;
    }
    if (*stage == 1u) {
        if (value == FLASH_KEY2) {
            flash_ctl.regs[cr_off / 4u] &= ~FLASH_CR_LOCK;
        }
        *stage = 0u;
    }
}

static void flash_apply_erase(mm_u32 cr_off, mm_u32 sr_off)
{
    mm_u32 cr = flash_ctl.regs[cr_off / 4u];
    mm_u32 sector_size = flash_sector_size();
    mm_u32 bank_size = 0;
    mm_u32 bank_offset = 0;
    mm_u32 start = 0;
    mm_u32 length = 0;
    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0 || sector_size == 0) {
        return;
    }
    if ((cr & FLASH_CR_BER) != 0u) {
        start = 0;
        length = flash_ctl.flash_size;
    } else if ((cr & FLASH_CR_SER) != 0u) {
        mm_u32 snb = (cr & FLASH_CR_SNB_MASK) >> FLASH_CR_SNB_SHIFT;
        start = snb * sector_size;
        bank_size = flash_ctl.flash_size / FLASH_BANK_COUNT;
        if (FLASH_BANK_COUNT != 0u && bank_size != 0u && (cr & FLASH_CR_BKSEL) != 0u) {
            bank_offset = bank_size;
            start += bank_offset;
        }
        if (start >= flash_ctl.flash_size) {
            return;
        }
        length = sector_size;
        if (start + length > flash_ctl.flash_size) {
            length = flash_ctl.flash_size - start;
        }
    } else {
        return;
    }
    if (flash_trace_enabled()) {
        const char *sec = (cr_off == FLASH_SECCR) ? "S" : "NS";
        const char *mode = (cr & FLASH_CR_BER) ? "BER" : "SER";
        mm_u32 snb = (cr & FLASH_CR_SNB_MASK) >> FLASH_CR_SNB_SHIFT;
        printf("[FLASH_ERASE] %s mode=%s snb=%lu start=0x%08lx len=0x%08lx\n",
               sec,
               mode,
               (unsigned long)snb,
               (unsigned long)start,
               (unsigned long)length);
    }
    flash_set_busy(sr_off, MM_TRUE);
    memset(flash_ctl.flash + start, 0xFF, length);
    flash_set_busy(sr_off, MM_FALSE);
    flash_set_eop(sr_off);
    if (flash_ctl.persist != 0 && flash_ctl.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)flash_ctl.persist, start, length);
    }
}

static mm_bool flash_write_cb(void *opaque, enum mm_sec_state sec, mm_u32 addr, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 base;
    mm_u32 offset;
    mm_u32 cr_off;
    mm_u32 sr_off;
    mm_u32 sector_size;
    mm_u32 sector_base;
    mm_u32 i;
    (void)opaque;

    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0) {
        return MM_FALSE;
    }
    if (addr >= flash_ctl.base_s && addr < flash_ctl.base_s + flash_ctl.flash_size) {
        base = flash_ctl.base_s;
    } else if (addr >= flash_ctl.base_ns && addr < flash_ctl.base_ns + flash_ctl.flash_size) {
        base = flash_ctl.base_ns;
    } else {
        return MM_FALSE;
    }
    offset = addr - base;
    cr_off = (sec == MM_SECURE) ? FLASH_SECCR : FLASH_NSCR;
    sr_off = (sec == MM_SECURE) ? FLASH_SECSR : FLASH_NSSR;

    if (offset + size_bytes > flash_ctl.flash_size) {
        return MM_FALSE;
    }
    if (flash_trace_enabled()) {
        printf("[FLASH_WRITE] %s addr=0x%08lx size=%lu value=0x%08lx\n",
               (sec == MM_SECURE) ? "S" : "NS",
               (unsigned long)addr,
               (unsigned long)size_bytes,
               (unsigned long)value);
    }
    if (!flash_is_unlocked(cr_off)) {
        return MM_TRUE;
    }
    if ((flash_ctl.regs[cr_off / 4u] & FLASH_CR_PG) == 0u) {
        return MM_TRUE;
    }

    sector_size = flash_sector_size();
    if ((flash_ctl.flags & MM_TARGET_FLAG_NVM_WRITEONCE) != 0u && sector_size != 0u) {
        sector_base = (offset / sector_size) * sector_size;
        for (i = 0; i < sector_size; ++i) {
            if (sector_base + i >= flash_ctl.flash_size) {
                break;
            }
            if (flash_ctl.flash[sector_base + i] != 0xFFu) {
                return MM_TRUE;
            }
        }
    }

    if (size_bytes == 4u) {
        flash_ctl.flash[offset] = (mm_u8)(value & 0xFFu);
        flash_ctl.flash[offset + 1u] = (mm_u8)((value >> 8) & 0xFFu);
        flash_ctl.flash[offset + 2u] = (mm_u8)((value >> 16) & 0xFFu);
        flash_ctl.flash[offset + 3u] = (mm_u8)((value >> 24) & 0xFFu);
    } else if (size_bytes == 2u) {
        flash_ctl.flash[offset] = (mm_u8)(value & 0xFFu);
        flash_ctl.flash[offset + 1u] = (mm_u8)((value >> 8) & 0xFFu);
    } else if (size_bytes == 1u) {
        flash_ctl.flash[offset] = (mm_u8)(value & 0xFFu);
    } else {
        return MM_FALSE;
    }
    flash_set_eop(sr_off);
    if (flash_ctl.persist != 0 && flash_ctl.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)flash_ctl.persist, offset, size_bytes);
    }
    return MM_TRUE;
}

/* Reset bits currently unused (could clear state if asserted). */

static mm_bool rcc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)r->regs + offset, size_bytes);
    return MM_TRUE;
}

static void rcc_update_ready(struct rcc_state *r)
{
    mm_u32 cr = r->regs[0]; /* offset 0x0 */
    /* Mirror RDY bits to match ON bits (immediate ready). */
    /* HSIRDY bit1 follows HSION bit0 */
    if ((cr & (1u << 0)) != 0u) cr |= (1u << 1); else cr &= ~(1u << 1);
    /* CSIRDY bit9 follows CSION bit8 */
    if ((cr & (1u << 8)) != 0u) cr |= (1u << 9); else cr &= ~(1u << 9);
    /* HSI48RDY bit13 follows HSI48ON bit12 */
    if ((cr & (1u << 12)) != 0u) cr |= (1u << 13); else cr &= ~(1u << 13);
    /* HSERDY bit17 follows HSEON bit16 */
    if ((cr & (1u << 16)) != 0u) cr |= (1u << 17); else cr &= ~(1u << 17);
    /* PLL1RDY bit25 follows PLL1ON bit24 */
    if ((cr & (1u << 24)) != 0u) cr |= (1u << 25); else cr &= ~(1u << 25);
    r->regs[0] = cr;
}

static mm_u64 rcc_pll1_p_clk(const struct rcc_state *r)
{
    mm_u32 pllcfgr = r->regs[RCC_PLL1CFGR / 4];
    mm_u32 plldivr = r->regs[RCC_PLL1DIVR / 4];
    mm_u32 src = pllcfgr & 0x3u;
    mm_u64 fin = 0;
    mm_u32 divm = (pllcfgr >> 8) & 0x3fu;
    mm_u32 n = (plldivr & 0x1ffu) + 1u;
    mm_u32 p = ((plldivr >> 9) & 0x7fu) + 1u;

    if (src == 1u) fin = 64000000ull; /* HSI */
    else if (src == 2u) fin = 4000000ull; /* CSI */
    else if (src == 3u) fin = 8000000ull; /* HSE */
    else fin = 0;

    if (fin == 0 || divm == 0u || p == 0u) {
        return 0;
    }
    return (fin / (mm_u64)divm) * (mm_u64)n / (mm_u64)p;
}

static void rcc_update_sysclk(struct rcc_state *r)
{
    mm_u32 cfgr1 = r->regs[RCC_CFGR1 / 4];
    mm_u32 cfgr2 = r->regs[RCC_CFGR2 / 4];
    mm_u32 sw = cfgr1 & 0x7u;
    mm_u32 hpre = cfgr2 & 0xfu;
    mm_u64 sys = 0;
    mm_u32 div = 1u;

    if (sw == 0u) sys = 64000000ull;
    else if (sw == 1u) sys = 4000000ull;
    else if (sw == 2u) sys = 8000000ull;
    else if (sw == 3u) sys = rcc_pll1_p_clk(r);

    if (hpre >= 8u) {
        switch (hpre) {
        case 8u: div = 2u; break;
        case 9u: div = 4u; break;
        case 10u: div = 8u; break;
        case 11u: div = 16u; break;
        case 12u: div = 64u; break;
        case 13u: div = 128u; break;
        case 14u: div = 256u; break;
        case 15u: div = 512u; break;
        default: div = 1u; break;
        }
    }
    if (sys == 0) {
        r->cpu_hz = 0;
    } else {
        r->cpu_hz = sys / (mm_u64)div;
        if (r->cpu_hz == 0) r->cpu_hz = 1;
    }
    r->regs[RCC_CFGR1 / 4] = (r->regs[RCC_CFGR1 / 4] & ~(0x7u << 3)) | (sw << 3);
}

static mm_bool rcc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)r->regs + offset, &value, size_bytes);
    if (offset == RCC_CR) {
        rcc_update_ready(r);
    }
    if (offset == RCC_CFGR1 || offset == RCC_CFGR2 ||
        offset == RCC_PLL1CFGR || offset == RCC_PLL1DIVR ||
        offset == RCC_CR) {
        rcc_update_sysclk(r);
    }
    return MM_TRUE;
}

static mm_bool pwr_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct pwr_state *p = (struct pwr_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)p->regs + offset, size_bytes);
    return MM_TRUE;
}

static void pwr_update_vos(struct pwr_state *p)
{
    mm_u32 voscr = p->regs[0x10 / 4];
    mm_u32 vos = (voscr >> 4) & 0x3u;
    mm_u32 vossr = p->regs[0x14 / 4];
    vossr &= ~((1u << 14) | (1u << 13));
    vossr |= (vos << 14);      /* ACTVOS mirrors VOS */
    vossr |= (1u << 13);       /* ACTVOSRDY set */
    vossr |= (1u << 3);        /* VOSRDY set */
    p->regs[0x14 / 4] = vossr;
}

static mm_bool pwr_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct pwr_state *p = (struct pwr_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)p->regs + offset, &value, size_bytes);
    if (offset == 0x10u) {
        pwr_update_vos(p);
    }
    return MM_TRUE;
}

static mm_bool simple_blk_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct simple_blk *b = (struct simple_blk *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GTZC_BLK_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)b->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool simple_blk_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct simple_blk *b = (struct simple_blk *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GTZC_BLK_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)b->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool mpcbb_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct mpcbb_state *b = (struct mpcbb_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > MPCBB_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)b->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool mpcbb_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct mpcbb_state *b = (struct mpcbb_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > MPCBB_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)b->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static void mpcbb_init_defaults(void)
{
    mm_u32 bank;
    for (bank = 0; bank < (mm_u32)(sizeof(mpcbb) / sizeof(mpcbb[0])); ++bank) {
        mm_u32 word;
        memset(&mpcbb[bank], 0, sizeof(mpcbb[bank]));
        for (word = 0; word < mpcbb_words[bank]; ++word) {
            mpcbb[bank].regs[(MPCBB_SECCFGR_OFFSET / 4u) + word] = 0xFFFFFFFFu;
        }
    }
}

static mm_bool gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct gpio_state *g = ((struct gpio_state *)((void **)opaque)[0]);
    struct rcc_state *rcc = (struct rcc_state *)((void **)opaque)[2];
    int index = (int)(mm_uptr)((void **)opaque)[3];
    mm_bool is_secure_alias = ((void **)opaque)[1] != 0;
    mm_u32 seccfgr = g->regs[0x30u / 4];
    mm_u32 v = 0;
    mm_u32 mask = ~seccfgr; /* pins accessible to NS */

    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset >= sizeof(g->regs)) return MM_FALSE;

    if (!gpio_clock_enabled(rcc, index)) {
        *value_out = 0;
        return MM_TRUE;
    }

    memcpy(&v, (mm_u8 *)g->regs + offset, size_bytes);

    if (!is_secure_alias) {
        if (offset == 0x30u) { /* SECCFGR */
            v = 0;
        } else if (offset == 0x10u || offset == 0x14u || offset == 0x18u || offset == 0x1cu || offset == 0x20u || offset == 0x24u || offset == 0x28u || offset == 0x2cu) {
            /* Data/bit set/reset/lock/AF/BRR/HSLVR: mask secure pins */
            v &= mask;
        } else if (offset <= 0x0cu || offset == 0x04u || offset == 0x08u) {
            /* config registers are readable but secure bits masked */
            v &= mask | (mask << 16); /* apply per-bit twice per 2 bits? conservative: mask low 16 bits */ 
        }
    }

    *value_out = v;
    return MM_TRUE;
}

static void gpio_apply_brr(struct gpio_state *g, mm_u32 bits, mm_u32 mask)
{
    g->regs[0x14u / 4] &= ~(bits & mask);
}

static void gpio_apply_bsrr(struct gpio_state *g, mm_u32 val, mm_u32 mask)
{
    mm_u32 set = val & 0xFFFFu;
    mm_u32 reset = (val >> 16) & 0xFFFFu;
    mm_u32 odr = g->regs[0x14u / 4];
    odr |= (set & mask);
    odr &= ~(reset & mask);
    g->regs[0x14u / 4] = odr;
}

static void gpio_sync_odr(struct gpio_state *g, int bank, mm_u32 old_odr)
{
    mm_u32 new_odr = g->regs[GPIO_ODR_OFFSET / 4];
    if (new_odr != old_odr) {
        g->regs[GPIO_IDR_OFFSET / 4] = new_odr;
        exti_gpio_update(bank, old_odr, new_odr);
    }
}

static mm_u32 gpio_mask_to_2bit(mm_u32 mask)
{
    mm_u32 out = 0;
    int i;
    for (i = 0; i < 16; ++i) {
        if ((mask & (1u << i)) != 0u) {
            out |= (3u << (i * 2));
        }
    }
    return out;
}

static mm_bool gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct gpio_state *g = ((struct gpio_state *)((void **)opaque)[0]);
    struct rcc_state *rcc = (struct rcc_state *)((void **)opaque)[2];
    int index = (int)(mm_uptr)((void **)opaque)[3];
    mm_bool is_secure_alias = ((void **)opaque)[1] != 0;
    mm_u32 seccfgr = g->regs[0x30u / 4];
    mm_u32 mask = ~seccfgr;

    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset >= sizeof(g->regs)) return MM_FALSE;

    if (!gpio_clock_enabled(rcc, index)) {
        return MM_TRUE; /* WI if disabled */
    }

    /* Non-secure alias write handling */
    if (!is_secure_alias) {
        if (offset == 0x30u) {
            /* SECCFGR is Secure-only: WI */
            return MM_TRUE;
        }
        /* Mask out secure pins */
        if (offset == 0x18u) { /* BSRR */
            mm_u32 old_odr = g->regs[GPIO_ODR_OFFSET / 4];
            gpio_apply_bsrr(g, value, mask & 0xFFFFu);
            gpio_sync_odr(g, index, old_odr);
            return MM_TRUE;
        } else if (offset == 0x28u) { /* BRR */
            mm_u32 old_odr = g->regs[GPIO_ODR_OFFSET / 4];
            gpio_apply_brr(g, value & 0xFFFFu, mask & 0xFFFFu);
            gpio_sync_odr(g, index, old_odr);
            return MM_TRUE;
        } else {
            /* General regs: apply mask */
            if (offset == 0x00u) { /* MODER: 2 bits per pin */
                mm_u32 m2 = gpio_mask_to_2bit(mask & 0xFFFFu);
                value &= (m2 | (m2 << 16));
            } else {
                value &= (mask | (mask << 16));
            }
        }
    }

    if (offset == 0x18u) { /* BSRR */
        mm_u32 old_odr = g->regs[GPIO_ODR_OFFSET / 4];
        gpio_apply_bsrr(g, value, 0xFFFFu);
        gpio_sync_odr(g, index, old_odr);
        return MM_TRUE;
    }
    if (offset == 0x28u) { /* BRR */
        mm_u32 old_odr = g->regs[GPIO_ODR_OFFSET / 4];
        gpio_apply_brr(g, value & 0xFFFFu, 0xFFFFu);
        gpio_sync_odr(g, index, old_odr);
        return MM_TRUE;
    }

    if (offset == GPIO_ODR_OFFSET) {
        mm_u32 old_odr = g->regs[GPIO_ODR_OFFSET / 4];
        memcpy((mm_u8 *)g->regs + offset, &value, size_bytes);
        gpio_sync_odr(g, index, old_odr);
        return MM_TRUE;
    }

    memcpy((mm_u8 *)g->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool gpdma_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct gpdma_state *d = (struct gpdma_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > sizeof(d->regs)) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)d->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool gpdma_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct gpdma_state *d = (struct gpdma_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > sizeof(d->regs)) return MM_FALSE;
    memcpy((mm_u8 *)d->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool flash_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct flash_state *f = (struct flash_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FLASH_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)f->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool flash_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct flash_state *f = (struct flash_state *)opaque;
    mm_u32 cr_off;
    mm_u32 sr_off;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FLASH_SIZE) return MM_FALSE;

    if (offset == FLASH_NSKEYR || offset == FLASH_SECKEYR) {
        flash_handle_key(offset, value);
        return MM_TRUE;
    }

    if (offset == FLASH_NSCCR || offset == FLASH_SECCCR) {
        sr_off = (offset == FLASH_NSCCR) ? FLASH_NSSR : FLASH_SECSR;
        if ((value & (1u << 16)) != 0u) {
            flash_clear_eop(sr_off);
        }
        return MM_TRUE;
    }

    if (offset == FLASH_NSCR || offset == FLASH_SECCR) {
        cr_off = offset;
        sr_off = (offset == FLASH_NSCR) ? FLASH_NSSR : FLASH_SECSR;
        if (!flash_is_unlocked(cr_off) && (value & FLASH_CR_LOCK) == 0u) {
            return MM_TRUE;
        }
        memcpy((mm_u8 *)f->regs + offset, &value, size_bytes);
        if ((value & FLASH_CR_LOCK) != 0u) {
            f->regs[cr_off / 4u] |= FLASH_CR_LOCK;
        }
        if ((value & FLASH_CR_STRT) != 0u) {
            flash_apply_erase(cr_off, sr_off);
            f->regs[cr_off / 4u] &= ~FLASH_CR_STRT;
        }
        return MM_TRUE;
    }

    memcpy((mm_u8 *)f->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool rng_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rng_state *r = ((struct rng_state *)((void **)opaque)[0]);
    mm_bool secure_alias = ((void **)opaque)[1] != 0;
    struct rcc_state *rcc = (struct rcc_state *)((void **)opaque)[2];
    struct simple_blk *tzsc = (struct simple_blk *)((void **)opaque)[3];

    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RNG_SIZE) return MM_FALSE;

    if (!secure_alias && rng_requires_secure(tzsc)) {
        *value_out = 0;
        return MM_TRUE;
    }
    if (!rng_clock_enabled(rcc)) {
        *value_out = 0;
        return MM_TRUE;
    }

    if (offset == RNG_SR_OFFSET) {
        if (!r->dr_valid) {
            rng_fill(r);
        }
        *value_out = r->regs[RNG_SR_OFFSET / 4];
        return MM_TRUE;
    }
    if (offset == RNG_DR_OFFSET) {
        if (!r->dr_valid) {
            rng_fill(r);
        }
        *value_out = r->dr;
        r->dr_valid = MM_FALSE;
        r->regs[RNG_SR_OFFSET / 4] &= ~1u;
        return MM_TRUE;
    }

    memcpy(value_out, (mm_u8 *)r->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool rng_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rng_state *r = ((struct rng_state *)((void **)opaque)[0]);
    mm_bool secure_alias = ((void **)opaque)[1] != 0;
    struct rcc_state *rcc = (struct rcc_state *)((void **)opaque)[2];
    struct simple_blk *tzsc = (struct simple_blk *)((void **)opaque)[3];

    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RNG_SIZE) return MM_FALSE;

    if (!secure_alias && rng_requires_secure(tzsc)) {
        return MM_TRUE;
    }
    if (!rng_clock_enabled(rcc)) {
        return MM_TRUE;
    }

    if (offset == RNG_SR_OFFSET || offset == RNG_DR_OFFSET) {
        return MM_TRUE;
    }

    memcpy((mm_u8 *)r->regs + offset, &value, size_bytes);
    if (offset == RNG_CR_OFFSET) {
        if ((value & (1u << 2)) == 0u) {
            r->dr_valid = MM_FALSE;
            r->regs[RNG_SR_OFFSET / 4] &= ~1u;
        }
        if ((value & (1u << 3)) != 0u) {
            if (g_rng_nvic != 0 && (r->regs[RNG_SR_OFFSET / 4] & 1u) != 0u) {
                mm_nvic_set_pending(g_rng_nvic, (mm_u32)RNG_IRQ, MM_TRUE);
            }
        }
    }
    return MM_TRUE;
}

static mm_bool wwdg_clock_enabled(void)
{
    return ((rcc.regs[0x9c / 4] >> 11) & 1u) != 0u;
}

static int exti_line_bank(int line)
{
    mm_u32 reg;
    mm_u32 shift;
    mm_u32 val;
    if (line < 0 || line > 15) return -1;
    reg = exti.regs[(EXTI_EXTICR1 + (mm_u32)(line / 4) * 4u) / 4u];
    shift = (mm_u32)(line % 4) * 8u;
    val = (reg >> shift) & 0xFFu;
    if (val > 8u) return -1;
    return (int)val;
}

static void exti_raise_irq(int line)
{
    if (g_exti_nvic != 0) {
        mm_nvic_set_pending(g_exti_nvic, (mm_u32)(11 + line), MM_TRUE);
    }
}

static void exti_set_pending_rise(int line)
{
    exti.regs[EXTI_RPR1 / 4u] |= (1u << line);
    if ((exti.regs[EXTI_IMR1 / 4u] & (1u << line)) != 0u) {
        exti_raise_irq(line);
    }
}

static void exti_set_pending_fall(int line)
{
    exti.regs[EXTI_FPR1 / 4u] |= (1u << line);
    if ((exti.regs[EXTI_IMR1 / 4u] & (1u << line)) != 0u) {
        exti_raise_irq(line);
    }
}

static void exti_gpio_update(int bank, mm_u32 old_level, mm_u32 new_level)
{
    int line;
    mm_u32 changed = old_level ^ new_level;
    if (changed == 0u) return;
    for (line = 0; line < 16; ++line) {
        mm_u32 mask = (1u << line);
        if ((changed & mask) == 0u) continue;
        if (exti_line_bank(line) != bank) continue;
        if ((new_level & mask) != 0u) {
            if ((exti.regs[EXTI_RTSR1 / 4u] & mask) != 0u) {
                exti_set_pending_rise(line);
            }
        } else {
            if ((exti.regs[EXTI_FTSR1 / 4u] & mask) != 0u) {
                exti_set_pending_fall(line);
            }
        }
    }
}

static mm_bool exti_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct exti_state *e = (struct exti_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > EXTI_SIZE) return MM_FALSE;
    if (offset == EXTI_SWIER1) {
        *value_out = 0;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)e->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool exti_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct exti_state *e = (struct exti_state *)opaque;
    int line;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > EXTI_SIZE) return MM_FALSE;
    if (offset == EXTI_SWIER1) {
        mm_u32 v = value & 0xFFFFu;
        for (line = 0; line < 16; ++line) {
            if ((v & (1u << line)) != 0u) {
                exti_set_pending_rise(line);
            }
        }
        return MM_TRUE;
    }
    if (offset == EXTI_RPR1) {
        e->regs[offset / 4u] &= ~value;
        return MM_TRUE;
    }
    if (offset == EXTI_FPR1) {
        e->regs[offset / 4u] &= ~value;
        return MM_TRUE;
    }
    memcpy((mm_u8 *)e->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool iwdg_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct iwdg_state *w = (struct iwdg_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > IWDG_SIZE) return MM_FALSE;
    if (offset == IWDG_KR) {
        *value_out = 0;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)w->regs + offset, size_bytes);
    return MM_TRUE;
}

static void iwdg_apply_key(struct iwdg_state *w, mm_u32 key)
{
    if (key == 0x5555u) {
        w->write_access = MM_TRUE;
    } else if (key == 0xAAAAu) {
        w->counter = w->regs[IWDG_RLR / 4u] & 0x0FFFu;
    } else if (key == 0xCCCCu) {
        w->running = MM_TRUE;
        w->write_access = MM_FALSE;
        w->counter = w->regs[IWDG_RLR / 4u] & 0x0FFFu;
    }
}

static mm_bool iwdg_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct iwdg_state *w = (struct iwdg_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > IWDG_SIZE) return MM_FALSE;
    if (offset == IWDG_KR) {
        iwdg_apply_key(w, value & 0xFFFFu);
        return MM_TRUE;
    }
    if (!w->write_access) {
        return MM_TRUE;
    }
    if (offset == IWDG_PR) {
        w->regs[IWDG_PR / 4u] = value & 0x7u;
        return MM_TRUE;
    }
    if (offset == IWDG_RLR) {
        w->regs[IWDG_RLR / 4u] = value & 0x0FFFu;
        return MM_TRUE;
    }
    if (offset == IWDG_WINR) {
        w->regs[IWDG_WINR / 4u] = value & 0x0FFFu;
        return MM_TRUE;
    }
    if (offset == IWDG_EWCR) {
        w->regs[IWDG_EWCR / 4u] = value;
        return MM_TRUE;
    }
    return MM_TRUE;
}

static mm_bool wwdg_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct wwdg_state *w = (struct wwdg_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > WWDG_SIZE) return MM_FALSE;
    if (!wwdg_clock_enabled()) {
        *value_out = 0;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)w->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool wwdg_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct wwdg_state *w = (struct wwdg_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > WWDG_SIZE) return MM_FALSE;
    if (!wwdg_clock_enabled()) {
        return MM_TRUE;
    }
    if (offset == WWDG_CR) {
        w->regs[WWDG_CR / 4u] = value & 0xFFu;
        w->counter = w->regs[WWDG_CR / 4u] & 0x7Fu;
        return MM_TRUE;
    }
    if (offset == WWDG_CFR) {
        w->regs[WWDG_CFR / 4u] = value;
        return MM_TRUE;
    }
    if (offset == WWDG_SR) {
        w->regs[WWDG_SR / 4u] &= ~value;
        return MM_TRUE;
    }
    memcpy((mm_u8 *)w->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

mm_u32 *mm_stm32h563_rcc_regs(void)
{
    return rcc.regs;
}

mm_u64 mm_stm32h563_cpu_hz(void)
{
    return rcc.cpu_hz;
}

void mm_stm32h563_exti_set_nvic(struct mm_nvic *nvic)
{
    g_exti_nvic = nvic;
    g_wdg_nvic = nvic;
}

void mm_stm32h563_watchdog_tick(mm_u64 cycles)
{
    static const mm_u32 iwdg_presc_div[8] = { 4u, 8u, 16u, 32u, 64u, 128u, 256u, 256u };
    mm_u64 cpu_hz = mm_stm32h563_cpu_hz();

    if (wwdg_clock_enabled() && wwdg.counter != 0u && (wwdg.regs[WWDG_CR / 4u] & 0x80u) != 0u) {
        mm_u32 wdgtb = (wwdg.regs[WWDG_CFR / 4u] >> 11) & 0x7u;
        mm_u64 step = 4096u * (mm_u64)(1u << wdgtb);
        wwdg.accum += cycles;
        while (wwdg.accum >= step) {
            wwdg.accum -= step;
            if (wwdg.counter > 0u) {
                wwdg.counter--;
                wwdg.regs[WWDG_CR / 4u] = (wwdg.regs[WWDG_CR / 4u] & ~0x7Fu) | (wwdg.counter & 0x7Fu);
                if (wwdg.counter == 0x40u && (wwdg.regs[WWDG_CFR / 4u] & (1u << 9)) != 0u) {
                    wwdg.regs[WWDG_SR / 4u] |= 1u;
                    if (g_wdg_nvic != 0) {
                        mm_nvic_set_pending(g_wdg_nvic, 0u, MM_TRUE);
                    }
                }
                if (wwdg.counter == 0x3Fu) {
                    mm_system_request_reset();
                    break;
                }
            }
        }
    }

    if (iwdg.running) {
        mm_u32 pr = iwdg.regs[IWDG_PR / 4u] & 0x7u;
        mm_u64 ticks_per_sec;
        mm_u64 cycles_per_tick;
        mm_u64 lsi = 32000u;
        mm_u32 div = iwdg_presc_div[pr];
        ticks_per_sec = lsi / (mm_u64)div;
        if (ticks_per_sec == 0) ticks_per_sec = 1;
        if (cpu_hz == 0) cpu_hz = 1;
        cycles_per_tick = cpu_hz / ticks_per_sec;
        if (cycles_per_tick == 0) cycles_per_tick = 1;
        iwdg.accum += cycles;
        while (iwdg.accum >= cycles_per_tick) {
            iwdg.accum -= cycles_per_tick;
            if (iwdg.counter > 0u) {
                iwdg.counter--;
            }
            if (iwdg.counter == 0u) {
                mm_system_request_reset();
                break;
            }
        }
    }
}

mm_bool mm_stm32h563_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;

    memset(&rcc, 0, sizeof(rcc));
    memset(&pwr, 0, sizeof(pwr));
    memset(&tzsc_s, 0, sizeof(tzsc_s));
    memset(&tzsc_ns, 0, sizeof(tzsc_ns));
    memset(&tzic_s, 0, sizeof(tzic_s));
    memset(&tzic_ns, 0, sizeof(tzic_ns));
    memset(&rng, 0, sizeof(rng));
    memset(&exti, 0, sizeof(exti));
    memset(&iwdg, 0, sizeof(iwdg));
    memset(&wwdg, 0, sizeof(wwdg));
    memset(&flash_ctl, 0, sizeof(flash_ctl));
    memset(gpio, 0, sizeof(gpio));
    mpcbb_init_defaults();
    memset(&gpdma1, 0, sizeof(gpdma1));
    memset(&gpdma2, 0, sizeof(gpdma2));
    rcc.regs[RCC_CR / 4] |= 1u;
    rcc_update_ready(&rcc);
    rcc_update_sysclk(&rcc);
    rng.regs[RNG_CR_OFFSET / 4] = 0x00871f00u;
    rng.regs[RNG_HTCR_OFFSET / 4] = 0x000072acu;
    flash_ctl.regs[FLASH_ACR / 4] = 0x00000013u;
    flash_ctl.regs[FLASH_NSCR / 4] = 0x00000001u;
    flash_ctl.regs[FLASH_SECCR / 4] = 0x00000001u;
    iwdg.regs[IWDG_RLR / 4] = 0x00000FFFu;
    iwdg.regs[IWDG_WINR / 4] = 0x00000FFFu;
    wwdg.regs[WWDG_CR / 4] = 0x0000007Fu;
    wwdg.regs[WWDG_CFR / 4] = 0x0000007Fu;
    wwdg.counter = 0x7Fu;
    exti.regs[EXTI_IMR1 / 4u] = 0xFFFE0000u;

    /* RCC */
    reg.base = RCC_BASE;
    reg.size = RCC_SIZE;
    reg.opaque = &rcc;
    reg.read = rcc_read;
    reg.write = rcc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* RCC secure alias */
    reg.base = RCC_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* PWR */
    reg.base = PWR_BASE;
    reg.size = PWR_SIZE;
    reg.opaque = &pwr;
    reg.read = pwr_read;
    reg.write = pwr_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* PWR secure alias */
    reg.base = PWR_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* FLASH controller */
    reg.base = FLASH_BASE;
    reg.size = FLASH_SIZE;
    reg.opaque = &flash_ctl;
    reg.read = flash_read;
    reg.write = flash_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* FLASH secure alias */
    reg.base = FLASH_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GTZC TZSC secure */
    reg.base = GTZC_TZSC_S_BASE;
    reg.size = GTZC_TZSC_SIZE;
    reg.opaque = &tzsc_s;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GTZC TZSC non-secure alias */
    reg.base = GTZC_TZSC_NS_BASE;
    reg.opaque = &tzsc_ns;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GTZC TZIC secure */
    reg.base = GTZC_TZIC_S_BASE;
    reg.size = GTZC_TZIC_SIZE;
    reg.opaque = &tzic_s;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GTZC TZIC non-secure alias */
    reg.base = GTZC_TZIC_NS_BASE;
    reg.opaque = &tzic_ns;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* MPCBB1..3 (non-secure and secure aliases) */
    reg.size = MPCBB_SIZE;
    reg.read = mpcbb_read;
    reg.write = mpcbb_write;
    reg.base = MPCBB1_BASE;
    reg.opaque = &mpcbb[0];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = MPCBB1_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = MPCBB2_BASE;
    reg.opaque = &mpcbb[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = MPCBB2_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = MPCBB3_BASE;
    reg.opaque = &mpcbb[2];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = MPCBB3_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* RNG (non-secure and secure aliases) */
    reg.base = RNG_BASE;
    reg.size = RNG_SIZE;
    rng_ctx[0][0] = &rng;
    rng_ctx[0][1] = (void *)0;
    rng_ctx[0][2] = &rcc;
    rng_ctx[0][3] = &tzsc_s;
    reg.opaque = rng_ctx[0];
    reg.read = rng_read;
    reg.write = rng_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = RNG_SEC_BASE;
    rng_ctx[1][0] = &rng;
    rng_ctx[1][1] = (void *)1;
    rng_ctx[1][2] = &rcc;
    rng_ctx[1][3] = &tzsc_s;
    reg.opaque = rng_ctx[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* EXTI (non-secure and secure aliases) */
    reg.base = EXTI_BASE;
    reg.size = EXTI_SIZE;
    reg.opaque = &exti;
    reg.read = exti_read;
    reg.write = exti_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = EXTI_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* IWDG (non-secure and secure aliases) */
    reg.base = IWDG_BASE;
    reg.size = IWDG_SIZE;
    reg.opaque = &iwdg;
    reg.read = iwdg_read;
    reg.write = iwdg_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = IWDG_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* WWDG (non-secure and secure aliases) */
    reg.base = WWDG_BASE;
    reg.size = WWDG_SIZE;
    reg.opaque = &wwdg;
    reg.read = wwdg_read;
    reg.write = wwdg_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = WWDG_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GPDMA1/GPDMA2 (non-secure and secure aliases) */
    reg.size = 0x1000u;
    reg.read = gpdma_read;
    reg.write = gpdma_write;
    reg.base = 0x40020000u; /* GPDMA1 */
    reg.opaque = &gpdma1;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = 0x50020000u; /* SEC_GPDMA1 */
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = 0x40021000u; /* GPDMA2 */
    reg.opaque = &gpdma2;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = 0x50021000u; /* SEC_GPDMA2 */
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GPIO A..I: NS alias 0x4202xxxx, Secure alias 0x5202xxxx */
    {
        mm_u32 base_ns = 0x42020000u;
        mm_u32 base_s  = 0x52020000u;
        int i;
        for (i = 0; i < 9; ++i) {
            /* NS */
            reg.base = base_ns + (mm_u32)(i * 0x400u);
            reg.size = 0x400u;
            gpio_ctx[i * 2][0] = &gpio[i];
            gpio_ctx[i * 2][1] = (void *)0; /* not secure alias */
            gpio_ctx[i * 2][2] = &rcc;
            gpio_ctx[i * 2][3] = (void *)(mm_uptr)i;
            reg.opaque = gpio_ctx[i * 2];
            reg.read = gpio_read;
            reg.write = gpio_write;
            if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

            /* Secure */
            reg.base = base_s + (mm_u32)(i * 0x400u);
            gpio_ctx[i * 2 + 1][0] = &gpio[i];
            gpio_ctx[i * 2 + 1][1] = (void *)1; /* secure alias */
            gpio_ctx[i * 2 + 1][2] = &rcc;
            gpio_ctx[i * 2 + 1][3] = (void *)(mm_uptr)i;
            reg.opaque = gpio_ctx[i * 2 + 1];
            if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
        }
    }

    if (!mm_stm32h563_usb_register_mmio(bus)) return MM_FALSE;
    return MM_TRUE;
}

void mm_stm32h563_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags)
{
    if (map == 0) {
        return;
    }
    flash_ctl.flash = flash;
    flash_ctl.flash_size = flash_size;
    flash_ctl.persist = persist;
    flash_ctl.flags = flags;
    flash_ctl.base_s = map->flash_base_s;
    flash_ctl.base_ns = map->flash_base_ns;
    mm_memmap_set_flash_writer(map, flash_write_cb, &flash_ctl);
}

void mm_stm32h563_rng_set_nvic(struct mm_nvic *nvic)
{
    g_rng_nvic = nvic;
}
