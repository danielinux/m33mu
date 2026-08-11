/* With SysTick off, the core sleeps in WFI and only the injected edges can
 * wake it: all 4 crossings must still raise their interrupts. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5
#define EXPECTED_COUNT 4u

static volatile uint32_t irq_count = 0;

void EXTI5_Handler(void)
{
    irq_count++;
    EXTI_RPR1 = (1u << PIN);
    EXTI_FPR1 = (1u << PIN);
}

void Reset_Handler(void)
{
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, PIN);
    exti_enable(PIN, 0, 1, 1);

    signal_trigger_marker();

    while (irq_count < EXPECTED_COUNT) {
        __asm volatile("wfi");
    }
    test_pass();
}
