/* gpio-output-loopback: firmware drives a known ODR sequence on PA5 and
 * reads back IDR after each write. Pure regression check that
 * gpio_sync_odr()'s existing ODR->IDR mirror behavior is untouched by
 * the new stm32_gpio_set_external_input() path added for signal
 * injection -- both write the same IDR register, so this is cheap
 * insurance that the new code didn't disturb the old. No --signal-file
 * or --signal-bind needed for this test at all.
 *
 * bkpt #0x7f -> pass (every write was read back correctly),
 * bkpt #0x7e -> fail.
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5

void Reset_Handler(void)
{
    uint32_t *src, *dst;
    int i;
    static const uint32_t sequence[] = { 1u, 0u, 1u, 1u, 0u, 0u, 1u, 0u };

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0u;
    __libc_init_array();

    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    /* PA5 as output: MODER bits [11:10] = 01 */
    GPIO_MODER(GPIOA_BASE) = (GPIO_MODER(GPIOA_BASE) & ~(0x3u << (PIN * 2u)))
                              | (0x1u << (PIN * 2u));

    for (i = 0; i < (int)(sizeof(sequence) / sizeof(sequence[0])); ++i) {
        uint32_t odr = GPIO_ODR(GPIOA_BASE);
        if (sequence[i]) {
            odr |= (1u << PIN);
        } else {
            odr &= ~(1u << PIN);
        }
        GPIO_ODR(GPIOA_BASE) = odr;

        if (((GPIO_IDR(GPIOA_BASE) >> PIN) & 1u) != sequence[i]) {
            __asm volatile("bkpt #0x7e");
            while (1) {
                __asm volatile("wfi");
            }
        }
    }

    __asm volatile("bkpt #0x7f");
    while (1) {
        __asm volatile("wfi");
    }
}
