/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include "nrf_gpio.h"

/* PIN_CNF fields (nRF5340 PS GPIO chapter; nRF54L uses the same layout). */
#define CNF_DIR        (1u << 0)
#define CNF_INPUT_DISC (1u << 1)
#define CNF_PULL_SHIFT 2u
#define CNF_PULL_DOWN  1u
#define CNF_PULL_UP    3u
#define CNF_DRIVE_SHIFT 8u

static mm_u32 slice(mm_u32 reg, mm_u32 off, mm_u32 size)
{
    mm_u32 mask = size == 4u ? 0xFFFFFFFFu : ((1u << (size * 8u)) - 1u);
    return (reg >> (off * 8u)) & mask;
}

static mm_u32 merge(mm_u32 cur, mm_u32 off, mm_u32 size, mm_u32 value)
{
    mm_u32 mask = size == 4u ? 0xFFFFFFFFu : ((1u << (size * 8u)) - 1u);
    return (cur & ~(mask << (off * 8u))) | ((value & mask) << (off * 8u));
}

void nrf_gpio_bank_init(struct nrf_gpio_bank *g, int index, int pins)
{
    if (g == 0) return;
    memset(g, 0, sizeof(*g));
    g->index = index;
    g->pins = pins;
    /* Reset PIN_CNF: input, input buffer disconnected. */
    {
        int i;
        for (i = 0; i < 32; ++i) g->pin_cnf[i] = CNF_INPUT_DISC;
    }
}

/* Drive "disconnect 1" settings (S0D1, H0D1) make a pin open-drain. */
static mm_bool open_drain(mm_u32 cnf)
{
    mm_u32 drive = (cnf >> CNF_DRIVE_SHIFT) & 0xFu;
    return drive == 6u || drive == 7u;
}

/* Pins driven by the port and their levels (open drain drives only 0). */
static mm_u32 driven_mask(const struct nrf_gpio_bank *g, mm_u32 *level_out)
{
    mm_u32 dir = g->regs[NRF_GPIO_DIR / 4];
    mm_u32 out = g->regs[NRF_GPIO_OUT / 4];
    mm_u32 driven = dir;
    int pin;
    for (pin = 0; pin < 32; ++pin) {
        mm_u32 bit = 1u << pin;
        if ((dir & bit) != 0u && open_drain(g->pin_cnf[pin]) && (out & bit) != 0u) driven &= ~bit;
    }
    if (level_out != 0) *level_out = out & driven;
    return driven;
}

static void sync_inputs(struct nrf_gpio_bank *g)
{
    mm_u32 level = 0u;
    mm_u32 driven = driven_mask(g, &level);
    mm_u32 in = g->regs[NRF_GPIO_OUT / 4];
    int pin;
    for (pin = 0; pin < 32; ++pin) {
        mm_u32 bit = 1u << pin;
        mm_u32 pull = (g->pin_cnf[pin] >> CNF_PULL_SHIFT) & 3u;
        if ((driven & bit) != 0u) {
            in = (in & ~bit) | (level & bit);
        } else if ((g->ext_valid & bit) != 0u) {
            in = (in & ~bit) | (g->ext_level & bit);
        } else if (pull == CNF_PULL_UP) {
            in |= bit;
        } else if (pull == CNF_PULL_DOWN) {
            in &= ~bit;
        }
    }
    g->regs[NRF_GPIO_IN / 4] = in;
}

