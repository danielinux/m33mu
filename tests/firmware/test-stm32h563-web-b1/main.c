/* Web workbench on-board button demo: NUCLEO-H563ZI B1 (PC13, high while
 * pressed) is copied to LD1 (PB0) by polling. */

#include <stdint.h>
#include "../_signal_common/signal_test.h"

#define GPIOC_BASE 0x42020800u
#define RCC_AHB2ENR_GPIOCEN (1u << 2)

#define BUTTON_PIN 13
#define LED_PIN 0

void Reset_Handler(void)
{
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN;
    gpio_set_input(GPIOC_BASE, BUTTON_PIN);
    GPIO_ODR(GPIOB_BASE) &= ~(1u << LED_PIN);
    gpio_set_output(GPIOB_BASE, LED_PIN);
    for (;;) {
        if (GPIO_IDR(GPIOC_BASE) & (1u << BUTTON_PIN)) GPIO_ODR(GPIOB_BASE) |= 1u << LED_PIN;
        else GPIO_ODR(GPIOB_BASE) &= ~(1u << LED_PIN);
    }
}
