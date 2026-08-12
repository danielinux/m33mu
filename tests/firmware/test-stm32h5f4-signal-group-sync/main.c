/* gpio-group-sync: two bindings (PA5, PB2) in the same group, each with
 * an identical crossing schedule. Validates decision #8: both cursors
 * reset to elapsed_ns=0 at exactly the same instant when the shared
 * master trigger fires, regardless of firmware's read order. If the
 * bindings were independently anchored (a bug reintroducing the
 * rejected per-peripheral-anchor design), the two pins could plausibly
 * still line up by coincidence with identical schedules -- so this test
 * doesn't prove independent-vs-shared anchoring definitively on its
 * own, but it does prove the two bindings are at minimum consistent
 * with the shared-origin model to within a tight tolerance, which the
 * design requires.
 *
 * Polls both pins in the same tight loop, recording the iteration index
 * at which each first reads high; the two indices must be within a
 * small tolerance of each other.
 *
 * bkpt #0x7f -> pass (both went high, indices within tolerance),
 * bkpt #0x7e -> fail (one or both never went high, or indices diverged).
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN_A 5   /* PA5 */
#define PIN_B 2   /* PB2 */
#define POLL_ITERATIONS 4000000u
#define TOLERANCE_ITERATIONS 10u

__attribute__((noinline, used))
static void signal_trigger_marker(void)
{
    __asm volatile("nop");
}

void Reset_Handler(void)
{
    uint32_t *src, *dst;
    unsigned i;
    long idx_a = -1, idx_b = -1;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0u;
    __libc_init_array();

    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    GPIO_MODER(GPIOA_BASE) &= ~(0x3u << (PIN_A * 2u));
    GPIO_MODER(GPIOB_BASE) &= ~(0x3u << (PIN_B * 2u));

    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        if (idx_a < 0 && (GPIO_IDR(GPIOA_BASE) & (1u << PIN_A)) != 0u) {
            idx_a = (long)i;
        }
        if (idx_b < 0 && (GPIO_IDR(GPIOB_BASE) & (1u << PIN_B)) != 0u) {
            idx_b = (long)i;
        }
        if (idx_a >= 0 && idx_b >= 0) {
            break;
        }
    }

    if (idx_a >= 0 && idx_b >= 0) {
        long diff = idx_a - idx_b;
        if (diff < 0) diff = -diff;
        if ((uint32_t)diff <= TOLERANCE_ITERATIONS) {
            __asm volatile("bkpt #0x7f");
            while (1) {
                __asm volatile("wfi");
            }
        }
    }

    __asm volatile("bkpt #0x7e");
    while (1) {
        __asm volatile("wfi");
    }
}
