/* DMAEN = 1 with no GPDMA channel enabled: the continuous scan of IN3, IN0,
 * IN7, IN1 overruns at the 4th result. A channel enabled afterwards moves
 * nothing while OVR is set; clearing OVR lets it drain the three kept results. */

#include <stdint.h>
#include "dma_test.h"

#define COUNT 4u
#define KEPT  3u
/* The start waits for the end of its batch; then the poll loop. */
#define SLACK 160u

static volatile uint16_t g_buf[COUNT];

void Reset_Handler(void)
{
    uint32_t expected = (KEPT + 1u) * SLOT_92_5;
    uint32_t t0;
    uint32_t dt;
    uint32_t i;

    adc_dma_board_init(CFGR_CONT | CFGR_DMACFG);
    dma_init_adc();
    for (i = 0; i < COUNT; ++i) {
        g_buf[i] = 0xFFFFu;
    }

    signal_trigger_marker();

    ADC_ISR = ISR_ALL;
    ADC_CFGR |= CFGR_DMAEN;
    t0 = cyc();
    cr_set(CR_ADSTART);
    adc_wait_isr(ISR_OVR);
    dt = cyc() - t0;
    adc_stop();
    if (dt < expected || dt > expected + SLACK) test_fail();
    if ((ADC_ISR & ISR_EOC) == 0u) test_fail();
    if ((DMA_CCR & CCR_EN) != 0u || DMA_CSR != SR_IDLEF) test_fail();

    dma_start(g_buf, COUNT, 0u);
    if ((DMA_CCR & CCR_EN) == 0u || (DMA_CBR1 & BR1_BNDT) != COUNT * 2u || g_buf[0] != 0xFFFFu) test_fail();
    ADC_ISR = ISR_OVR;
    if ((DMA_CBR1 & BR1_BNDT) != (COUNT - KEPT) * 2u || (ADC_ISR & (ISR_EOC | ISR_OVR)) != 0u) test_fail();
    for (i = 0; i < KEPT; ++i) {
        if (g_buf[i] != g_scan_codes[i]) test_fail();
    }
    if (g_buf[KEPT] != 0xFFFFu || (DMA_CCR & CCR_EN) == 0u) test_fail();

    /* HAL_ADC_Stop_DMA with the transfer still running. */
    dma_abort();
    ADC_CFGR &= ~CFGR_DMAEN;
    if ((DMA_CCR & CCR_EN) != 0u) test_fail();
    test_pass();
}
