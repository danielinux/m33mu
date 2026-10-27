/* ADC1 scans IN3, IN0, IN7, IN1 continuously; GPDMA1 channel 0 moves two
 * sequences in one block with DMACFG = 0. The last beat stops the ADC: DR
 * keeps the last code, ADSTART stays set and nothing more converts. */

#include <stdint.h>
#include "dma_test.h"

#define COUNT  (2u * SCAN_RANKS)
#define PASSES 2u
#define IDLE   (10u * SLOT_92_5)

static volatile uint16_t g_buf[COUNT];
static volatile uint32_t g_ht;
static volatile uint32_t g_tc;
static volatile uint32_t g_errors;

void GPDMA1_CH0_Handler(void)
{
    uint32_t sr = DMA_CSR;

    if ((sr & SR_HTF) != 0u) {
        DMA_CFCR = SR_HTF;
        g_ht++;
    }
    if ((sr & SR_TCF) != 0u) {
        DMA_CFCR = SR_TCF;
        g_tc++;
    }
    if ((sr & SR_ERRORS) != 0u) {
        DMA_CFCR = SR_ERRORS;
        g_errors++;
    }
}

void Reset_Handler(void)
{
    uint32_t pass;
    uint32_t i;

    adc_dma_board_init(CFGR_CONT);
    dma_init_adc();
    nvic_enable_irq(GPDMA1_CH0_IRQN);
    enable_irqs();

    signal_trigger_marker();

    for (pass = 0; pass < PASSES; ++pass) {
        for (i = 0; i < COUNT; ++i) {
            g_buf[i] = 0xFFFFu;
        }
        g_ht = 0u;
        g_tc = 0u;

        /* HAL_ADC_Start_DMA */
        ADC_ISR = ISR_EOC | ISR_EOS | ISR_OVR;
        ADC_CFGR |= CFGR_DMAEN;
        dma_start(g_buf, COUNT, CCR_HTIE | CCR_TCIE | CCR_ERRIE);
        cr_set(CR_ADSTART);
        wfi_until(&g_tc);

        if (g_ht != 1u || g_tc != 1u || g_errors != 0u) test_fail();
        if ((DMA_CCR & CCR_EN) != 0u || (DMA_CSR & SR_IDLEF) == 0u || (DMA_CBR1 & BR1_BNDT) != 0u) test_fail();
        for (i = 0; i < COUNT; ++i) {
            if (g_buf[i] != g_scan_codes[i % SCAN_RANKS]) test_fail();
        }
        delay_until(cyc() + IDLE);
        if ((ADC_CR & CR_ADSTART) == 0u || (ADC_ISR & (ISR_EOC | ISR_OVR)) != 0u) test_fail();
        if (ADC_DR != g_scan_codes[SCAN_RANKS - 1u] || g_tc != 1u) test_fail();

        /* HAL_ADC_Stop_DMA */
        adc_stop();
        ADC_CFGR &= ~CFGR_DMAEN;
    }
    test_pass();
}
