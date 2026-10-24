/* Web workbench PWM demo on the NUCLEO-H563ZI.
 *
 * TIM3_CH3 on PB0 (AF2) drives the on-board green LED LD1 (also CN10-31,
 * morpho CN11-34) at 1 kHz with a duty cycle that ramps up and down over two
 * seconds. TIM4_CH4 on PD15 (AF2, Zio CN7-18 "D9") is a fixed 1 kHz, 25 %
 * PWM for an external RC filter or LED. Timers run from the 64 MHz core
 * clock through PSC = 63, so one counter tick is 1 us. */

#include <stdint.h>
#include "../_signal_common/signal_test.h"

#define GPIOD_BASE   0x42020C00u
#define GPIO_AFRL(b) REG32((b) + 0x20u)
#define GPIO_AFRH(b) REG32((b) + 0x24u)
#define RCC_APB1LENR REG32(RCC_BASE + 0x9Cu)
#define RCC_AHB2ENR_GPIODEN (1u << 3)
#define TIM3_BASE    0x40000400u
#define TIM4_BASE    0x40000800u
#define TIM_CR1(b)   REG32((b) + 0x00u)
#define TIM_EGR(b)   REG32((b) + 0x14u)
#define TIM_CCMR2(b) REG32((b) + 0x1Cu)
#define TIM_CCER(b)  REG32((b) + 0x20u)
#define TIM_PSC(b)   REG32((b) + 0x28u)
#define TIM_ARR(b)   REG32((b) + 0x2Cu)
#define TIM_CCR3(b)  REG32((b) + 0x3Cu)
#define TIM_CCR4(b)  REG32((b) + 0x40u)
#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)

#define PERIOD_US 1000u
#define STEP_US   20u   /* duty change per 20 ms SysTick */

static volatile uint32_t ticks;

void SysTick_Handler(void)
{
    ticks++;
}

static void pin_af(uint32_t base, int pin, uint32_t af)
{
    volatile uint32_t *afr = pin < 8 ? &GPIO_AFRL(base) : &GPIO_AFRH(base);
    int shift = (pin & 7) * 4;
    *afr = (*afr & ~(0xFu << shift)) | (af << shift);
    GPIO_MODER(base) = (GPIO_MODER(base) & ~(3u << (pin * 2))) | (2u << (pin * 2));
}

static void pwm_start(uint32_t tim, int channel, uint32_t ccr)
{
    TIM_PSC(tim) = 63u;
    TIM_ARR(tim) = PERIOD_US - 1u;
    if (channel == 3) {
        TIM_CCR3(tim) = ccr;
        TIM_CCMR2(tim) = (TIM_CCMR2(tim) & ~0x00FFu) | (6u << 4) | (1u << 3); /* PWM1, preload */
        TIM_CCER(tim) |= 1u << 8;
    } else {
        TIM_CCR4(tim) = ccr;
        TIM_CCMR2(tim) = (TIM_CCMR2(tim) & ~0xFF00u) | (6u << 12) | (1u << 11);
        TIM_CCER(tim) |= 1u << 12;
    }
    TIM_EGR(tim) = 1u;
    TIM_CR1(tim) = 1u | (1u << 7); /* CEN, ARPE */
}

void Reset_Handler(void)
{
    uint32_t duty = 0u;
    int up = 1;
    crt_init();
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIODEN;
    RCC_APB1LENR |= (1u << 1) | (1u << 2); /* TIM3, TIM4 */
    pin_af(GPIOB_BASE, 0, 2u);
    pin_af(GPIOD_BASE, 15, 2u);
    pwm_start(TIM4_BASE, 4, PERIOD_US / 4u);
    pwm_start(TIM3_BASE, 3, 0u);
    SYST_RVR = 64000u * 20u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 0x7u;
    for (;;) {
        __asm volatile("wfi");
        if (ticks == 0u) continue;
        ticks = 0u;
        if (up) {
            duty += STEP_US;
            if (duty >= PERIOD_US) { duty = PERIOD_US; up = 0; }
        } else {
            duty = duty >= STEP_US ? duty - STEP_US : 0u;
            if (duty == 0u) up = 1;
        }
        TIM_CCR3(TIM3_BASE) = duty;
    }
}
