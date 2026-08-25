/* gpio-input-hysteresis: trace values deliberately wobble inside the
 * Schmitt hysteresis band (vdd/2 +/- 125mV, i.e. between 1525mV and
 * 1775mV at vdd=3300mV), never actually crossing either threshold.
 * Validates decision #17: the crossing schedule must produce ZERO
 * dispatched transitions for in-band noise. A naive flat-VDD/2
 * comparator (no hysteresis) would flip state on every sample and fire
 * spuriously.
 *
 * Uses EXTI (both edges armed) as the observable: if the Schmitt state
 * machine is correct, irq_count stays 0 for the whole run despite the
 * trace data changing on every sample.
 *
 * bkpt #0x7f -> pass (irq_count == 0), bkpt #0x7e -> fail (spurious
 * interrupt(s) fired).
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
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

    signal_trigger_marker();

    for (i = 0; i < POLL_ITERATIONS; ++i) {
        __asm volatile("nop");
    }

    if (irq_count == 0u) {
        __asm volatile("bkpt #0x7f");
    } else {
        __asm volatile("bkpt #0x7e");
    }
    while (1) {
        __asm volatile("wfi");
    }
}
