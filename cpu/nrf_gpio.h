/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* Nordic GPIO port (nRF5340 and nRF54L): OUT/IN/DIR, PIN_CNF and pads. */

#ifndef M33MU_NRF_GPIO_H
#define M33MU_NRF_GPIO_H

#include "m33mu/types.h"
#include "m33mu/gpio.h"

#define NRF_GPIO_SIZE      0x300u
#define NRF_GPIO_OUT       0x004u
#define NRF_GPIO_OUTSET    0x008u
#define NRF_GPIO_OUTCLR    0x00Cu
#define NRF_GPIO_IN        0x010u
#define NRF_GPIO_DIR       0x014u
#define NRF_GPIO_DIRSET    0x018u
#define NRF_GPIO_DIRCLR    0x01Cu
#define NRF_GPIO_PIN_CNF0  0x200u

struct nrf_gpio_bank {
    mm_u32 regs[NRF_GPIO_SIZE / 4];
    mm_u32 pin_cnf[32];
    int index;          /* port number: pads are reported as bank `index` */
    int pins;
    mm_u32 ext_valid;   /* pins an external circuit drives */
    mm_u32 ext_level;
    mm_u32 pub_driven;  /* last reported drive */
    mm_u32 pub_level;
    struct mm_gpio_pad_config pub_cfg[32];
    mm_bool pub_valid;
};

void nrf_gpio_bank_init(struct nrf_gpio_bank *g, int index, int pins);
mm_bool nrf_gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out);
mm_bool nrf_gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value);
/* An external circuit drives `pin`; undriven input pins read it. */
mm_bool nrf_gpio_set_external(struct nrf_gpio_bank *g, int pin, mm_bool level);
/* Report changed pad drive/configuration (after direct register changes). */
void nrf_gpio_publish(struct nrf_gpio_bank *g);

#endif /* M33MU_NRF_GPIO_H */
