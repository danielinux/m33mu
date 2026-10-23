/* Web workbench input demo: PA6 (CN9 pin 1) is a plain input fed by an
 * external RC/divider; its EXTI handler mirrors the input level on PA5
 * (CN7 pin 10) and counts edges. The core sleeps in WFI otherwise. */

#include <stdint.h>
#include "../_signal_common/signal_test.h"

#define LED_PIN 5
#define INPUT_PIN 6

volatile uint32_t edge_count;

void EXTI6_Handler(void)
{
    uint32_t odr;
    EXTI_RPR1 = (1u << INPUT_PIN);
    EXTI_FPR1 = (1u << INPUT_PIN);
    edge_count++;
    odr = GPIO_ODR(GPIOA_BASE) & ~(1u << LED_PIN);
    if (GPIO_IDR(GPIOA_BASE) & (1u << INPUT_PIN)) odr |= (1u << LED_PIN);
    GPIO_ODR(GPIOA_BASE) = odr;
}

void Reset_Handler(void)
{
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    GPIO_ODR(GPIOA_BASE) &= ~(1u << LED_PIN);
    gpio_set_output(GPIOA_BASE, LED_PIN);
    gpio_set_input(GPIOA_BASE, INPUT_PIN);
    exti_enable(INPUT_PIN, 0, 1, 1);
    for (;;) {
        __asm volatile("wfi");
    }
}
