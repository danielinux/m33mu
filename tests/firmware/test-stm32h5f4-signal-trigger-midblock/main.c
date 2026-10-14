/* --signal-sync on a fall-through instruction, not a branch target, must
 * still start replay. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5
#define POLL_ITERATIONS 2000000u

void Reset_Handler(void)
{
    unsigned i;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, PIN);

    __asm volatile("nop\n"
                   ".global signal_midblock_marker\n"
                   "signal_midblock_marker:\n"
                   "nop\n");

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
            test_pass();
        }
    }
    test_fail();
}
