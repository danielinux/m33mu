/* A crossing 16ns after the trigger must show on IDR within a few polls. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5
#define POLL_BUDGET 200u

void Reset_Handler(void)
{
    unsigned i;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, PIN);

    signal_trigger_marker();

    for (i = 0; i < POLL_BUDGET; ++i) {
        if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
            test_pass();
        }
    }
    test_fail();
}
