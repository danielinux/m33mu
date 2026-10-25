/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "nrf54lm20/nrf54lm20_mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/gpio.h"
#include "nrf_gpio.h"

#define FICR_BASE_NS 0x00FFC000u
#define FICR_SIZE    0x1000u

#define FICR_INFO_BASE 0x300u
#define FICR_INFO_RAM  0x328u
#define FICR_INFO_RRAM 0x32Cu

#define GPIO_P0_BASE_NS 0x4010A000u
#define GPIO_P1_BASE_NS 0x400D8200u
#define GPIO_P2_BASE_NS 0x40050400u
#define GPIO_P3_BASE_NS 0x400D8600u
#define GPIO_P0_BASE_S  0x5010A000u
#define GPIO_P1_BASE_S  0x500D8200u
#define GPIO_P2_BASE_S  0x50050400u
#define GPIO_P3_BASE_S  0x500D8600u
#define GPIO_SIZE       0x300u

#define GPIO_OUT     0x004u
#define GPIO_OUTSET  0x008u
#define GPIO_OUTCLR  0x00Cu
#define GPIO_IN      0x010u
#define GPIO_DIR     0x014u
#define GPIO_DIRSET  0x018u
#define GPIO_DIRCLR  0x01Cu
#define GPIO_LATCH   0x020u
#define GPIO_DETECTMODE 0x024u
#define GPIO_PIN_CNF0 0x200u

#define RRAMC_BASE_S 0x5004E000u
#define RRAMC_SIZE   0x1000u

#define RRAMC_READY    0x400u
#define RRAMC_READYNEXT 0x404u
#define RRAMC_CONFIG   0x500u
#define RRAMC_ERASEALL 0x540u

#define CLOCK_BASE_NS 0x4010E000u
#define CLOCK_BASE_S  0x5010E000u
#define CLOCK_SIZE    0x1000u

#define CLOCK_TASKS_XOSTART   0x000u
#define CLOCK_TASKS_XOSTOP    0x004u
#define CLOCK_TASKS_PLLSTART  0x008u
#define CLOCK_TASKS_PLLSTOP   0x00Cu
#define CLOCK_TASKS_LFCLKSTART 0x010u
#define CLOCK_TASKS_LFCLKSTOP  0x014u
#define CLOCK_TASKS_CAL       0x018u
#define CLOCK_TASKS_XOTUNE    0x01Cu
#define CLOCK_TASKS_XOTUNEABORT 0x020u
#define CLOCK_TASKS_XO24MSTART 0x024u
#define CLOCK_TASKS_XO24MSTOP  0x028u

#define CLOCK_EVENTS_XOSTARTED  0x100u
#define CLOCK_EVENTS_PLLSTARTED 0x104u
#define CLOCK_EVENTS_LFCLKSTARTED 0x108u
#define CLOCK_EVENTS_DONE       0x10Cu
#define CLOCK_EVENTS_XOTUNED    0x110u
#define CLOCK_EVENTS_XOTUNEERROR 0x114u
#define CLOCK_EVENTS_XOTUNEFAILED 0x118u
#define CLOCK_EVENTS_XO24MSTARTED 0x11Cu

#define CLOCK_INTEN    0x300u
#define CLOCK_INTENSET 0x304u
#define CLOCK_INTENCLR 0x308u
#define CLOCK_INTPEND  0x30Cu

#define CLOCK_XO_RUN   0x408u
#define CLOCK_XO_STAT  0x40Cu
#define CLOCK_PLL_RUN  0x428u
#define CLOCK_PLL_STAT 0x42Cu
#define CLOCK_LFCLK_SRC 0x440u
#define CLOCK_LFCLK_RUN 0x448u
#define CLOCK_LFCLK_STAT 0x44Cu
#define CLOCK_LFCLK_SRCCOPY 0x450u
#define CLOCK_PLL24M_STAT 0x46Cu

#define OSCILLATORS_BASE_NS 0x40120000u
#define OSCILLATORS_BASE_S  0x50120000u
#define OSCILLATORS_SIZE    0x1000u

#define OSC_PLL_FREQ        0x800u
#define OSC_PLL_CURRENTFREQ 0x804u

#define GRTC_BASE_NS 0x400E2000u
#define GRTC_BASE_S  0x500E2000u
#define GRTC_SIZE    0x1000u

#define GRTC_TASKS_CAPTURE0 0x000u
#define GRTC_TASKS_START    0x060u
#define GRTC_TASKS_STOP     0x064u
#define GRTC_TASKS_CLEAR    0x068u

#define GRTC_EVENTS_COMPARE0 0x100u

#define GRTC_INTEN0    0x300u
#define GRTC_INTENSET0 0x304u
#define GRTC_INTENCLR0 0x308u
#define GRTC_INTPEND0  0x30Cu
#define GRTC_INT_GROUP_STRIDE 0x10u

