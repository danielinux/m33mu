/* A scan of IN3, IN0, IN1 is stopped in the middle of rank 2. ADC1 is then
 * disabled, recalibrated and enabled again, and the scan converts in full. */

#include <stdint.h>
#include "adc_test.h"

#define RANKS 3u
#define SLOT  2612u   /* (640.5 + 12.5) * 4 */

static const uint32_t g_seq[RANKS] = { 3u, 0u, 1u };
static const uint32_t g_expected[RANKS] = { CODE_IN3, CODE_IN0, CODE_IN1 };

static void scan_polled(void)
{
    uint32_t i;
    uint32_t eos;

    ADC_ISR = ISR_ALL;
    cr_set(CR_ADSTART);
    for (i = 0; i < RANKS; ++i) {
        adc_wait_isr(ISR_EOC);
        eos = ADC_ISR & ISR_EOS;
        if ((eos != 0u) != (i == RANKS - 1u)) test_fail();
        if (ADC_DR != g_expected[i]) test_fail();
    }
    adc_wait_cr_clear(CR_ADSTART);
    if ((ADC_ISR & ISR_OVR) != 0u) test_fail();
}

void Reset_Handler(void)
{
    uint32_t t1;

    adc_board_init(3u, 0u);
    adc_sequence(g_seq, RANKS, SMP_640_5);

    signal_trigger_marker();

    ADC_ISR = ISR_ALL;
    cr_set(CR_ADSTART);
    adc_wait_isr(ISR_EOC);
    t1 = cyc();
    if ((ADC_ISR & ISR_EOS) != 0u || ADC_DR != CODE_IN3) test_fail();
    delay_until(t1 + SLOT / 2u);
    adc_stop();
    ADC_ISR = ISR_ALL;
    delay_until(cyc() + 3u * SLOT);
    if ((ADC_ISR & (ISR_EOC | ISR_EOS)) != 0u) test_fail();

    adc_disable();
    adc_calibrate();
    adc_enable();
    scan_polled();
    scan_polled();
    test_pass();
}
