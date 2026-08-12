/* gpio-input-exti-both: both RTSR1 and FTSR1 armed and unmasked for
 * PA5/EXTI5; the ISR counts every interrupt. The bound trace is
 * constructed with exactly 4 threshold crossings (rise, fall, rise,
 * fall). Validates decision #19: removing the bind-time edge= parameter
 * and always forwarding both directions from signal.c, relying on
 * exti_gpio_update() to gate on RTSR1/FTSR1 dynamically, produces the
 * correct interrupt count when firmware arms both edges.
 *
 * bkpt #0x7f -> pass (irq_count == 4), bkpt #0x7e -> fail (timeout or
 * wrong count).
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
#define EXPECTED_COUNT 4u
#define POLL_ITERATIONS 4000000u

static volatile uint32_t irq_count = 0;

void EXTI5_Handler(void)
{
    irq_count++;
    /* Write-1-to-clear both pending bits: whichever direction actually
     * fired is the only one set, clearing both is harmless and avoids
     * needing to read-then-selectively-clear. */
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
    EXTI_FTSR1 |= (1u << PIN);
    EXTI_IMR1  |= (1u << PIN); /* unmasked: NVIC IRQ actually fires */
    nvic_enable_irq(EXTI_IRQN(PIN));
    enable_irqs();

    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        if (irq_count >= EXPECTED_COUNT) {
            break;
        }
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
