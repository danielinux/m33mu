/* Polled single conversions on ADC1, with HAL's register sequence: PA0 (IN0)
 * steps 1000/2500/3000 mV, PA1 (IN1) stays at 600 mV. ADC_PIN_DIGITAL leaves
 * PA0 in input mode, where IN0 must convert 0. */

#include <stdint.h>
#include "signal_test.h"

#define ADC1_BASE           0x42028000u
#define ADC_ISR             REG32(ADC1_BASE + 0x00u)
#define ADC_CR              REG32(ADC1_BASE + 0x08u)
#define ADC_CFGR            REG32(ADC1_BASE + 0x0Cu)
#define ADC_SMPR1           REG32(ADC1_BASE + 0x14u)
#define ADC_SQR1            REG32(ADC1_BASE + 0x30u)
#define ADC_DR              REG32(ADC1_BASE + 0x40u)
#define ADC_DIFSEL          REG32(ADC1_BASE + 0xB0u)
#define ADC_OR              REG32(ADC1_BASE + 0xC8u)
#define ADC12_CCR           REG32(ADC1_BASE + 0x308u)
#define RCC_AHB2ENR_ADCEN   (1u << 10)

#define CR_ADEN     (1u << 0)
#define CR_ADDIS    (1u << 1)
#define CR_ADSTART  (1u << 2)
#define CR_JADSTART (1u << 3)
#define CR_ADSTP    (1u << 4)
#define CR_JADSTP   (1u << 5)
#define CR_ADVREGEN (1u << 28)
#define CR_DEEPPWD  (1u << 29)
#define CR_ADCALDIF (1u << 30)
#define CR_ADCAL    (1u << 31)
#define CR_RS       (CR_ADCAL | CR_JADSTP | CR_ADSTP | CR_JADSTART | CR_ADSTART | CR_ADDIS | CR_ADEN)

#define ISR_ADRDY   (1u << 0)
#define ISR_EOC     (1u << 2)
#define ISR_EOS     (1u << 3)
#define ISR_OVR     (1u << 4)

#define CFGR_RES    (3u << 3)
#define CFGR_ALIGN  (1u << 15)
#define CFGR_CONT   (1u << 13)
#define CFGR_OVRMOD (1u << 12)
#define CCR_CKMODE  (3u << 16)
#define CCR_PRESC   (0xFu << 18)
#define SMP_24_5    3u

#define POLL_MAX    20000u
#define ITERATIONS  4000u
#define GUARD_ITERATIONS 400u
#ifdef ADC_VREF_2900
#define CODE_IN1    847u
#else
#define CODE_IN1    745u
#endif
#define NO_CODE     0xFFFFFFFFu

static void cr_set(uint32_t bit)
{
    ADC_CR = (ADC_CR & ~CR_RS) | bit;
}

static void adc_init(void)
{
    volatile uint32_t n;

    ADC_CR = ADC_CR & ~(CR_DEEPPWD | CR_RS);
    cr_set(CR_ADVREGEN);
    for (n = 0; n < 642u; ++n) {
    }
    if ((ADC_CR & CR_ADVREGEN) == 0u) test_fail();
    ADC12_CCR = (ADC12_CCR & ~(CCR_CKMODE | CCR_PRESC)) | (3u << 16);
    ADC_CFGR = (ADC_CFGR & ~(CFGR_RES | CFGR_ALIGN | CFGR_CONT)) | CFGR_OVRMOD;
    ADC_SQR1 &= ~0xFu;
}

static void adc_calibrate(void)
{
    uint32_t n;

    ADC_CR = (ADC_CR & ~(CR_RS | CR_ADCALDIF)) | CR_ADCAL;
    for (n = 0; (ADC_CR & CR_ADCAL) != 0u; ++n) {
        if (n > POLL_MAX) test_fail();
    }
}

static void adc_config_channel(uint32_t ch)
{
    ADC_SQR1 = (ADC_SQR1 & ~(0x1Fu << 6)) | (ch << 6);
    ADC_SMPR1 = (ADC_SMPR1 & ~(0x7u << (ch * 3u))) | (SMP_24_5 << (ch * 3u));
}

/* DIFSEL and OR are written only while ADEN = 0, as HAL does. */
static void adc_config_inputs(void)
{
    ADC_DIFSEL &= ~0x3u;
    ADC_OR |= 1u;
}

static void adc_enable(void)
{
    uint32_t n;

    cr_set(CR_ADEN);
    for (n = 0; (ADC_ISR & ISR_ADRDY) == 0u; ++n) {
        if ((ADC_CR & CR_ADEN) == 0u) {
            cr_set(CR_ADEN);
        }
        if (n > POLL_MAX) test_fail();
    }
}

static uint32_t adc_convert(void)
{
    uint32_t n;

    ADC_ISR = ISR_EOC | ISR_EOS | ISR_OVR;
    cr_set(CR_ADSTART);
    for (n = 0; (ADC_ISR & ISR_EOC) == 0u; ++n) {
        if (n > POLL_MAX) return NO_CODE;
    }
    ADC_ISR = ISR_EOC | ISR_EOS;
    return ADC_DR;
}

static uint32_t convert_channel(uint32_t ch)
{
    adc_config_channel(ch);
    return adc_convert();
}

#ifdef ADC_PIN_DIGITAL
static void run_conversions(void)
{
    uint32_t i;

    for (i = 0; i < GUARD_ITERATIONS; ++i) {
        if (convert_channel(0u) != 0u) test_fail();
        if (convert_channel(1u) != CODE_IN1) test_fail();
    }
    test_pass();
}
#else
#ifdef ADC_VREF_2900
/* round(mV * 4095 / 2900), no value near .5; 3000 mV clamps */
static const uint32_t g_codes_in0[3] = { 1412u, 3530u, 4095u };
#else
/* round(mV * 4095 / 3300) */
static const uint32_t g_codes_in0[3] = { 1241u, 3102u, 3723u };
#endif

/* Pass after 8 conversions on the last segment; every code must be the
 * current segment's or the next one's. */
static void run_conversions(void)
{
    uint32_t i;
    uint32_t idx = 0u;
    uint32_t on_last = 0u;
    uint32_t code;

    for (i = 0; i < ITERATIONS; ++i) {
        code = convert_channel(0u);
        if (code != g_codes_in0[idx]) {
            if (idx >= 2u || code != g_codes_in0[idx + 1u]) test_fail();
            idx++;
        }
        if (convert_channel(1u) != CODE_IN1) test_fail();
        if (idx == 2u && ++on_last >= 8u) test_pass();
    }
    test_fail();
}
#endif

void Reset_Handler(void)
{
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_ADCEN;
#ifdef ADC_PIN_DIGITAL
    GPIO_MODER(GPIOA_BASE) = (GPIO_MODER(GPIOA_BASE) & ~0xFu) | (0x3u << 2);
#else
    GPIO_MODER(GPIOA_BASE) |= 0xFu;
#endif

    adc_init();
    adc_calibrate();
    adc_config_inputs();
    adc_config_channel(0u);
    adc_enable();

    signal_trigger_marker();
    run_conversions();
}