#define GRTC_MODE      0x510u
#define GRTC_CC_BASE   0x520u
#define GRTC_CC_STRIDE 0x10u
#define GRTC_CC_CCL    0x000u
#define GRTC_CC_CCH    0x004u
#define GRTC_CC_CCADD  0x008u
#define GRTC_CC_CCEN   0x00Cu

#define GRTC_INTERVAL  0x6A8u
#define GRTC_STATUS_LFTIMER 0x6B0u
#define GRTC_STATUS_PWM     0x6B4u
#define GRTC_STATUS_CLKOUT  0x6B8u

#define GRTC_SYSCOUNTER_BASE   0x720u
#define GRTC_SYSCOUNTER_STRIDE 0x10u
#define GRTC_SYSCOUNTERL 0x000u
#define GRTC_SYSCOUNTERH 0x004u
#define GRTC_SYSCOUNTER_ACTIVE 0x008u

#define GRTC_CC_COUNT 12u
#define GRTC_SYSCOUNTER_COUNT 4u
#define GRTC_INT_GROUPS 4u
#define GRTC_IRQ_BASE 226

#define CLOCK_EVENT_XOSTARTED_BIT    (1u << 0)
#define CLOCK_EVENT_PLLSTARTED_BIT   (1u << 1)
#define CLOCK_EVENT_LFCLKSTARTED_BIT (1u << 2)
#define CLOCK_EVENT_DONE_BIT         (1u << 3)
#define CLOCK_EVENT_XOTUNED_BIT      (1u << 4)
#define CLOCK_EVENT_XOTUNEERROR_BIT  (1u << 5)
#define CLOCK_EVENT_XOTUNEFAILED_BIT (1u << 6)
#define CLOCK_EVENT_XO24MSTARTED_BIT (1u << 7)

struct ficr_state {
    mm_u32 regs[FICR_SIZE / 4];
};

struct rramc_state {
    mm_u32 regs[RRAMC_SIZE / 4];
    mm_u8 *flash;
    mm_u32 flash_size;
    const struct mm_flash_persist *persist;
    mm_u32 flags;
    mm_u32 base_s;
    mm_u32 base_ns;
};

struct clock_state {
    mm_u32 regs[CLOCK_SIZE / 4];
    mm_u32 events_mask;
    mm_u32 inten;
    mm_u32 lfclk_src;
    mm_bool xo_running;
    mm_bool pll_running;
    mm_bool lfclk_running;
    mm_bool xo_triggered;
    mm_bool pll_triggered;
    mm_bool lfclk_triggered;
    mm_bool pll24m_running;
};

struct osc_state {
    mm_u32 regs[OSCILLATORS_SIZE / 4];
};

struct grtc_state {
    mm_u32 regs[GRTC_SIZE / 4];
    mm_u64 syscounter;
    mm_u64 cc[GRTC_CC_COUNT];
    mm_u32 cc_active_mask;
    mm_u32 events_compare_mask;
    mm_u32 inten[GRTC_INT_GROUPS];
    mm_u64 accum_cycles;
    mm_bool running;
};

static struct nrf_gpio_bank gpio_banks[4];
static struct ficr_state ficr_state;
static struct rramc_state rramc_state;
static struct clock_state clock_state;
static struct osc_state osc_state;
static struct grtc_state grtc_state;
static struct mm_nvic *g_nrf54lm20_nvic = 0;

static mm_u32 read_slice(mm_u32 reg, mm_u32 offset_in_reg, mm_u32 size_bytes)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    return (reg >> shift) & mask;
}

static mm_u32 apply_write(mm_u32 cur, mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    mm_u32 shifted = (value & mask) << shift;
    return (cur & ~(mask << shift)) | shifted;
}

static void clock_update_irq(struct clock_state *clk)
{
    mm_u32 pending;
    if (clk == 0 || g_nrf54lm20_nvic == 0) return;
    pending = clk->events_mask & clk->inten;
    if (pending != 0u) {
        mm_nvic_set_pending(g_nrf54lm20_nvic, 270u, MM_TRUE);
    }
}

static void clock_set_event(struct clock_state *clk, mm_u32 offset, mm_u32 bit)
{
    if (clk == 0) return;
    clk->regs[offset / 4] = 1u;
    clk->events_mask |= bit;
    clock_update_irq(clk);
}

