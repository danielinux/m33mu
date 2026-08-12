/* gpio-trigger-oneshot: the trigger-marker function is called twice.
 * Validates decision #10 (one-shot, no re-arm): only the first call
 * should reset the binding's elapsed-time cursor and start replay; the
 * second call must be a no-op. Observable via EXTI: the trace has
 * exactly 1 rising crossing. If the trigger incorrectly re-armed on the
 * second call, the cursor would reset back to t=0 right as (or after)
 * the crossing was about to dispatch, and firmware would either see 0
 * interrupts (crossing perpetually reset away) or, if timed unluckily,
 * an extra one -- either way, != 1.
 *
 * bkpt #0x7f -> pass (irq_count == 1), bkpt #0x7e -> fail.
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
#define EXPECTED_COUNT 1u
#define POLL_ITERATIONS 4000000u

static volatile uint32_t irq_count = 0;

void EXTI5_Handler(void)
{
    irq_count++;
    EXTI_RPR1 = (1u << PIN);
    EXTI_FPR1 = (1u << PIN);
}

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

    exti_select_port(PIN, 0);
    EXTI_RTSR1 |= (1u << PIN);
    EXTI_FTSR1 |= (1u << PIN);
    EXTI_IMR1  |= (1u << PIN);
    nvic_enable_irq(EXTI_IRQN(PIN));
    enable_irqs();

    signal_trigger_marker(); /* first call: must arm */
    signal_trigger_marker(); /* second call: must be a no-op */

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        __asm volatile("nop");
    }

    if (irq_count == EXPECTED_COUNT) {
        __asm volatile("bkpt #0x7f");
    } else {
        __asm volatile("bkpt #0x7e");
    }
    while (1) {
        __asm volatile("wfi");
    }
}
