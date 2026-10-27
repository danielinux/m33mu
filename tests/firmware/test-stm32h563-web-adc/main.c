/* Web workbench PWM -> RC -> ADC feedback demo on the NUCLEO-H563ZI.
 *
 * TIM4_CH4 on PD15 (AF2, Zio CN7-18 "D9") is a 1 kHz PWM. An external RC
 * filter feeds its average back into Arduino A0 (PA6, CN9-1, ADC1 IN3).
 * Every 5 ms the firmware converts A0 and nudges the duty cycle toward a
 * 1650 mV setpoint, so the loop settles at 50 % duty when VDDA = 3.3 V.
 * SysTick paces the loop by polling COUNTFLAG. Results are left in g_adc_mv, g_duty_us and g_conversions. */

#include <stdint.h>
#include "../_signal_common/adc_test.h"

#define GPIOD_BASE   0x42020C00u
#define GPIO_AFRH(b) REG32((b) + 0x24u)
#define RCC_APB1LENR REG32(RCC_BASE + 0x9Cu)
#define RCC_AHB2ENR_GPIODEN (1u << 3)
#define TIM4_BASE    0x40000800u
#define TIM_CR1(b)   REG32((b) + 0x00u)
#define TIM_EGR(b)   REG32((b) + 0x14u)
#define TIM_CCMR2(b) REG32((b) + 0x1Cu)
#define TIM_CCER(b)  REG32((b) + 0x20u)
#define TIM_PSC(b)   REG32((b) + 0x28u)
#define TIM_ARR(b)   REG32((b) + 0x2Cu)
#define TIM_CCR4(b)  REG32((b) + 0x40u)

#define PERIOD_US   1000u
#define SETPOINT_MV 1650
#define ADC_IN_A0   3u

volatile uint32_t g_adc_mv;
volatile uint32_t g_duty_us;
volatile uint32_t g_conversions;

static uint32_t adc_read_mv(void)
{
    uint32_t code;
    cr_set(CR_ADSTART);
    adc_wait_isr(ISR_EOC);
    code = ADC_DR;
    return (code * 3300u + 2047u) / 4095u;
}

void Reset_Handler(void)
{
    int32_t duty = 0;
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIODEN | RCC_AHB2ENR_ADCEN;
    RCC_APB1LENR |= 1u << 2; /* TIM4 */
    GPIO_MODER(GPIOA_BASE) |= 3u << (6 * 2); /* PA6 analog */
    GPIO_AFRH(GPIOD_BASE) = (GPIO_AFRH(GPIOD_BASE) & ~(0xFu << 28)) | (2u << 28);
    GPIO_MODER(GPIOD_BASE) = (GPIO_MODER(GPIOD_BASE) & ~(3u << 30)) | (2u << 30);

    TIM_PSC(TIM4_BASE) = 63u;
    TIM_ARR(TIM4_BASE) = PERIOD_US - 1u;
    TIM_CCR4(TIM4_BASE) = 0u;
    TIM_CCMR2(TIM4_BASE) = (6u << 12) | (1u << 11); /* PWM1, preload */
    TIM_CCER(TIM4_BASE) = 1u << 12;
    TIM_EGR(TIM4_BASE) = 1u;
    TIM_CR1(TIM4_BASE) = 1u | (1u << 7);

    adc_core_init(1u, 0u); /* CKMODE = HCLK/1, 12-bit, right aligned */
    adc_sequence((const uint32_t[]){ ADC_IN_A0 }, 1u, SMP_247_5);

    SYST_RVR = 64000u * 5u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 0x5u; /* polled: adc_test.h's SysTick_Handler fails the test */
    for (;;) {
        while ((SYST_CSR & (1u << 16)) == 0u) {
        }
        g_adc_mv = adc_read_mv();
        g_conversions++;
        /* Integral step: 1 us of duty per 8 mV of error. */
        duty += (SETPOINT_MV - (int32_t)g_adc_mv) / 8;
        if (duty < 0) duty = 0;
        if (duty > (int32_t)PERIOD_US) duty = PERIOD_US;
        g_duty_us = (uint32_t)duty;
        TIM_CCR4(TIM4_BASE) = (uint32_t)duty;
    }
}
