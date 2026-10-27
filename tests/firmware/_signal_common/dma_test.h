/* GPDMA1 channel 0 registers and HAL-style helpers for the adc-dma-* tests,
 * which bind fixture_adc_dma.sigc: the adc_test.h inputs plus PA7 (IN7)
 * 1800 mV. */
#ifndef DMA_TEST_H
#define DMA_TEST_H

#include <stdint.h>
#include "adc_test.h"

#define RCC_AHB1ENR          REG32(RCC_BASE + 0x88u)
#define RCC_AHB1ENR_GPDMA1EN (1u << 0)

#define GPDMA1_BASE     0x50020000u   /* secure alias: channel 0 is secure */
#define GPDMA1_CH0_IRQN 27
#define DMA_SECCFGR     REG32(GPDMA1_BASE + 0x00u)
#define DMA_PRIVCFGR    REG32(GPDMA1_BASE + 0x04u)
#define DMA_CH0(off)    REG32(GPDMA1_BASE + 0x50u + (off))
#define DMA_CLBAR       DMA_CH0(0x00u)
#define DMA_CFCR        DMA_CH0(0x0Cu)
#define DMA_CSR         DMA_CH0(0x10u)
#define DMA_CCR         DMA_CH0(0x14u)
#define DMA_CTR1        DMA_CH0(0x40u)
#define DMA_CTR2        DMA_CH0(0x44u)
#define DMA_CBR1        DMA_CH0(0x48u)
#define DMA_CSAR        DMA_CH0(0x4Cu)
#define DMA_CDAR        DMA_CH0(0x50u)
#define DMA_CLLR        DMA_CH0(0x7Cu)

#define CCR_EN      (1u << 0)
#define CCR_RESET   (1u << 1)
#define CCR_SUSP    (1u << 2)
#define CCR_TCIE    (1u << 8)
#define CCR_HTIE    (1u << 9)
#define CCR_ERRIE   (0x7u << 10)   /* DTEIE, ULEIE, USEIE */

#define SR_IDLEF    (1u << 0)
#define SR_TCF      (1u << 8)
#define SR_HTF      (1u << 9)
#define SR_ERRORS   (0x7u << 10)   /* DTEF, ULEF, USEF */
#define SR_SUSPF    (1u << 13)
#define SR_ALL      0x7F00u

#define SECCFGR_SEC0   (1u << 0)
#define PRIVCFGR_PRIV0 (1u << 0)

/* SDW = DDW = halfword, DINC, DSEC; SSEC = 0 as the ADC stays non-secure. */
#define TR1_ADC_TO_SRAM ((1u << 0) | (1u << 16) | (1u << 19) | (1u << 31))
#define TR2_ADC1    0u                                      /* REQSEL adc1_dma */
#define BR1_BNDT    0xFFFFu
#define LLR_ALL     ((0x1Fu << 27) | (1u << 16))            /* UT1 UT2 UB1 USA UDA, ULL */

#define CFGR_DMAEN  (1u << 0)
#define CFGR_DMACFG (1u << 1)
#define ADC_DR_ADDR (ADC_BASE + 0x40u)

#define CODE_IN7    2234u
#define SCAN_RANKS  4u
#define SLOT_92_5   420u   /* (92.5 + 12.5) * 4 */

#define DMA_ADDR(p) ((uint32_t)(uintptr_t)(p))

static const uint32_t g_scan[SCAN_RANKS] = { 3u, 0u, 7u, 1u };
static const uint16_t g_scan_codes[SCAN_RANKS] = { CODE_IN3, CODE_IN0, CODE_IN7, CODE_IN1 };

/* adc_board_init plus PA7 analog, the GPDMA1 clock, secure privileged
 * channel 0 and the 4-rank scan. */
static inline void adc_dma_board_init(uint32_t cfgr)
{
    adc_board_init(3u, cfgr);
    GPIO_MODER(GPIOA_BASE) |= 0x3u << 14;
    RCC_AHB1ENR |= RCC_AHB1ENR_GPDMA1EN;
    (void)RCC_AHB1ENR;
    DMA_SECCFGR |= SECCFGR_SEC0;
    DMA_PRIVCFGR |= PRIVCFGR_PRIV0;
    adc_sequence(g_scan, SCAN_RANKS, SMP_92_5);
}

/* HAL_DMA_Init: ADC1 DR to memory, halfwords, no linked list. */
static inline void dma_init_adc(void)
{
    DMA_CTR1 = TR1_ADC_TO_SRAM;
    DMA_CTR2 = TR2_ADC1;
    DMA_CLLR = 0u;
}

/* HAL_DMA_Start_IT */
static inline void dma_start(volatile uint16_t *buf, uint32_t count, uint32_t ie)
{
    DMA_CBR1 = (DMA_CBR1 & ~BR1_BNDT) | (count * 2u);
    DMA_CSAR = ADC_DR_ADDR;
    DMA_CDAR = DMA_ADDR(buf);
    DMA_CFCR = SR_ALL;
    DMA_CCR |= ie;
    DMA_CCR |= CCR_EN;
}

/* HAL_DMA_Abort: suspend, wait for SUSPF, reset. */
static inline void dma_abort(void)
{
    uint32_t n;

    DMA_CCR |= CCR_SUSP;
    for (n = 0; (DMA_CSR & SR_SUSPF) == 0u; ++n) {
        if (n > POLL_MAX) test_fail();
    }
    DMA_CCR |= CCR_RESET;
    DMA_CFCR = SR_ALL;
}

#endif /* DMA_TEST_H */
