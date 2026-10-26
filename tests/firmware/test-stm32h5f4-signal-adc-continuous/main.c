/* Continuous conversions of IN0. The period follows SMPR and CKMODE, and an
 * unread result overruns: at the 4th result with OVRMOD = 0 (three-stage
 * FIFO), at the 2nd with OVRMOD = 1. */

#include <stdint.h>
#include "adc_test.h"

#define PERIOD_92_5_DIV4 420u   /* (92.5 + 12.5) * 4 */
#define PERIOD_47_5_DIV2 120u   /* (47.5 + 12.5) * 2 */
#define PERIODS          10u
#define JITTER           64u
/* The start waits for the end of its batch; then the poll loop. */
#define SLACK            160u

static void start(uint32_t cfgr, uint32_t smp, uint32_t *t0)
{
    uint32_t ch = 0u;

    ADC_CFGR = (ADC_CFGR & ~(CFGR_CONT | CFGR_OVRMOD)) | CFGR_CONT | cfgr;
    adc_sequence(&ch, 1u, smp);
    ADC_ISR = ISR_ALL;
    *t0 = cyc();
    cr_set(CR_ADSTART);
}

/* After ADSTP nothing converts any more. */
static void stop_and_check_idle(void)
{
    uint32_t i;

    adc_stop();
    for (i = 0; i < 3u; ++i) {
        (void)ADC_DR;
    }
    ADC_ISR = ISR_ALL;
    delay_until(cyc() + 2000u);
    if ((ADC_ISR & ISR_ALL) != 0u || (ADC_CR & CR_ADSTART) != 0u) test_fail();
}

static void check_rate(uint32_t smp, uint32_t period)
{
    uint32_t t0;
    uint32_t first = 0u;
    uint32_t span = 0u;
    uint32_t k;

    start(CFGR_OVRMOD, smp, &t0);
    for (k = 0; k <= PERIODS; ++k) {
        adc_wait_isr(ISR_EOC);
        span = cyc();
        if (k == 0u) {
            first = span;
        }
        if (ADC_DR != CODE_IN0) test_fail();
    }
    if ((ADC_CR & CR_ADSTART) == 0u) test_fail();
    span -= first;
    if (span < PERIODS * period - JITTER || span > PERIODS * period + JITTER) test_fail();
    stop_and_check_idle();
}

static void check_overrun(uint32_t cfgr, uint32_t at_result, uint32_t kept)
{
    uint32_t expected = at_result * PERIOD_92_5_DIV4;
    uint32_t t0;
    uint32_t dt;
    uint32_t k;

    start(cfgr, SMP_92_5, &t0);
    adc_wait_isr(ISR_OVR);
    dt = cyc() - t0;
    adc_stop();
    if (dt < expected || dt > expected + SLACK) test_fail();
    for (k = 0; k < kept; ++k) {
        if (ADC_DR != CODE_IN0) test_fail();
    }
    if ((ADC_ISR & ISR_OVR) == 0u) test_fail();
    stop_and_check_idle();
}

void Reset_Handler(void)
{
    adc_board_init(3u, 0u);

    signal_trigger_marker();

    check_rate(SMP_92_5, PERIOD_92_5_DIV4);
    check_overrun(0u, 4u, 3u);
    check_overrun(CFGR_OVRMOD, 2u, 1u);

    /* CKMODE may change only while the ADC is disabled. */
    adc_disable();
    ADC_CCR = (ADC_CCR & ~CCR_CKMODE) | (2u << 16);
    adc_enable();
    check_rate(SMP_47_5, PERIOD_47_5_DIV2);
    test_pass();
}
