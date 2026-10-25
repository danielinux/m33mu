/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* Pad reporting for a 32-pin GPIO port model: change-only drive and
 * configuration notifications to the mm_gpio listeners, and the input view
 * when an external circuit drives undriven pins. */

#ifndef M33MU_GPIO_PADS_H
#define M33MU_GPIO_PADS_H

#include "m33mu/types.h"
#include "m33mu/gpio.h"

struct mm_gpio_pads {
    int bank;
    int pins;
    mm_u32 ext_valid;
    mm_u32 ext_level;
    mm_u32 pub_driven;
    mm_u32 pub_level;
    struct mm_gpio_pad_config pub_cfg[32];
    mm_bool pub_valid;
};

typedef void (*mm_gpio_pads_config_fn)(void *opaque, int pin, struct mm_gpio_pad_config *cfg);

void mm_gpio_pads_init(struct mm_gpio_pads *p, int bank, int pins);
/* driven: pins the port drives; level: their levels. cfg_fn fills each pin's
 * mode/pull/af/open_drain. */
void mm_gpio_pads_publish(struct mm_gpio_pads *p, mm_u32 driven, mm_u32 level,
                          mm_gpio_pads_config_fn cfg_fn, void *opaque);
/* What an input read sees: driven pins their level, externally driven pins
 * the external level, pulled pins their pull, others `idle` (the model's
 * previous behaviour). */
mm_u32 mm_gpio_pads_input(const struct mm_gpio_pads *p, mm_u32 driven, mm_u32 level,
                          mm_u32 pull_up, mm_u32 pull_down, mm_u32 idle);
mm_bool mm_gpio_pads_set_external(struct mm_gpio_pads *p, int pin, mm_bool level);

#endif /* M33MU_GPIO_PADS_H */
