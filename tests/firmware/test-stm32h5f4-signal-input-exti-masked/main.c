/* gpio-input-exti-masked: RTSR1 armed (edge detection active) but IMR1
 * left cleared (line masked) for the whole run. Validates the hard
 * requirement from decision #19's open item: exti_gpio_update() must
 * never call exti_raise_irq() (and therefore the ISR must never run)
 * when the line is masked, regardless of how many qualifying edges the
 * injected trace produces. The trace has >=1 rising crossing.
 *
 * This test deliberately does NOT assert anything about the pending
 * register (RPR1/FPR1) -- see the design doc's open item on that being
 * unresolved -- only that the ISR was never invoked.
 *
 * bkpt #0x7f -> pass (isr_hit stayed 0 for the whole run), bkpt #0x7e ->
 * fail (ISR ran despite the masked line).
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
#define POLL_ITERATIONS 2000000u

static volatile uint32_t isr_hit = 0;

void EXTI5_Handler(void)
{
    isr_hit = 1;
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
    GPIO_MODER(GPIOA_BASE) &= ~(0x3u << (PIN * 2u)); /* PA5 input */

    exti_select_port(PIN, 0 /* bank A */);
    EXTI_RTSR1 |= (1u << PIN);
    /* IMR1 deliberately left at reset value (masked) -- this is the
     * point of the test. */
    nvic_enable_irq(EXTI_IRQN(PIN));
    enable_irqs();

    signal_trigger_marker();

    /* Just burn cycles for a while, long enough for the trace's crossing
     * to have definitely been dispatched by signal.c, then check the ISR
     * never ran. There's no "wait for isr_hit" loop here on purpose --
     * we are proving a negative, so we wait a fixed budget instead. */
    for (i = 0; i < POLL_ITERATIONS; ++i) {
        __asm volatile("nop");
    }

    if (isr_hit == 0u) {
        __asm volatile("bkpt #0x7f");
    } else {
        __asm volatile("bkpt #0x7e");
    }
    while (1) {
        __asm volatile("wfi");
    }
}
