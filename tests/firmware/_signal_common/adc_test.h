/* ADC registers (ADC1 unless ADC_BASE is defined first) and HAL-style helpers
 * for the H5F4 adc-* tests that bind fixture_adc_scan.sigc: PA0 (IN0)
 * 1000 mV, PA1 (IN1) 2500 mV, PA6 (IN3) 300 mV, all flat. */
#ifndef ADC_TEST_H
#define ADC_TEST_H

#include <stdint.h>
#include "signal_test.h"

#ifndef ADC_BASE
#define ADC_BASE            0x42028000u
#endif
#define ADC_ISR             REG32(ADC_BASE + 0x00u)
#define ADC_IER             REG32(ADC_BASE + 0x04u)
#define ADC_CR              REG32(ADC_BASE + 0x08u)
#define ADC_CFGR            REG32(ADC_BASE + 0x0Cu)
#define ADC_SMPR1           REG32(ADC_BASE + 0x14u)
#define ADC_SQR1            REG32(ADC_BASE + 0x30u)
#define ADC_DR              REG32(ADC_BASE + 0x40u)
#define ADC_DIFSEL          REG32(ADC_BASE + 0xB0u)
#define ADC_OR              REG32(ADC_BASE + 0xC8u)
#define ADC_CSR             REG32(ADC_BASE + 0x300u)
#define ADC_CCR             REG32(ADC_BASE + 0x308u)
#define RCC_AHB2ENR_ADCEN   (1u << 10)
#define ADC1_IRQN           37

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
#define ISR_EOSMP   (1u << 1)
#define ISR_EOC     (1u << 2)
#define ISR_EOS     (1u << 3)
#define ISR_OVR     (1u << 4)
#define ISR_ALL     (ISR_EOSMP | ISR_EOC | ISR_EOS | ISR_OVR)
#define IER_EOCIE   ISR_EOC
#define IER_EOSIE   ISR_EOS
#define IER_OVRIE   ISR_OVR

#define CFGR_RES    (3u << 3)
#define CFGR_OVRMOD (1u << 12)
#define CFGR_CONT   (1u << 13)
#define CFGR_ALIGN  (1u << 15)
#define CCR_CKMODE  (3u << 16)
#define CCR_PRESC   (0xFu << 18)

#define SMP_47_5    4u
#define SMP_92_5    5u
#define SMP_247_5   6u
#define SMP_640_5   7u

#define DEMCR       REG32(0xE000EDFCu)
#define DWT_CTRL    REG32(0xE0001000u)
#define DWT_CYCCNT  REG32(0xE0001004u)
#define SYST_CSR    REG32(0xE000E010u)
#define SYST_RVR    REG32(0xE000E014u)
#define SYST_CVR    REG32(0xE000E018u)
#define SCB_ICSR    REG32(0xE000ED04u)
#define ICSR_PENDSTCLR (1u << 25)
#define WFI_MAX     1000000u

#define CODE_IN0    1241u
#define CODE_IN1    3102u
#define CODE_IN3    372u

#define POLL_MAX    200000u

static inline void cr_set(uint32_t bit)
{
    ADC_CR = (ADC_CR & ~CR_RS) | bit;
}

static inline void cyc_init(void)
{
    DEMCR |= 1u << 24;
    DWT_CYCCNT = 0u;
    DWT_CTRL |= 1u;
}

static inline uint32_t cyc(void)
{
    return DWT_CYCCNT;
}

static inline void delay_until(uint32_t t)
{
    while ((int32_t)(cyc() - t) < 0) {
    }
}

static inline void adc_wait_isr(uint32_t flag)
{
    uint32_t n;

    for (n = 0; (ADC_ISR & flag) == 0u; ++n) {
        if (n > POLL_MAX) test_fail();
    }
}

static inline void adc_wait_cr_clear(uint32_t bit)
{
    uint32_t n;

    for (n = 0; (ADC_CR & bit) != 0u; ++n) {
        if (n > POLL_MAX) test_fail();
    }
}

static inline void adc_calibrate(void)
{
    ADC_CR = (ADC_CR & ~(CR_RS | CR_ADCALDIF)) | CR_ADCAL;
    adc_wait_cr_clear(CR_ADCAL);
}

/* ADEN is ignored for 4 ADC clocks after ADCAL clears, so HAL retries it. */
static inline void adc_enable(void)
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

static inline void adc_disable(void)
{
    cr_set(CR_ADDIS);
    ADC_ISR = ISR_EOSMP | ISR_ADRDY;
    adc_wait_cr_clear(CR_ADEN);
}

static inline void adc_stop(void)
{
    if ((ADC_CR & CR_ADSTART) != 0u) {
        cr_set(CR_ADSTP);
        adc_wait_cr_clear(CR_ADSTART);
    }
}

/* SQR1 holds up to four ranks; every channel gets the same sampling time. */
static inline void adc_sequence(const uint32_t *ch, uint32_t count, uint32_t smp)
{
    uint32_t sqr = count - 1u;
    uint32_t i;

    for (i = 0; i < count; ++i) {
        sqr |= ch[i] << (6u * (i + 1u));
        ADC_SMPR1 = (ADC_SMPR1 & ~(0x7u << (3u * ch[i]))) | (smp << (3u * ch[i]));
    }
    ADC_SQR1 = sqr;
}

/* Clock already on: regulator, CKMODE, CFGR, calibration, enable. */
static inline void adc_core_init(uint32_t ckmode, uint32_t cfgr)
{
    volatile uint32_t n;

    ADC_CR = ADC_CR & ~(CR_DEEPPWD | CR_RS);
    cr_set(CR_ADVREGEN);
    for (n = 0; n < 642u; ++n) {
    }
    ADC_CCR = (ADC_CCR & ~(CCR_CKMODE | CCR_PRESC)) | (ckmode << 16);
    ADC_CFGR = (ADC_CFGR & ~(CFGR_RES | CFGR_ALIGN | CFGR_CONT | CFGR_OVRMOD)) | cfgr;
    adc_calibrate();
    ADC_DIFSEL &= ~0xBu;
    ADC_OR |= 1u;
    adc_enable();
}

/* GPIOA and ADC1/2 clocks, PA0/PA1/PA6 analog, ADC1 calibrated and enabled. */
static inline void adc_board_init(uint32_t ckmode, uint32_t cfgr)
{
    crt_init();
    cyc_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_ADCEN;
    GPIO_MODER(GPIOA_BASE) |= 0xFu | (0x3u << 12);
    adc_core_init(ckmode, cfgr);
}

/* Fires only when wfi_until runs out of WFI_MAX cycles. */
void SysTick_Handler(void)
{
    test_fail();
}

/* "cpsid i; wfi; cpsie i" cannot miss a wake-up that lands before the WFI. */
static inline void wfi_until(volatile uint32_t *flag)
{
    __asm volatile("cpsid i" ::: "memory");
    SYST_RVR = WFI_MAX - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 0x7u;
    while (*flag == 0u) {
        __asm volatile("wfi");
        __asm volatile("cpsie i" ::: "memory");
        __asm volatile("isb");
        __asm volatile("cpsid i" ::: "memory");
    }
    SYST_CSR = 0u;
    SCB_ICSR = ICSR_PENDSTCLR;
    __asm volatile("cpsie i" ::: "memory");
}

#endif /* ADC_TEST_H */
