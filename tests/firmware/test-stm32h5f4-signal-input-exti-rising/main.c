/* gpio-input-exti-rising: RTSR1 only (FTSR1 left clear). The trace has
 * 2 rising and 2 falling crossings; only the rising ones should produce
 * an interrupt. Validates exti_gpio_update() correctly gates on RTSR1
 * alone, ignoring falling transitions when FTSR1 isn't armed.
 *
 * bkpt #0x7f -> pass (irq_count == 2), bkpt #0x7e -> fail.
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
#define EXPECTED_COUNT 2u
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
    EXTI_RTSR1 |= (1u << PIN); /* rising only */
    EXTI_IMR1  |= (1u << PIN);
    nvic_enable_irq(EXTI_IRQN(PIN));
    enable_irqs();

    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        /* Can't early-break on irq_count==EXPECTED_COUNT the way
         * exti-both does: if gating were broken and falling edges also
         * fired, we'd want to see the wrong (too-high) count rather than
         * stop early and miss it. Burn the full budget instead. */
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
