/* ADC3 with its own clock and common block converts PB2, PE7 and PE15
 * (IN0, IN1, IN9): polled single conversions, then a scan read in the EOC
 * handler on IRQ 149. ADCEN stays clear, so ADC1 must read as gated. */

#include <stdint.h>

#define ADC_BASE 0x4202D800u
#include "adc_test.h"

#define ADC3_IRQN            149
#define RCC_AHB2ENR_GPIOEEN  (1u << 4)
#define RCC_AHB2ENR_ADC3EN   (1u << 24)
#define GPIOE_BASE           0x42021000u
#define ADC1_CR              REG32(0x42028008u)

#define RANKS  3u
#define PASSES 3u

static const uint32_t g_seq[RANKS] = { 0u, 1u, 9u };
static const uint32_t g_expected[RANKS] = { CODE_IN0, CODE_IN1, CODE_IN3 };

static volatile uint32_t g_codes[RANKS];
static volatile uint32_t g_count;
static volatile uint32_t g_done;
static volatile uint32_t g_errors;

void ADC3_Handler(void)
{
    uint32_t isr = ADC_ISR;
    uint32_t ier = ADC_IER;
    uint32_t code;

    if ((isr & ier & (ISR_EOC | ISR_EOS)) != 0u) {
        if ((ADC_ISR & ISR_EOS) != 0u) {
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

static void start_it(void)
{
    g_count = 0u;
    g_done = 0u;
    ADC_ISR = ISR_EOC | ISR_EOS | ISR_OVR;
    ADC_IER &= ~(IER_EOCIE | IER_EOSIE | IER_OVRIE);
    ADC_IER |= IER_EOCIE | IER_OVRIE;
    cr_set(CR_ADSTART);
}

static void convert_polled(uint32_t ch, uint32_t expected)
{
    adc_sequence(&ch, 1u, SMP_92_5);
    ADC_ISR = ISR_ALL;
    cr_set(CR_ADSTART);
    adc_wait_isr(ISR_EOC);
    if ((ADC_CSR & ISR_EOC) == 0u) test_fail();
    if (ADC_DR != expected) test_fail();
    adc_wait_cr_clear(CR_ADSTART);
}

void Reset_Handler(void)
{
    uint32_t pass;
    uint32_t i;

    crt_init();
    cyc_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOEEN | RCC_AHB2ENR_ADC3EN;
    GPIO_MODER(GPIOB_BASE) |= 0x3u << 4;
    GPIO_MODER(GPIOE_BASE) |= (0x3u << 14) | (0x3u << 30);
    adc_core_init(3u, 0u);
    if (ADC1_CR != 0u) test_fail();

    signal_trigger_marker();

    for (pass = 0; pass < PASSES; ++pass) {
        for (i = 0; i < RANKS; ++i) {
            convert_polled(g_seq[i], g_expected[i]);
        }
    }

    adc_sequence(g_seq, RANKS, SMP_92_5);
    nvic_enable_irq(ADC3_IRQN);
    enable_irqs();
    for (pass = 0; pass < PASSES; ++pass) {
        start_it();
        wfi_until(&g_done);
        if (g_count != RANKS || g_errors != 0u || (ADC_CR & CR_ADSTART) != 0u) test_fail();
        for (i = 0; i < RANKS; ++i) {
            if (g_codes[i] != g_expected[i]) test_fail();
        }
    }
    test_pass();
}
