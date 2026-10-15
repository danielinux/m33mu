/* TIM2 CH1 on PA5 measures an injected 100 us square wave (6400 cycles at
 * 64 MHz): first capture to capture, then in PWM input mode. */

#include <stdint.h>
#include "signal_test.h"

#define PIN 5

#define GPIO_AFRL(base)     REG32((base) + 0x20u)
#define RCC_APB1LENR        REG32(RCC_BASE + 0x9Cu)
#define RCC_APB1LENR_TIM2EN (1u << 0)

#define TIM2_BASE  0x40000000u
#define TIM_CR1    REG32(TIM2_BASE + 0x00u)
#define TIM_SMCR   REG32(TIM2_BASE + 0x08u)
#define TIM_SR     REG32(TIM2_BASE + 0x10u)
#define TIM_CCMR1  REG32(TIM2_BASE + 0x18u)
#define TIM_CCER   REG32(TIM2_BASE + 0x20u)
#define TIM_PSC    REG32(TIM2_BASE + 0x28u)
#define TIM_ARR    REG32(TIM2_BASE + 0x2Cu)
#define TIM_CCR1   REG32(TIM2_BASE + 0x34u)
#define TIM_CCR2   REG32(TIM2_BASE + 0x38u)

#define TIM_CR1_CEN   (1u << 0)
#define TIM_SR_CC1IF  (1u << 1)
#define TIM_SR_CC1OF  (1u << 9)

#define PERIOD_TICKS  6400u
#define HIGH_TICKS    3200u
#define CAPTURES      5
#define WAIT_LIMIT    2000000u

static int within_one(uint32_t got, uint32_t want)
{
    return (got + 1u >= want) && (got <= want + 1u);
}

static uint32_t wait_capture(void)
{
    uint32_t i;
    for (i = 0; i < WAIT_LIMIT; ++i) {
        if ((TIM_SR & TIM_SR_CC1IF) != 0u) {
            return TIM_CCR1;
        }
    }
    test_fail();
}

void Reset_Handler(void)
{
    uint32_t prev;
    uint32_t period;
    int k;

    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC_APB1LENR |= RCC_APB1LENR_TIM2EN;
    GPIO_AFRL(GPIOA_BASE) = (GPIO_AFRL(GPIOA_BASE) & ~(0xFu << (PIN * 4))) | (1u << (PIN * 4));
    GPIO_MODER(GPIOA_BASE) = (GPIO_MODER(GPIOA_BASE) & ~(0x3u << (PIN * 2))) | (0x2u << (PIN * 2));

    TIM_PSC = 0u;
    TIM_ARR = 0xFFFFFFFFu;
    TIM_CCMR1 = 0x01u;
    TIM_CCER = 0x1u;
    TIM_CR1 = TIM_CR1_CEN;

    signal_trigger_marker();

    prev = wait_capture();
    for (k = 0; k < CAPTURES; ++k) {
        uint32_t now = wait_capture();
        if (!within_one(now - prev, PERIOD_TICKS)) {
            test_fail();
        }
        prev = now;
    }
    if ((TIM_SR & TIM_SR_CC1OF) != 0u) {
        test_fail();
    }

    /* PWM input: CH1 rising (period), CH2 = TI1 falling (pulse), reset on TI1FP1. */
    TIM_CR1 = 0u;
    TIM_CCMR1 = 0x01u | 0x0200u;
    TIM_CCER = 0x1u | 0x10u | 0x20u;
    TIM_SMCR = 0x4u | (0x5u << 4);
    TIM_SR = 0u;
    TIM_CR1 = TIM_CR1_CEN;
    (void)wait_capture();
    period = wait_capture();
    if (!within_one(period, PERIOD_TICKS) || !within_one(TIM_CCR2, HIGH_TICKS)) {
        test_fail();
    }
    test_pass();
}