void nrf_gpio_publish(struct nrf_gpio_bank *g)
{
    mm_u32 level = 0u;
    mm_u32 driven;
    int pin;
    if (g == 0) return;
    sync_inputs(g);
    if (!mm_gpio_pad_listener_present() && !mm_gpio_pad_config_listener_present()) return;
    driven = driven_mask(g, &level);
    for (pin = 0; pin < g->pins && pin < 32; ++pin) {
        mm_u32 bit = 1u << pin;
        mm_u32 cnf = g->pin_cnf[pin];
        mm_u32 pull = (cnf >> CNF_PULL_SHIFT) & 3u;
        struct mm_gpio_pad_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.mode = (g->regs[NRF_GPIO_DIR / 4] & bit) != 0u ? MM_GPIO_MODE_OUTPUT : MM_GPIO_MODE_INPUT;
        cfg.open_drain = open_drain(cnf) ? 1u : 0u;
        cfg.pull = pull == CNF_PULL_UP ? MM_GPIO_PULL_UP : pull == CNF_PULL_DOWN ? MM_GPIO_PULL_DOWN : MM_GPIO_PULL_NONE;
        if (!g->pub_valid || memcmp(&cfg, &g->pub_cfg[pin], sizeof(cfg)) != 0) {
            g->pub_cfg[pin] = cfg;
            mm_gpio_notify_pad_config(g->index, pin, &cfg);
        }
        if (!g->pub_valid || ((g->pub_driven ^ driven) & bit) != 0u ||
            ((g->pub_level ^ level) & bit) != 0u) {
            mm_gpio_notify_pad(g->index, pin, (driven & bit) != 0u ? (mm_u8)((level & bit) != 0u)
                                                                   : (mm_u8)MM_GPIO_LEVEL_Z);
        }
    }
    g->pub_driven = driven;
    g->pub_level = level;
    g->pub_valid = MM_TRUE;
}

mm_bool nrf_gpio_set_external(struct nrf_gpio_bank *g, int pin, mm_bool level)
{
    if (g == 0 || pin < 0 || pin >= g->pins || pin >= 32) return MM_FALSE;
    g->ext_valid |= 1u << pin;
    if (level) g->ext_level |= 1u << pin;
    else g->ext_level &= ~(1u << pin);
    sync_inputs(g);
    return MM_TRUE;
}

mm_bool nrf_gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct nrf_gpio_bank *g = (struct nrf_gpio_bank *)opaque;
    if (g == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > NRF_GPIO_SIZE) return MM_FALSE;
    if (offset >= NRF_GPIO_PIN_CNF0 && size_bytes == 4) {
        mm_u32 pin = (offset - NRF_GPIO_PIN_CNF0) / 4u;
        if (pin < 32u) {
            *value_out = g->pin_cnf[pin];
            return MM_TRUE;
        }
    }
    if ((offset & ~3u) == NRF_GPIO_IN) sync_inputs(g);
    *value_out = slice(g->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

mm_bool nrf_gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct nrf_gpio_bank *g = (struct nrf_gpio_bank *)opaque;
    mm_u32 *out;
    mm_u32 *dir;
    if (g == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > NRF_GPIO_SIZE) return MM_FALSE;
    out = &g->regs[NRF_GPIO_OUT / 4];
    dir = &g->regs[NRF_GPIO_DIR / 4];
    if (offset >= NRF_GPIO_PIN_CNF0 && size_bytes == 4) {
        mm_u32 pin = (offset - NRF_GPIO_PIN_CNF0) / 4u;
        if (pin < 32u) {
            g->pin_cnf[pin] = value;
            /* PIN_CNF.DIR and the DIR register are one bit. */
            if ((value & CNF_DIR) != 0u) *dir |= 1u << pin;
            else *dir &= ~(1u << pin);
            nrf_gpio_publish(g);
            return MM_TRUE;
        }
    }
    if (size_bytes == 4 && offset == NRF_GPIO_OUT) *out = value;
    else if (size_bytes == 4 && offset == NRF_GPIO_OUTSET) *out |= value;
    else if (size_bytes == 4 && offset == NRF_GPIO_OUTCLR) *out &= ~value;
    else if (size_bytes == 4 && (offset == NRF_GPIO_DIR || offset == NRF_GPIO_DIRSET ||
                                 offset == NRF_GPIO_DIRCLR)) {
        mm_u32 old = *dir;
        mm_u32 pin;
        *dir = offset == NRF_GPIO_DIR ? value : offset == NRF_GPIO_DIRSET ? (old | value) : (old & ~value);
        for (pin = 0; pin < 32u; ++pin) {
            if ((*dir >> pin) & 1u) g->pin_cnf[pin] |= CNF_DIR;
            else g->pin_cnf[pin] &= ~CNF_DIR;
        }
    } else {
        g->regs[offset / 4] = merge(g->regs[offset / 4], offset & 3u, size_bytes, value);
        return MM_TRUE;
    }
    nrf_gpio_publish(g);
    return MM_TRUE;
}
