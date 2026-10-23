/* Web workbench LED demo: PA5 (NUCLEO-H563ZI CN7 pin 10) toggles every
 * 100 SysTick periods of 64000 CPU cycles, sleeping in WFI in between. */

#include <stdint.h>
#include "../_signal_common/signal_test.h"

#define LED_PIN 5
#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)
#define TICKS_PER_TOGGLE 100u

static volatile uint32_t ticks;

void SysTick_Handler(void)
{
    ticks++;
}

void Reset_Handler(void)
{
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    GPIO_ODR(GPIOA_BASE) &= ~(1u << LED_PIN);
    gpio_set_output(GPIOA_BASE, LED_PIN);
    SYST_RVR = 64000u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 0x7u; /* processor clock, interrupt, enable */
    for (;;) {
        __asm volatile("wfi");
        if (ticks >= TICKS_PER_TOGGLE) {
            ticks = 0u;
            GPIO_ODR(GPIOA_BASE) ^= (1u << LED_PIN);
        }
    }
}
