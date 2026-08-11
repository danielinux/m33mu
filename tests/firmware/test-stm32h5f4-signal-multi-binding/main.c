/* PA5 and PB2 bound to identical traces must rise together. */

#include <stdint.h>
#include "signal_test.h"

#define PIN_A 5
#define PIN_B 2
#define POLL_ITERATIONS 4000000u
#define TOLERANCE_ITERATIONS 10u

void Reset_Handler(void)
{
    unsigned i;
    long idx_a = -1, idx_b = -1;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    gpio_set_input(GPIOA_BASE, PIN_A);
    gpio_set_input(GPIOB_BASE, PIN_B);

    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        if (idx_a < 0 && (GPIO_IDR(GPIOA_BASE) & (1u << PIN_A)) != 0u) {
            idx_a = (long)i;
        }
        if (idx_b < 0 && (GPIO_IDR(GPIOB_BASE) & (1u << PIN_B)) != 0u) {
            idx_b = (long)i;
        }
        if (idx_a >= 0 && idx_b >= 0) {
            break;
        }
    }

    if (idx_a >= 0 && idx_b >= 0) {
        long diff = idx_a - idx_b;
        if (diff < 0) diff = -diff;
        if ((uint32_t)diff <= TOLERANCE_ITERATIONS) {
            test_pass();
        }
    }
    test_fail();
}
