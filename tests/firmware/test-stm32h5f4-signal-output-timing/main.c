/* SysTick must measure a fixed busy loop within a loose cycle band. */

#include <stdint.h>
#include "signal_test.h"

#define SYST_CSR   REG32(0xE000E010u)
#define SYST_RVR   REG32(0xE000E014u)
#define SYST_CVR   REG32(0xE000E018u)
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_CLKSOURCE (1u << 2)

#define PIN 5
#define TOGGLE_COUNT 6
#define ITERS_PER_TOGGLE 1000u
#define MIN_EXPECTED_CYCLES 500u
#define MAX_EXPECTED_CYCLES 20000u

static uint32_t systick_elapsed(uint32_t start_cvr, uint32_t end_cvr)
{
    if (end_cvr <= start_cvr) {
        return start_cvr - end_cvr;
    }
    return (SYST_RVR - end_cvr) + start_cvr + 1u;
}

void Reset_Handler(void)
{
    int t;
    uint32_t level = 0;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    gpio_set_output(GPIOA_BASE, PIN);

    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_CLKSOURCE;

    for (t = 0; t < TOGGLE_COUNT; ++t) {
        uint32_t start_cvr = SYST_CVR;
        uint32_t elapsed;
        unsigned i;

        for (i = 0; i < ITERS_PER_TOGGLE; ++i) {
            __asm volatile("nop");
        }
        elapsed = systick_elapsed(start_cvr, SYST_CVR);
        if (elapsed < MIN_EXPECTED_CYCLES || elapsed > MAX_EXPECTED_CYCLES) {
            test_fail();
        }
        level ^= 1u;
        if (level) {
            GPIO_ODR(GPIOA_BASE) |= (1u << PIN);
        } else {
            GPIO_ODR(GPIOA_BASE) &= ~(1u << PIN);
        }
    }
    test_pass();
}