static mm_u32 clock_event_bit_for_offset(mm_u32 offset)
{
    switch (offset) {
    case CLOCK_EVENTS_XOSTARTED:
        return CLOCK_EVENT_XOSTARTED_BIT;
    case CLOCK_EVENTS_PLLSTARTED:
        return CLOCK_EVENT_PLLSTARTED_BIT;
    case CLOCK_EVENTS_LFCLKSTARTED:
        return CLOCK_EVENT_LFCLKSTARTED_BIT;
    case CLOCK_EVENTS_DONE:
        return CLOCK_EVENT_DONE_BIT;
    case CLOCK_EVENTS_XOTUNED:
        return CLOCK_EVENT_XOTUNED_BIT;
    case CLOCK_EVENTS_XOTUNEERROR:
        return CLOCK_EVENT_XOTUNEERROR_BIT;
    case CLOCK_EVENTS_XOTUNEFAILED:
        return CLOCK_EVENT_XOTUNEFAILED_BIT;
    case CLOCK_EVENTS_XO24MSTARTED:
        return CLOCK_EVENT_XO24MSTARTED_BIT;
    default:
        return 0u;
    }
}

static void grtc_update_irq(struct grtc_state *grtc)
{
    int group;
    if (grtc == 0 || g_nrf54lm20_nvic == 0) return;
    for (group = 0; group < (int)GRTC_INT_GROUPS; ++group) {
        mm_u32 pending = grtc->events_compare_mask & grtc->inten[group];
        if (pending != 0u) {
            mm_nvic_set_pending(g_nrf54lm20_nvic, (mm_u32)(GRTC_IRQ_BASE + group), MM_TRUE);
        }
    }
}

static mm_bool grtc_set_compare_event(struct grtc_state *grtc, mm_u32 ch)
{
    mm_u32 bit;
    if (grtc == 0 || ch >= GRTC_CC_COUNT) return MM_FALSE;
    bit = 1u << ch;
    if ((grtc->events_compare_mask & bit) != 0u) return MM_FALSE;
    grtc->events_compare_mask |= bit;
    grtc->regs[(GRTC_EVENTS_COMPARE0 + ch * 4u) / 4] = 1u;
    grtc_update_irq(grtc);
    return MM_TRUE;
}

static void grtc_sync_cc_regs(struct grtc_state *grtc, mm_u32 ch)
{
    mm_u64 value;
    mm_u32 base;
    if (grtc == 0 || ch >= GRTC_CC_COUNT) return;
    value = grtc->cc[ch];
    base = GRTC_CC_BASE + ch * GRTC_CC_STRIDE;
    grtc->regs[(base + GRTC_CC_CCL) / 4] = (mm_u32)(value & 0xFFFFFFFFu);
    grtc->regs[(base + GRTC_CC_CCH) / 4] = (mm_u32)((value >> 32) & 0xFFFFFFFFu);
}

static mm_bool nrf54lm20_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= 4) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0u) {
        snprintf(name_out, name_len, "P%d", bank);
    }
    if (pins_out != 0) {
        *pins_out = 32;
    }
    return MM_TRUE;
}

static mm_bool nrf_gpio_set_external_input_cb(void *opaque, int bank, int pin, mm_bool level,
                                              mm_u64 late_cycles)
{
    (void)opaque;
    (void)late_cycles;
    if (bank < 0 || bank >= (int)(sizeof(gpio_banks) / sizeof(gpio_banks[0]))) return MM_FALSE;
    return nrf_gpio_set_external(&gpio_banks[bank], pin, level);
}

static mm_u32 nrf_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= 4) return 0;
    return gpio_banks[bank].regs[GPIO_OUT / 4];
}

static mm_u32 nrf_gpio_bank_read_moder(void *opaque, int bank)
{
    mm_u32 moder = 0;
    mm_u32 dir;
    int pin;
    (void)opaque;
    if (bank < 0 || bank >= 4) return 0;
    dir = gpio_banks[bank].regs[GPIO_DIR / 4];
    for (pin = 0; pin < 16; ++pin) {
        mm_u32 mode = ((dir >> pin) & 1u) ? 1u : 0u;
        moder |= (mode << (pin * 2));
    }
    return moder;
}

static mm_bool nrf_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    (void)bank;
    return MM_TRUE;
}

static mm_u32 nrf_gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    (void)bank;
    return 0u;
}

