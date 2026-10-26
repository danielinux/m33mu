/* The core sleeps in WFI until ADC1's EOC interrupt. With CKMODE = HCLK/4 a
 * conversion takes (sampling + 12.5) * 4 cycles; the handler must run no
 * earlier than that and within SLACK cycles after it. */

#include <stdint.h>
#include "adc_test.h"

#define CONV_640_5 2612u
#define CONV_247_5 1040u
/* The start waits for the end of its batch; then exception entry, prologue. */
#define SLACK      256u

static volatile uint32_t g_t1;
static volatile uint32_t g_code;
static volatile uint32_t g_done;

void ADC1_Handler(void)
{
    g_t1 = cyc();
    g_code = ADC_DR;
    ADC_ISR = ISR_EOC | ISR_EOS;
    g_done = 1u;
}

static void check_wakeup(uint32_t smp, uint32_t conv)
{
    uint32_t ch = 0u;
    uint32_t t0;
    uint32_t dt;

    adc_sequence(&ch, 1u, smp);
    g_done = 0u;
    ADC_ISR = ISR_ALL;
    t0 = cyc();
    cr_set(CR_ADSTART);
    wfi_until(&g_done);
    dt = g_t1 - t0;
    if (g_code != CODE_IN0 || dt < conv || dt > conv + SLACK) test_fail();
}

void Reset_Handler(void)
{
    adc_board_init(3u, 0u);
    ADC_IER = IER_EOCIE;
    nvic_enable_irq(ADC1_IRQN);
    enable_irqs();

    signal_trigger_marker();

    check_wakeup(SMP_640_5, CONV_640_5);
    check_wakeup(SMP_247_5, CONV_247_5);
    test_pass();
}
