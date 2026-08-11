/* Non-uniform timebase: the 3-crossing trace must raise 3 interrupts. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5
#define EXPECTED_COUNT 3u
#define POLL_ITERATIONS 4000000u

static volatile uint32_t irq_count = 0;

void EXTI5_Handler(void)
{
    irq_count++;
    EXTI_RPR1 = (1u << PIN);
    EXTI_FPR1 = (1u << PIN);
}

void Reset_Handler(void)
{
    unsigned i;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, PIN);
    exti_enable(PIN, 0, 1, 1);

    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        if (irq_count >= EXPECTED_COUNT) {
            break;
        }
    }

    if (irq_count == EXPECTED_COUNT) {
        test_pass();
    }
    test_fail();
}
