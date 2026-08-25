/* gpio-output-timing: toggles PA5 every fixed number of loop iterations
 * and measures the elapsed processor-clock cycles (via SysTick, counting
 * down at the core clock) between toggles. Not the design doc's original
 * plan of logging over UART for external comparison -- simplified to a
 * self-contained assertion (a loose tolerance band, not exact-cycle
 * matching, since compiler-generated loop overhead isn't hand-counted
 * here) so it stays a plain pass/fail bkpt test like everything else in
 * this suite. The real point of this one: signal.c's tick hooks were
 * added directly into main.c's existing vcycles/cycle_total/mm_timer_tick
 * accounting sites (both the scalar and translation-block paths) -- this
 * is a regression guard that adding those hooks didn't silently corrupt
 * or stall that accounting, independent of whether any signal trace is
 * even loaded (no --signal-file needed for this test).
 *
 * bkpt #0x7f -> pass (every measured interval fell within tolerance),
 * bkpt #0x7e -> fail.
 */

#include <stdint.h>
#include "board_regs.h"

#define SYST_CSR   REG32(0xE000E010u)
#define SYST_RVR   REG32(0xE000E014u)
#define SYST_CVR   REG32(0xE000E018u)
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_CLKSOURCE (1u << 2) /* processor clock, no /8 prescale */

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
#define TOGGLE_COUNT 6
#define ITERS_PER_TOGGLE 1000u
#define MIN_EXPECTED_CYCLES 500u   /* loose lower bound */
#define MAX_EXPECTED_CYCLES 20000u /* loose upper bound */

static uint32_t systick_elapsed(uint32_t start_cvr, uint32_t end_cvr)
{
    /* SysTick counts down; reload wraps to SYST_RVR after hitting 0. */
    if (end_cvr <= start_cvr) {
        return start_cvr - end_cvr;
    }
    return (SYST_RVR - end_cvr) + start_cvr + 1u;
}

void Reset_Handler(void)
{
    uint32_t *src, *dst;
    int t;
    uint32_t level = 0;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0u;
    __libc_init_array();

    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    GPIO_MODER(GPIOA_BASE) = (GPIO_MODER(GPIOA_BASE) & ~(0x3u << (PIN * 2u)))
                              | (0x1u << (PIN * 2u));

    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_CLKSOURCE;

    for (t = 0; t < TOGGLE_COUNT; ++t) {
        uint32_t start_cvr = SYST_CVR;
        unsigned i;
        for (i = 0; i < ITERS_PER_TOGGLE; ++i) {
            __asm volatile("nop");
        }
        {
            uint32_t end_cvr = SYST_CVR;
            uint32_t elapsed = systick_elapsed(start_cvr, end_cvr);
            if (elapsed < MIN_EXPECTED_CYCLES || elapsed > MAX_EXPECTED_CYCLES) {
                __asm volatile("bkpt #0x7e");
                while (1) {
                    __asm volatile("wfi");
                }
            }
        }
        level ^= 1u;
        if (level) {
            GPIO_ODR(GPIOA_BASE) |= (1u << PIN);
        } else {
            GPIO_ODR(GPIOA_BASE) &= ~(1u << PIN);
        }
    }

    __asm volatile("bkpt #0x7f");
    while (1) {
        __asm volatile("wfi");
    }
}
