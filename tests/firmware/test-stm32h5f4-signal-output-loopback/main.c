/* An output pin's IDR must still mirror ODR. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5

void Reset_Handler(void)
{
    int i;
    static const uint32_t sequence[] = { 1u, 0u, 1u, 1u, 0u, 0u, 1u, 0u };

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_output(GPIOA_BASE, PIN);

    for (i = 0; i < (int)(sizeof(sequence) / sizeof(sequence[0])); ++i) {
        uint32_t odr = GPIO_ODR(GPIOA_BASE);
        if (sequence[i]) {
            odr |= (1u << PIN);
        } else {
            odr &= ~(1u << PIN);
        }
        GPIO_ODR(GPIOA_BASE) = odr;

        if (((GPIO_IDR(GPIOA_BASE) >> PIN) & 1u) != sequence[i]) {
            test_fail();
        }
    }
    test_pass();
}
