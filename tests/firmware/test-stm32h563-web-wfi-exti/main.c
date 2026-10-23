/* Worker integration fixture: a queued PA6 edge wakes WFI through EXTI. */

#include <stdint.h>
#include "../_signal_common/signal_test.h"

#define INPUT_PIN 6

static volatile uint32_t irq_count;

void EXTI6_Handler(void)
{
    irq_count++;
    EXTI_RPR1 = (1u << INPUT_PIN);
    EXTI_FPR1 = (1u << INPUT_PIN);
}

void Reset_Handler(void)
{
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_input(GPIOA_BASE, INPUT_PIN);
    exti_enable(INPUT_PIN, 0, 1, 0);

    while (irq_count == 0u) {
        __asm volatile("wfi");
    }
    test_pass();
}
