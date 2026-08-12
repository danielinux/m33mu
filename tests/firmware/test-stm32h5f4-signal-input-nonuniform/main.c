/* gpio-input-nonuniform: both edges armed, but the bound trace uses an
 * explicit non-uniform timebase (irregular timestamps_ns[], not a fixed
 * period_ns). The ISR counts every interrupt. Exercises the
 * timestamps_ns[] parsing path in mm_signal_load()/signal_timestamp_for_index()
 * used to build the crossing schedule from irregular sample spacing.
 *
 * Note: this does NOT exercise signal_sample_at()'s binary-search
 * lookup -- that function is currently unused for GPIO bindings (the
 * crossing schedule is precomputed once at bind time by walking samples
 * in order, not looked up per-tick); it's reserved for the ADC phase's
 * per-conversion voltage sampling. See the design doc's note on this.
 *
 * bkpt #0x7f -> pass (irq_count == 3), bkpt #0x7e -> fail (timeout or
 * wrong count).
 */

#include <stdint.h>
#include "board_regs.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

#define PIN 5
#define EXPECTED_COUNT 3u
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
