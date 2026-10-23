/* Live RC feedback fixture: each PA6 threshold interrupt toggles PA5. */

#include <stdint.h>
#include "../_signal_common/signal_test.h"

#define OUTPUT_PIN 5
#define INPUT_PIN 6
#define EXPECTED_EDGES 4u
#define POLL_ITERATIONS 1000000u

static volatile uint32_t irq_count;

void EXTI6_Handler(void)
{
    uint32_t odr;
    irq_count++;
    EXTI_RPR1 = (1u << INPUT_PIN);
    EXTI_FPR1 = (1u << INPUT_PIN);
    odr = GPIO_ODR(GPIOA_BASE) ^ (1u << OUTPUT_PIN);
    GPIO_ODR(GPIOA_BASE) = odr;
}

void Reset_Handler(void)
{
    unsigned i;
    uint32_t odr;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_output(GPIOA_BASE, OUTPUT_PIN);
    gpio_set_input(GPIOA_BASE, INPUT_PIN);
    odr = GPIO_ODR(GPIOA_BASE) & ~(1u << OUTPUT_PIN);
    GPIO_ODR(GPIOA_BASE) = odr;
    exti_enable(INPUT_PIN, 0, 1, 1);
    GPIO_ODR(GPIOA_BASE) = odr | (1u << OUTPUT_PIN);
    signal_trigger_marker();

    for (i = 0u; i < POLL_ITERATIONS; ++i) {
        if (irq_count >= EXPECTED_EDGES) break;
    }
    if (irq_count == EXPECTED_EDGES) test_pass();
    test_fail();
}
