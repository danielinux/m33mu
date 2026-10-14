/* A crossing that arrives while PA5 is an output must not change IDR, and
 * IDR must show the pad level as soon as the pin is back in input mode.
 * Trace: low, high(t1), low(t2), high(t3), low(t4), 100us apart. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5
#define POLL1_BUDGET 20000u
#define WAIT2_ITERS  2500u
#define POLL2_BUDGET 25000u

void Reset_Handler(void)
{
    unsigned i;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, PIN);

    signal_trigger_marker();

    for (i = 0; i < POLL1_BUDGET; ++i) {
        if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
            break;
        }
    }
    if (i == POLL1_BUDGET) {
        test_fail();
    }

    /* Drive high through t2, which would pull the pin low. */
    GPIO_ODR(GPIOA_BASE) |= (1u << PIN);
    gpio_set_output(GPIOA_BASE, PIN);
    for (i = 0; i < WAIT2_ITERS; ++i) {
        __asm volatile("nop");
    }
    if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) == 0u) {
        test_fail();
    }

    gpio_set_input(GPIOA_BASE, PIN);
    if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
        test_fail();
    }
    for (i = 0; i < POLL2_BUDGET; ++i) {
        if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
            test_pass();
        }
    }
    test_fail();
}
