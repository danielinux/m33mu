/* gpio-input-sharp-edge: a 2-sample trace (0mV, 3300mV) with a period of
 * only 16ns (~1 cycle at the 64MHz nominal boot clock), i.e. the
 * crossing is scheduled essentially immediately after the trigger.
 * Validates the "instantaneous edge" zero-order-hold claim from the
 * design doc's section 3.2/4.1: the transition must become visible on
 * IDR within a small, tight iteration budget, not smeared out or
 * delayed by anything beyond ordinary dispatch latency (the size of
 * whatever translation block happens to be executing when the crossing
 * time is reached -- see the design doc's note on the tb-batched
 * dispatch path).
 *
 * A tight bound here is deliberate: it's exactly what would have caught
 * the tb-path dispatch bug found during implementation (an early,
 * incorrect version of the trigger-check placement caused the crossing
 * to never be observed at all within any reasonable budget).
 *
 * bkpt #0x7f -> pass (IDR went high within POLL_BUDGET iterations),
 * bkpt #0x7e -> fail.
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
#define POLL_BUDGET 200u /* deliberately tight -- see header comment */

__attribute__((noinline, used))
static void signal_trigger_marker(void)
{
    __asm volatile("nop");
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
    GPIO_MODER(GPIOA_BASE) &= ~(0x3u << (PIN * 2u));

    signal_trigger_marker();

    for (i = 0; i < POLL_BUDGET; ++i) {
        if ((GPIO_IDR(GPIOA_BASE) & (1u << PIN)) != 0u) {
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
