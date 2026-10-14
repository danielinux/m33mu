/* PB0 toggles on every SysTick wrap after the marker; PB1 is open-drain. */

#include <stdint.h>
#include "signal_test.h"

#define GPIO_OTYPER(base)  REG32((base) + 0x04u)

#define SYST_CSR   REG32(0xE000E010u)
#define SYST_RVR   REG32(0xE000E014u)
#define SYST_CVR   REG32(0xE000E018u)
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_CLKSOURCE (1u << 2)
#define SYST_CSR_COUNTFLAG (1u << 16)

#define PERIOD_CYCLES 6400u
#define TOGGLE_COUNT  8

void Reset_Handler(void)
{
    int t;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    GPIO_OTYPER(GPIOB_BASE) |= (1u << 1);
    GPIO_ODR(GPIOB_BASE) |= (1u << 1);
    gpio_set_output(GPIOB_BASE, 0);
    gpio_set_output(GPIOB_BASE, 1);

    SYST_RVR = PERIOD_CYCLES - 1u;
    SYST_CVR = 0;

    signal_trigger_marker();
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_CLKSOURCE;

    for (t = 0; t < TOGGLE_COUNT; ++t) {
        while ((SYST_CSR & SYST_CSR_COUNTFLAG) == 0u) {
        }
        GPIO_ODR(GPIOB_BASE) ^= (1u << 0);
    }
    GPIO_ODR(GPIOB_BASE) &= ~(1u << 1);
    GPIO_ODR(GPIOB_BASE) |= (1u << 1);
    test_pass();
}
