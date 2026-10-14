/* An idle-high trace reads high before the trigger, and its single falling
 * crossing raises exactly one falling-edge interrupt. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5
#define POLL_ITERATIONS 4000000u

static volatile uint32_t irq_count = 0;
static volatile uint32_t falling_count = 0;

void EXTI5_Handler(void)
{
    irq_count++;
    if ((EXTI_FPR1 & (1u << PIN)) != 0u) {
        falling_count++;
    }
    EXTI_RPR1 = (1u << PIN);
    EXTI_FPR1 = (1u << PIN);
}

void Reset_Handler(void)
{
    unsigned i;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, PIN);
    if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) == 0u) {
        test_fail();
    }
    exti_enable(PIN, 0, 1, 1);

    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        __asm volatile("nop");
    }

    if (irq_count == 1u && falling_count == 1u &&
        (GPIO_IDR(GPIOA_BASE) & (1u << PIN)) == 0u) {
        test_pass();
    }
    test_fail();
}