static mm_bool ficr_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct ficr_state *ficr = (struct ficr_state *)opaque;
    if (ficr == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FICR_SIZE) return MM_FALSE;
    *value_out = read_slice(ficr->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool ficr_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct ficr_state *ficr = (struct ficr_state *)opaque;
    if (ficr == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FICR_SIZE) return MM_FALSE;
    ficr->regs[offset / 4] = apply_write(ficr->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool rramc_write_cb(void *opaque,
                              enum mm_sec_state sec,
                              mm_u32 addr,
                              mm_u32 size_bytes,
                              mm_u32 value)
{
    struct rramc_state *rramc = (struct rramc_state *)opaque;
    mm_u32 base;
    mm_u32 offset;
    mm_u8 *flash;
    (void)sec;

    if (rramc == 0 || rramc->flash == 0) return MM_FALSE;
    if ((rramc->regs[RRAMC_CONFIG / 4] & 1u) == 0u) {
        return MM_FALSE;
    }

    base = (addr >= rramc->base_s) ? rramc->base_s : rramc->base_ns;
    if (addr < base) return MM_FALSE;
    offset = addr - base;
    if (offset + size_bytes > rramc->flash_size) return MM_FALSE;

    flash = rramc->flash;
    if (size_bytes == 4u) {
        mm_u32 cur = (mm_u32)flash[offset] |
                     ((mm_u32)flash[offset + 1u] << 8) |
                     ((mm_u32)flash[offset + 2u] << 16) |
                     ((mm_u32)flash[offset + 3u] << 24);
        mm_u32 next = cur & value;
        flash[offset] = (mm_u8)(next & 0xffu);
        flash[offset + 1u] = (mm_u8)((next >> 8) & 0xffu);
        flash[offset + 2u] = (mm_u8)((next >> 16) & 0xffu);
        flash[offset + 3u] = (mm_u8)((next >> 24) & 0xffu);
    } else if (size_bytes == 2u) {
        mm_u32 cur = (mm_u32)flash[offset] | ((mm_u32)flash[offset + 1u] << 8);
        mm_u32 next = cur & value;
        flash[offset] = (mm_u8)(next & 0xffu);
        flash[offset + 1u] = (mm_u8)((next >> 8) & 0xffu);
    } else if (size_bytes == 1u) {
        flash[offset] &= (mm_u8)(value & 0xffu);
    } else {
        return MM_FALSE;
    }

    if (rramc->persist != 0 && rramc->persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)rramc->persist, offset, size_bytes);
    }
    return MM_TRUE;
}

static void rramc_erase_all(struct rramc_state *rramc)
{
    if (rramc == 0 || rramc->flash == 0) return;
    memset(rramc->flash, 0xFF, rramc->flash_size);
    if (rramc->persist != 0 && rramc->persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)rramc->persist,
                               0u,
                               rramc->flash_size);
    }
}

