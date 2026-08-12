/* gpio-input-mode-guard: PA5 briefly reconfigured to Output mid-test
 * while a binding targets it, then back to Input. Validates
 * stm32_gpio_set_external_input()'s mode guard (section 4.3): a
 * crossing that would arrive while the pin is in Output mode must be
 * rejected (not override firmware's own ODR-driven IDR value), and
 * control must resume correctly once the pin returns to Input.
 *
 * Trace: LOW -> HIGH(t1) -> LOW(t2) -> HIGH(t3) -> LOW(t4), evenly
 * spaced (period_ns=100000, i.e. 6400 cycles apart at the 64MHz nominal
 * boot clock).
 *
 * Sequence:
 *  1. Poll for t1 (Input mode): IDR must go HIGH.
 *  2. Switch to Output, drive ODR=1 (HIGH) -- deliberately the opposite
 *     of t2's LOW target, so if the rejection guard failed, IDR would
 *     visibly (and wrongly) drop to LOW when t2 dispatches.
 *  3. Busy-wait past t2's scheduled time (still well before t3).
 *  4. Check IDR is still HIGH (t2's LOW was correctly rejected).
 *  5. Switch back to Input (before t3 arrives -- t3's HIGH target
 *     coincides with the already-HIGH value, so it's not separately
 *     observable, but exercises the accept-path again harmlessly).
 *  6. Poll for t4 (LOW, distinguishable from the stuck HIGH): IDR must
 *     go LOW again, proving external control resumed correctly.
 *
 * bkpt #0x7f -> pass (all of the above held), bkpt #0x7e -> fail.
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
#define POLL1_BUDGET 20000u
#define WAIT2_ITERS  2500u
#define POLL2_BUDGET 25000u

__attribute__((noinline, used))
static void signal_trigger_marker(void)
{
    __asm volatile("nop");
}

static void fail(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    uint32_t *src, *dst;
    unsigned i;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0u;
    __libc_init_array();

    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    GPIO_MODER(GPIOA_BASE) &= ~(0x3u << (PIN * 2u)); /* PA5 input */

    signal_trigger_marker();

    /* 1. wait for t1 (rising) */
    {
        for (i = 0; i < POLL1_BUDGET; ++i) {
            if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
                goto t1_seen;
            }
        }
        fail(); /* t1 never arrived */
        t1_seen: ;
    }

    /* 2. switch to Output, drive HIGH (opposes t2's LOW target) */
    GPIO_ODR(GPIOA_BASE) |= (1u << PIN);
    GPIO_MODER(GPIOA_BASE) = (GPIO_MODER(GPIOA_BASE) & ~(0x3u << (PIN * 2u)))
                              | (0x1u << (PIN * 2u));

    /* 3. busy-wait past t2 (still well before t3) */
    for (i = 0; i < WAIT2_ITERS; ++i) {
        __asm volatile("nop");
    }

    /* 4. t2's rejected LOW must not have taken effect */
    if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) == 0u) {
        fail();
    }

    /* 5. switch back to Input (ODR left untouched) */
    GPIO_MODER(GPIOA_BASE) &= ~(0x3u << (PIN * 2u));

    /* 6. wait for t4 (falling): proves external control resumed */
    for (i = 0; i < POLL2_BUDGET; ++i) {
        if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) == 0u) {
            __asm volatile("bkpt #0x7f");
            while (1) {
                __asm volatile("wfi");
            }
        }
    }

    fail(); /* t4 never arrived */
}
