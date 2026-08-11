/* A masked EXTI line (IMR1 clear) must never run its ISR. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5
#define POLL_ITERATIONS 2000000u

static volatile uint32_t isr_hit = 0;

void EXTI5_Handler(void)
{
    isr_hit = 1;
    EXTI_RPR1 = (1u << PIN);
    EXTI_FPR1 = (1u << PIN);
}

void Reset_Handler(void)
{
    unsigned i;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, PIN);

    exti_select_port(PIN, 0);
    EXTI_RTSR1 |= (1u << PIN);
    nvic_enable_irq(EXTI_IRQN(PIN));
    enable_irqs();

    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        __asm volatile("nop");
    }

    if (isr_hit == 0u) {
        test_pass();
    }
    test_fail();
}
