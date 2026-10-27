/* ADC1 scans IN3, IN0, IN7, IN1 continuously with DMACFG = 1 into a buffer of
 * two sequences through a self-linked GPDMA1 channel 0 node. Each HT/TC
 * interrupt checks and clears its half, so the second lap must rewrite it. */

#include <stdint.h>
#include "dma_test.h"

#define COUNT (2u * SCAN_RANKS)
#define LAPS  2u

static volatile uint16_t g_buf[COUNT];
static volatile uint32_t g_node[6];
static volatile uint32_t g_ht;
static volatile uint32_t g_tc;
static volatile uint32_t g_done;
static volatile uint32_t g_errors;

static void check_and_clear(uint32_t first)
{
    uint32_t i;

    for (i = 0; i < SCAN_RANKS; ++i) {
        if (g_buf[first + i] != g_scan_codes[i]) {
            g_errors++;
        }
        g_buf[first + i] = 0u;
    }
}

void GPDMA1_CH0_Handler(void)
{
    uint32_t sr = DMA_CSR;

    if ((sr & SR_HTF) != 0u) {
        DMA_CFCR = SR_HTF;
        if (g_ht != g_tc) g_errors++;
        check_and_clear(0u);
        g_ht++;
    }
    if ((sr & SR_TCF) != 0u) {
        DMA_CFCR = SR_TCF;
        if (g_ht != g_tc + 1u) g_errors++;
        check_and_clear(SCAN_RANKS);
        g_tc++;
        if (g_tc == LAPS) {
            g_done = 1u;
        }
    }
    if ((sr & SR_ERRORS) != 0u) {
        DMA_CFCR = SR_ERRORS;
        g_errors++;
    }
}

/* HAL_DMAEx_List_Start_IT with a circular queue of one node. */
static void dma_list_start(uint32_t ie)
{
    uint32_t lli = DMA_ADDR(g_node);

    g_node[0] = TR1_ADC_TO_SRAM;
    g_node[1] = TR2_ADC1;
    g_node[2] = COUNT * 2u;
    g_node[3] = ADC_DR_ADDR;
    g_node[4] = DMA_ADDR(g_buf);
    g_node[5] = LLR_ALL | (lli & 0xFFFCu);
    DMA_CTR1 = 0u;
    DMA_CTR2 = 0u;
    DMA_CFCR = SR_ALL;
    DMA_CCR |= ie;
    DMA_CBR1 = 0u;
    DMA_CLBAR = lli & 0xFFFF0000u;
    DMA_CLLR = LLR_ALL | (lli & 0xFFFCu);
    DMA_CCR |= CCR_EN;
}

void Reset_Handler(void)
{
    adc_dma_board_init(CFGR_CONT | CFGR_DMACFG);
    nvic_enable_irq(GPDMA1_CH0_IRQN);
    enable_irqs();

    signal_trigger_marker();

    /* HAL_ADC_Start_DMA */
    ADC_ISR = ISR_EOC | ISR_EOS | ISR_OVR;
    ADC_CFGR |= CFGR_DMAEN;
    dma_list_start(CCR_HTIE | CCR_TCIE | CCR_ERRIE);
    if ((DMA_CCR & CCR_EN) == 0u || (DMA_CBR1 & BR1_BNDT) != COUNT * 2u) test_fail();
    if (DMA_CSAR != ADC_DR_ADDR || DMA_CDAR != DMA_ADDR(g_buf) || (DMA_CSR & SR_ALL) != 0u) test_fail();
    cr_set(CR_ADSTART);
    wfi_until(&g_done);

    /* HAL_ADC_Stop_DMA: a circular list never completes, so the channel is aborted. */
    adc_stop();
    if ((DMA_CCR & CCR_EN) == 0u || (ADC_ISR & ISR_OVR) != 0u) test_fail();
    dma_abort();
    ADC_CFGR &= ~CFGR_DMAEN;
    if ((DMA_CCR & CCR_EN) != 0u || (DMA_CSR & SR_IDLEF) == 0u) test_fail();
    if (g_errors != 0u || g_ht != LAPS || g_tc != LAPS) test_fail();
    test_pass();
}
