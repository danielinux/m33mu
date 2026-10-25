/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include "m33mu/gpio_pads.h"

void mm_gpio_pads_init(struct mm_gpio_pads *p, int bank, int pins)
{
    if (p == 0) return;
    memset(p, 0, sizeof(*p));
    p->bank = bank;
    p->pins = pins > 32 ? 32 : pins;
}

void mm_gpio_pads_publish(struct mm_gpio_pads *p, mm_u32 driven, mm_u32 level,
                          mm_gpio_pads_config_fn cfg_fn, void *opaque)
{
    int pin;
    if (p == 0) return;
    if (!mm_gpio_pad_listener_present() && !mm_gpio_pad_config_listener_present()) return;
    level &= driven;
    for (pin = 0; pin < p->pins; ++pin) {
        mm_u32 bit = 1u << pin;
        struct mm_gpio_pad_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.mode = (driven & bit) != 0u ? MM_GPIO_MODE_OUTPUT : MM_GPIO_MODE_INPUT;
        if (cfg_fn != 0) cfg_fn(opaque, pin, &cfg);
        if (!p->pub_valid || memcmp(&cfg, &p->pub_cfg[pin], sizeof(cfg)) != 0) {
            p->pub_cfg[pin] = cfg;
            mm_gpio_notify_pad_config(p->bank, pin, &cfg);
        }
        if (!p->pub_valid || ((p->pub_driven ^ driven) & bit) != 0u ||
            ((p->pub_level ^ level) & bit) != 0u) {
            mm_gpio_notify_pad(p->bank, pin, (driven & bit) != 0u ? (mm_u8)((level & bit) != 0u)
                                                                  : (mm_u8)MM_GPIO_LEVEL_Z);
        }
    }
    p->pub_driven = driven;
    p->pub_level = level;
    p->pub_valid = MM_TRUE;
}

mm_u32 mm_gpio_pads_input(const struct mm_gpio_pads *p, mm_u32 driven, mm_u32 level,
                          mm_u32 pull_up, mm_u32 pull_down, mm_u32 idle)
{
    mm_u32 ext = p != 0 ? p->ext_valid & ~driven : 0u;
    mm_u32 undriven = ~driven & ~ext;
    mm_u32 in = level & driven;
    if (p != 0) in |= p->ext_level & ext;
    in |= pull_up & undriven;
    in |= idle & undriven & ~pull_up & ~pull_down;
    return in;
}

mm_bool mm_gpio_pads_set_external(struct mm_gpio_pads *p, int pin, mm_bool level)
{
    if (p == 0 || pin < 0 || pin >= p->pins) return MM_FALSE;
    p->ext_valid |= 1u << pin;
    if (level) p->ext_level |= 1u << pin;
    else p->ext_level &= ~(1u << pin);
    return MM_TRUE;
}