static mm_bool rramc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rramc_state *rramc = (struct rramc_state *)opaque;
    if (rramc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RRAMC_SIZE) return MM_FALSE;

    if (offset == RRAMC_READY && size_bytes == 4) {
        *value_out = 1u;
        return MM_TRUE;
    }
    if (offset == RRAMC_READYNEXT && size_bytes == 4) {
        *value_out = 1u;
        return MM_TRUE;
    }

    *value_out = read_slice(rramc->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool rramc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rramc_state *rramc = (struct rramc_state *)opaque;
    if (rramc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RRAMC_SIZE) return MM_FALSE;

    if (offset == RRAMC_ERASEALL && size_bytes == 4) {
        if ((rramc->regs[RRAMC_CONFIG / 4] & 1u) != 0u && (value & 1u) != 0u) {
            rramc_erase_all(rramc);
        }
        return MM_TRUE;
    }

    rramc->regs[offset / 4] = apply_write(rramc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool clock_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct clock_state *clk = (struct clock_state *)opaque;
    mm_u32 reg = 0;

    if (clk == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CLOCK_SIZE) return MM_FALSE;

    if (offset == CLOCK_INTEN || offset == CLOCK_INTENSET || offset == CLOCK_INTENCLR) {
        reg = clk->inten;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_INTPEND) {
        reg = clk->events_mask & clk->inten;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_XO_RUN) {
        reg = clk->xo_triggered ? 1u : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_XO_STAT) {
        reg = clk->xo_running ? (1u << 16) : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_PLL_RUN) {
        reg = clk->pll_triggered ? 1u : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_PLL_STAT) {
        reg = clk->pll_running ? (1u << 16) : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLK_SRC) {
        reg = clk->lfclk_src & 0x3u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLK_RUN) {
        reg = clk->lfclk_triggered ? 1u : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLK_STAT) {
        reg = (clk->lfclk_src & 0x3u) | (clk->lfclk_running ? (1u << 16) : 0u);
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_PLL24M_STAT) {
        reg = clk->pll24m_running ? (1u << 16) : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    reg = clk->regs[offset / 4];
    *value_out = read_slice(reg, offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool clock_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct clock_state *clk = (struct clock_state *)opaque;
    mm_u32 bit;

    if (clk == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CLOCK_SIZE) return MM_FALSE;

    if (offset == CLOCK_TASKS_XOSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->xo_triggered = MM_TRUE;
            clk->xo_running = MM_TRUE;
            clock_set_event(clk, CLOCK_EVENTS_XOSTARTED, CLOCK_EVENT_XOSTARTED_BIT);
            clock_set_event(clk, CLOCK_EVENTS_XOTUNED, CLOCK_EVENT_XOTUNED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XOSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->xo_running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_PLLSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->pll_triggered = MM_TRUE;
            clk->pll_running = MM_TRUE;
            clock_set_event(clk, CLOCK_EVENTS_PLLSTARTED, CLOCK_EVENT_PLLSTARTED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_PLLSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->pll_running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_LFCLKSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->lfclk_triggered = MM_TRUE;
            clk->lfclk_src = clk->regs[CLOCK_LFCLK_SRC / 4] & 0x3u;
            clk->regs[CLOCK_LFCLK_SRCCOPY / 4] = clk->lfclk_src;
            clk->lfclk_running = MM_TRUE;
            clock_set_event(clk, CLOCK_EVENTS_LFCLKSTARTED, CLOCK_EVENT_LFCLKSTARTED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_LFCLKSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->lfclk_running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_CAL && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clock_set_event(clk, CLOCK_EVENTS_DONE, CLOCK_EVENT_DONE_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XOTUNE && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clock_set_event(clk, CLOCK_EVENTS_XOTUNED, CLOCK_EVENT_XOTUNED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XOTUNEABORT && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clock_set_event(clk, CLOCK_EVENTS_XOTUNEFAILED, CLOCK_EVENT_XOTUNEFAILED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XO24MSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->pll24m_running = MM_TRUE;
            clock_set_event(clk, CLOCK_EVENTS_XO24MSTARTED, CLOCK_EVENT_XO24MSTARTED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XO24MSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->pll24m_running = MM_FALSE;
        }
        return MM_TRUE;
    }

    bit = clock_event_bit_for_offset(offset);
    if (bit != 0u && size_bytes == 4) {
        if (value == 0u) {
            clk->events_mask &= ~bit;
            clk->regs[offset / 4] = 0u;
        } else {
            clk->events_mask |= bit;
            clk->regs[offset / 4] = value;
        }
        clock_update_irq(clk);
        return MM_TRUE;
    }

    if (offset == CLOCK_INTEN && size_bytes == 4) {
        clk->inten = value;
        clock_update_irq(clk);
        return MM_TRUE;
    }
    if (offset == CLOCK_INTENSET && size_bytes == 4) {
        clk->inten |= value;
        clock_update_irq(clk);
        return MM_TRUE;
    }
    if (offset == CLOCK_INTENCLR && size_bytes == 4) {
        clk->inten &= ~value;
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLK_SRC && size_bytes == 4) {
        clk->lfclk_src = value & 0x3u;
        clk->regs[offset / 4] = clk->lfclk_src;
        return MM_TRUE;
    }

    clk->regs[offset / 4] = apply_write(clk->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool oscillators_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct osc_state *osc = (struct osc_state *)opaque;
    if (osc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > OSCILLATORS_SIZE) return MM_FALSE;
    *value_out = read_slice(osc->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool oscillators_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct osc_state *osc = (struct osc_state *)opaque;
    mm_u32 v;
    if (osc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > OSCILLATORS_SIZE) return MM_FALSE;

    if (offset == OSC_PLL_FREQ && size_bytes == 4) {
        v = value & 0x3u;
        osc->regs[OSC_PLL_FREQ / 4] = v;
        osc->regs[OSC_PLL_CURRENTFREQ / 4] = v;
        return MM_TRUE;
    }
    if (offset == OSC_PLL_CURRENTFREQ) {
        return MM_TRUE;
    }

    osc->regs[offset / 4] = apply_write(osc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool grtc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct grtc_state *grtc = (struct grtc_state *)opaque;
    mm_u32 reg = 0;
    mm_u32 rel;
    mm_u32 idx;
    mm_u32 sub;
    mm_u32 ch;

    if (grtc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GRTC_SIZE) return MM_FALSE;

    if (offset >= GRTC_INTEN0 && offset < (GRTC_INTEN0 + GRTC_INT_GROUP_STRIDE * GRTC_INT_GROUPS)) {
        idx = (offset - GRTC_INTEN0) / GRTC_INT_GROUP_STRIDE;
        sub = (offset - GRTC_INTEN0) % GRTC_INT_GROUP_STRIDE;
        if (sub == 0u || sub == 4u || sub == 8u) {
            reg = grtc->inten[idx];
        } else if (sub == 0x0Cu) {
            reg = grtc->events_compare_mask & grtc->inten[idx];
        } else {
            reg = grtc->regs[offset / 4];
        }
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    if (offset >= GRTC_EVENTS_COMPARE0 && offset < (GRTC_EVENTS_COMPARE0 + GRTC_CC_COUNT * 4u)) {
        ch = (offset - GRTC_EVENTS_COMPARE0) / 4u;
        if (grtc->running) {
            mm_u32 active = (grtc->cc_active_mask >> ch) & 1u;
            mm_u32 set = (grtc->events_compare_mask >> ch) & 1u;
            if (active && !set) {
                if (grtc->syscounter < grtc->cc[ch]) {
                    grtc->syscounter = grtc->cc[ch];
                }
                (void)grtc_set_compare_event(grtc, ch);
            }
        }
        reg = (grtc->events_compare_mask >> ch) & 1u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    if (offset >= GRTC_CC_BASE && offset < (GRTC_CC_BASE + GRTC_CC_COUNT * GRTC_CC_STRIDE)) {
        rel = offset - GRTC_CC_BASE;
        ch = rel / GRTC_CC_STRIDE;
        sub = rel % GRTC_CC_STRIDE;
        if (sub == GRTC_CC_CCL) {
            reg = (mm_u32)(grtc->cc[ch] & 0xFFFFFFFFu);
        } else if (sub == GRTC_CC_CCH) {
            reg = (mm_u32)((grtc->cc[ch] >> 32) & 0xFFFFFFFFu);
        } else if (sub == GRTC_CC_CCEN) {
            mm_u32 active = (grtc->cc_active_mask >> ch) & 1u;
            mm_u32 past = (active && grtc->syscounter >= grtc->cc[ch]) ? 1u : 0u;
            reg = active | (past << 1);
        } else {
            reg = 0u;
        }
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    if (offset >= GRTC_SYSCOUNTER_BASE &&
        offset < (GRTC_SYSCOUNTER_BASE + GRTC_SYSCOUNTER_COUNT * GRTC_SYSCOUNTER_STRIDE)) {
        rel = offset - GRTC_SYSCOUNTER_BASE;
        sub = rel % GRTC_SYSCOUNTER_STRIDE;
        if (sub == GRTC_SYSCOUNTERL) {
            if (grtc->running) {
                grtc->syscounter += 1000u;
            }
            reg = (mm_u32)(grtc->syscounter & 0xFFFFFFFFu);
        } else if (sub == GRTC_SYSCOUNTERH) {
            reg = (mm_u32)((grtc->syscounter >> 32) & 0x000FFFFFu);
            reg |= (1u << 29);
        } else if (sub == GRTC_SYSCOUNTER_ACTIVE) {
            reg = grtc->regs[offset / 4] & 1u;
        } else {
            reg = 0u;
        }
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    if (offset == GRTC_STATUS_LFTIMER || offset == GRTC_STATUS_PWM || offset == GRTC_STATUS_CLKOUT) {
        reg = 1u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    reg = grtc->regs[offset / 4];
    *value_out = read_slice(reg, offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool grtc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct grtc_state *grtc = (struct grtc_state *)opaque;
    mm_u32 rel;
    mm_u32 idx;
    mm_u32 sub;
    mm_u32 ch;
    mm_u64 base;
    mm_u64 add;

    if (grtc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GRTC_SIZE) return MM_FALSE;

    if (offset < (GRTC_TASKS_CAPTURE0 + GRTC_CC_COUNT * 4u)) {
        ch = (offset - GRTC_TASKS_CAPTURE0) / 4u;
        if (size_bytes == 4 && (value & 1u) != 0u) {
            grtc->cc[ch] = grtc->syscounter;
            grtc_sync_cc_regs(grtc, ch);
        }
        return MM_TRUE;
    }
    if (offset == GRTC_TASKS_START && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            grtc->running = MM_TRUE;
            grtc->regs[GRTC_MODE / 4] |= (1u << 1);
        }
        return MM_TRUE;
    }
    if (offset == GRTC_TASKS_STOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            grtc->running = MM_FALSE;
            grtc->regs[GRTC_MODE / 4] &= ~(1u << 1);
        }
        return MM_TRUE;
    }
    if (offset == GRTC_TASKS_CLEAR && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            grtc->syscounter = 0u;
        }
        return MM_TRUE;
    }

    if (offset >= GRTC_EVENTS_COMPARE0 && offset < (GRTC_EVENTS_COMPARE0 + GRTC_CC_COUNT * 4u)) {
        ch = (offset - GRTC_EVENTS_COMPARE0) / 4u;
        if (size_bytes == 4) {
            if (value == 0u) {
                grtc->events_compare_mask &= ~(1u << ch);
                grtc->regs[offset / 4] = 0u;
            } else {
                grtc->events_compare_mask |= (1u << ch);
                grtc->regs[offset / 4] = value;
            }
            grtc_update_irq(grtc);
        }
        return MM_TRUE;
    }

    if (offset >= GRTC_INTEN0 && offset < (GRTC_INTEN0 + GRTC_INT_GROUP_STRIDE * GRTC_INT_GROUPS)) {
        idx = (offset - GRTC_INTEN0) / GRTC_INT_GROUP_STRIDE;
        sub = (offset - GRTC_INTEN0) % GRTC_INT_GROUP_STRIDE;
        if (size_bytes == 4) {
            if (sub == 0u) {
                grtc->inten[idx] = value;
            } else if (sub == 4u) {
                grtc->inten[idx] |= value;
            } else if (sub == 8u) {
                grtc->inten[idx] &= ~value;
            }
            grtc_update_irq(grtc);
        }
        return MM_TRUE;
    }

    if (offset == GRTC_MODE && size_bytes == 4) {
        grtc->regs[GRTC_MODE / 4] = value;
        grtc->running = ((value & (1u << 1)) != 0u) ? MM_TRUE : MM_FALSE;
        return MM_TRUE;
    }

    if (offset >= GRTC_CC_BASE && offset < (GRTC_CC_BASE + GRTC_CC_COUNT * GRTC_CC_STRIDE)) {
        rel = offset - GRTC_CC_BASE;
        ch = rel / GRTC_CC_STRIDE;
        sub = rel % GRTC_CC_STRIDE;
        if (size_bytes == 4) {
            if (sub == GRTC_CC_CCL) {
                grtc->cc[ch] = (grtc->cc[ch] & 0xFFFFFFFF00000000ull) | value;
                grtc_sync_cc_regs(grtc, ch);
            } else if (sub == GRTC_CC_CCH) {
                grtc->cc[ch] = (grtc->cc[ch] & 0x00000000FFFFFFFFull) | ((mm_u64)value << 32);
                grtc_sync_cc_regs(grtc, ch);
            } else if (sub == GRTC_CC_CCADD) {
                add = (mm_u64)(value & 0x7FFFFFFFu);
                base = (value & 0x80000000u) ? grtc->cc[ch] : grtc->syscounter;
                grtc->cc[ch] = base + add;
                grtc_sync_cc_regs(grtc, ch);
            } else if (sub == GRTC_CC_CCEN) {
                if ((value & 1u) != 0u) {
                    grtc->cc_active_mask |= (1u << ch);
                } else {
                    grtc->cc_active_mask &= ~(1u << ch);
                }
                grtc->regs[offset / 4] = value & 1u;
            }
        }
        return MM_TRUE;
    }

    if (offset >= GRTC_SYSCOUNTER_BASE &&
        offset < (GRTC_SYSCOUNTER_BASE + GRTC_SYSCOUNTER_COUNT * GRTC_SYSCOUNTER_STRIDE)) {
        rel = offset - GRTC_SYSCOUNTER_BASE;
        sub = rel % GRTC_SYSCOUNTER_STRIDE;
        if (sub == GRTC_SYSCOUNTER_ACTIVE && size_bytes == 4) {
            grtc->regs[offset / 4] = value & 1u;
            return MM_TRUE;
        }
    }

    if (offset == GRTC_INTERVAL && size_bytes == 4) {
        grtc->regs[offset / 4] = value;
        return MM_TRUE;
    }

    grtc->regs[offset / 4] = apply_write(grtc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

mm_bool mm_nrf54lm20_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;

    {
        int i;
        for (i = 0; i < (int)(sizeof(gpio_banks) / sizeof(gpio_banks[0])); ++i) {
            nrf_gpio_bank_init(&gpio_banks[i], i, 32);
        }
    }
    memset(&ficr_state, 0xFF, sizeof(ficr_state));
    memset(&rramc_state, 0, sizeof(rramc_state));
    memset(&clock_state, 0, sizeof(clock_state));
    memset(&osc_state, 0, sizeof(osc_state));
    memset(&grtc_state, 0, sizeof(grtc_state));

    ficr_state.regs[FICR_INFO_RAM / 4] = 0x00000200u;
    ficr_state.regs[FICR_INFO_RRAM / 4] = 0x000007F4u;
    osc_state.regs[OSC_PLL_FREQ / 4] = 0x3u;
    osc_state.regs[OSC_PLL_CURRENTFREQ / 4] = 0x3u;
    grtc_state.regs[GRTC_STATUS_LFTIMER / 4] = 1u;
    grtc_state.regs[GRTC_STATUS_PWM / 4] = 1u;
    grtc_state.regs[GRTC_STATUS_CLKOUT / 4] = 1u;

    reg.base = FICR_BASE_NS;
    reg.size = FICR_SIZE;
    reg.opaque = &ficr_state;
    reg.read = ficr_read;
    reg.write = ficr_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO_P0_BASE_NS;
    reg.size = GPIO_SIZE;
    reg.opaque = &gpio_banks[0];
    reg.read = nrf_gpio_read;
    reg.write = nrf_gpio_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO_P0_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO_P1_BASE_NS;
    reg.opaque = &gpio_banks[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO_P1_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO_P2_BASE_NS;
    reg.opaque = &gpio_banks[2];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO_P2_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO_P3_BASE_NS;
    reg.opaque = &gpio_banks[3];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO_P3_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = RRAMC_BASE_S;
    reg.size = RRAMC_SIZE;
    reg.opaque = &rramc_state;
    reg.read = rramc_read;
    reg.write = rramc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = CLOCK_BASE_NS;
    reg.size = CLOCK_SIZE;
    reg.opaque = &clock_state;
    reg.read = clock_read;
    reg.write = clock_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = CLOCK_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = OSCILLATORS_BASE_NS;
    reg.size = OSCILLATORS_SIZE;
    reg.opaque = &osc_state;
    reg.read = oscillators_read;
    reg.write = oscillators_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = OSCILLATORS_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GRTC_BASE_NS;
    reg.size = GRTC_SIZE;
    reg.opaque = &grtc_state;
    reg.read = grtc_read;
    reg.write = grtc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GRTC_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    mm_gpio_bank_set_reader(nrf_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(nrf_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(nrf_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(nrf_gpio_bank_read_seccfgr, 0);

    return MM_TRUE;
}

void mm_nrf54lm20_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags)
{
    if (map == 0) return;
    rramc_state.flash = flash;
    rramc_state.flash_size = flash_size;
    rramc_state.persist = persist;
    rramc_state.flags = flags;
    rramc_state.base_s = map->flash_base_s;
    rramc_state.base_ns = map->flash_base_ns;
    mm_memmap_set_flash_writer(map, rramc_write_cb, &rramc_state);
}

mm_u64 mm_nrf54lm20_cpu_hz(void)
{
    /* SystemCoreClockUpdate uses OSCILLATORS->PLL.CURRENTFREQ (64/128 MHz). */
    mm_u32 freq = osc_state.regs[OSC_PLL_CURRENTFREQ / 4] & 0x3u;
    if (freq == 0x1u) return 128000000ull;
    if (freq == 0x3u) return 64000000ull;
    return 128000000ull;
}

void mm_nrf54lm20_mmio_reset(void)
{
    {
        int i;
        for (i = 0; i < (int)(sizeof(gpio_banks) / sizeof(gpio_banks[0])); ++i) {
            nrf_gpio_bank_init(&gpio_banks[i], i, 32);
        }
    }
    memset(&rramc_state.regs, 0, sizeof(rramc_state.regs));
    rramc_state.regs[RRAMC_READY / 4] = 1u;
    rramc_state.regs[RRAMC_READYNEXT / 4] = 1u;

    memset(&ficr_state, 0xFF, sizeof(ficr_state));
    ficr_state.regs[FICR_INFO_RAM / 4] = 0x00000200u;
    ficr_state.regs[FICR_INFO_RRAM / 4] = 0x000007F4u;

    memset(&clock_state, 0, sizeof(clock_state));
    memset(&osc_state, 0, sizeof(osc_state));
    memset(&grtc_state, 0, sizeof(grtc_state));
    osc_state.regs[OSC_PLL_FREQ / 4] = 0x3u;
    osc_state.regs[OSC_PLL_CURRENTFREQ / 4] = 0x3u;
    grtc_state.running = MM_TRUE;
    grtc_state.regs[GRTC_MODE / 4] |= (1u << 1);
    grtc_state.regs[GRTC_STATUS_LFTIMER / 4] = 1u;
    grtc_state.regs[GRTC_STATUS_PWM / 4] = 1u;
    grtc_state.regs[GRTC_STATUS_CLKOUT / 4] = 1u;

    mm_gpio_set_external_input_writer(nrf_gpio_set_external_input_cb, 0);
    mm_gpio_set_bank_info_reader(nrf54lm20_gpio_bank_info, 0);
}

void mm_nrf54lm20_mmio_set_nvic(struct mm_nvic *nvic)
{
    g_nrf54lm20_nvic = nvic;
}

void mm_nrf54lm20_grtc_tick(mm_u64 cycles)
{
    mm_u64 div;
    mm_u64 ticks;
    mm_u64 hz;
    mm_u32 ch;
    mm_u32 interval;
    mm_bool fired;
    if (!grtc_state.running) return;
    hz = mm_nrf54lm20_cpu_hz();
    if (hz == 0u) return;
    div = hz / 1000000ull;
    if (div == 0u) return;
    if (div > 1u) {
        div = 1u;
    }

    grtc_state.accum_cycles += cycles;
    ticks = grtc_state.accum_cycles / div;
    if (ticks == 0u) return;
    grtc_state.accum_cycles -= ticks * div;
    grtc_state.syscounter += ticks;

    interval = grtc_state.regs[GRTC_INTERVAL / 4] & 0xFFFFu;
    for (ch = 0; ch < GRTC_CC_COUNT; ++ch) {
        if ((grtc_state.cc_active_mask & (1u << ch)) == 0u) continue;
        if (grtc_state.syscounter >= grtc_state.cc[ch]) {
            fired = grtc_set_compare_event(&grtc_state, ch);
            if (fired && ch == 0u && interval != 0u) {
                grtc_state.cc[0] += interval;
                grtc_sync_cc_regs(&grtc_state, 0u);
            }
        }
    }
}
