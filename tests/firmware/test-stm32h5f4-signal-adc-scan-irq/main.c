/* ADC1 scans IN3, IN0, IN1 three times. The EOC handler reads one rank per
 * interrupt with HAL_ADC_IRQHandler's flow: EOC selection, data preserved,
 * so OVRIE is on and an overrun would be an error. */

#include <stdint.h>
#include "adc_test.h"

#define RANKS  3u
#define PASSES 3u

static const uint32_t g_seq[RANKS] = { 3u, 0u, 1u };
static const uint32_t g_expected[RANKS] = { CODE_IN3, CODE_IN0, CODE_IN1 };

static volatile uint32_t g_codes[RANKS];
static volatile uint32_t g_count;
static volatile uint32_t g_done;
static volatile uint32_t g_errors;

void ADC1_Handler(void)
{
    uint32_t isr = ADC_ISR;
    uint32_t ier = ADC_IER;
    uint32_t code;

    if ((isr & ier & (ISR_EOC | ISR_EOS)) != 0u) {
        if ((ADC_CFGR & CFGR_CONT) == 0u && (ADC_ISR & ISR_EOS) != 0u) {
            if ((ADC_CR & CR_ADSTART) == 0u) {
                ADC_IER &= ~(IER_EOCIE | IER_EOSIE);
                g_done = 1u;
            } else {
                test_fail();
            }
        }
        code = ADC_DR;
        if (g_count < RANKS) {
            g_codes[g_count] = code;
        }
        g_count++;
        ADC_ISR = ISR_EOC | ISR_EOS;
    }
    if ((isr & ier & ISR_OVR) != 0u) {
        g_errors++;
        ADC_ISR = ISR_OVR;
    }
}

/* HAL_ADC_Start_IT */
static void start_it(void)
{
    g_count = 0u;
    g_done = 0u;
    ADC_ISR = ISR_EOC | ISR_EOS | ISR_OVR;
    ADC_IER &= ~(IER_EOCIE | IER_EOSIE | IER_OVRIE);
    ADC_IER |= IER_EOCIE | IER_OVRIE;
    cr_set(CR_ADSTART);
}

void Reset_Handler(void)
{
    uint32_t pass;
    uint32_t i;

    adc_board_init(3u, 0u);
    adc_sequence(g_seq, RANKS, SMP_92_5);
    nvic_enable_irq(ADC1_IRQN);
    enable_irqs();

    signal_trigger_marker();

    for (pass = 0; pass < PASSES; ++pass) {
        start_it();
        wfi_until(&g_done);
        if (g_count != RANKS || g_errors != 0u || (ADC_CR & CR_ADSTART) != 0u) test_fail();
        if ((ADC_IER & (IER_EOCIE | IER_EOSIE)) != 0u || (ADC_IER & IER_OVRIE) == 0u) test_fail();
        for (i = 0; i < RANKS; ++i) {
            if (g_codes[i] != g_expected[i]) test_fail();
        }
    }
    test_pass();
}
