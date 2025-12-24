/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#include <stdint.h>
#include <stddef.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;

/* int ISA_SWEEPER_Run(void); */

#define SYSCLK_HZ 64000000u

/* RCC */
#define RCC_BASE          0x44020C00u
#define RCC_AHB2ENR       (*(volatile uint32_t *)(RCC_BASE + 0x8Cu))
#define RCC_APB1LENR      (*(volatile uint32_t *)(RCC_BASE + 0x9Cu))

/* GPIOD */
#define GPIOD_BASE        0x42020C00u
#define GPIO_MODER(x)     (*(volatile uint32_t *)((x) + 0x00u))
#define GPIO_OTYPER(x)    (*(volatile uint32_t *)((x) + 0x04u))
#define GPIO_OSPEEDR(x)   (*(volatile uint32_t *)((x) + 0x08u))
#define GPIO_PUPDR(x)     (*(volatile uint32_t *)((x) + 0x0Cu))
#define GPIO_IDR(x)       (*(volatile uint32_t *)((x) + 0x10u))
#define GPIO_ODR(x)       (*(volatile uint32_t *)((x) + 0x14u))
#define GPIO_BSRR(x)      (*(volatile uint32_t *)((x) + 0x18u))
#define GPIO_AFRL(x)      (*(volatile uint32_t *)((x) + 0x20u))
#define GPIO_AFRH(x)      (*(volatile uint32_t *)((x) + 0x24u))

/* USART3 */
#define USART3_BASE       0x40004800u
#define USART_CR1(b)      (*(volatile uint32_t *)((b) + 0x00u))
#define USART_CR2(b)      (*(volatile uint32_t *)((b) + 0x04u))
#define USART_CR3(b)      (*(volatile uint32_t *)((b) + 0x08u))
#define USART_BRR(b)      (*(volatile uint32_t *)((b) + 0x0Cu))
#define USART_ISR(b)      (*(volatile uint32_t *)((b) + 0x1Cu))
#define USART_ICR(b)      (*(volatile uint32_t *)((b) + 0x20u))
#define USART_RDR(b)      (*(volatile uint32_t *)((b) + 0x24u))
#define USART_TDR(b)      (*(volatile uint32_t *)((b) + 0x28u))

/* SysTick */
#define SYST_CSR          (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR          (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR          (*(volatile uint32_t *)0xE000E018u)

/* TIM2..TIM5 */
#define TIM2_BASE         0x40000000u
#define TIM3_BASE         0x40000400u
#define TIM4_BASE         0x40000800u
#define TIM5_BASE         0x40000C00u
#define TIM_CR1(b)        (*(volatile uint32_t *)((b) + 0x00u))
#define TIM_DIER(b)       (*(volatile uint32_t *)((b) + 0x0Cu))
#define TIM_SR(b)         (*(volatile uint32_t *)((b) + 0x10u))
#define TIM_EGR(b)        (*(volatile uint32_t *)((b) + 0x14u))
#define TIM_CNT(b)        (*(volatile uint32_t *)((b) + 0x24u))
#define TIM_PSC(b)        (*(volatile uint32_t *)((b) + 0x28u))
#define TIM_ARR(b)        (*(volatile uint32_t *)((b) + 0x2Cu))

#define TIM_CR1_CEN       (1u << 0)
#define TIM_DIER_UIE      (1u << 0)
#define TIM_SR_UIF        (1u << 0)
#define TIM_EGR_UG        (1u << 0)

/* NVIC (for USART3 IRQ if needed later) */
#define NVIC_ISER1        (*(volatile uint32_t *)0xE000E104u)

static volatile uint32_t global_counter = 0;
static uint32_t static_buf[4] = { 1u, 2u, 3u, 4u };
static uint32_t zero_buf[4];
static volatile uint32_t systick_ms = 0;
static volatile uint32_t tim2_ticks = 0;
static volatile uint32_t tim3_ticks = 0;
static volatile uint32_t tim4_ticks = 0;
static volatile uint32_t tim5_ticks = 0;

static uint32_t add_pair(uint32_t a, uint32_t b)
{
    return a + b;
}

static void touch_stack(uint32_t *sp_before, uint32_t *sp_after)
{
    uint32_t tmp = 0xAA55AA55u;
    *sp_before = (uint32_t)__builtin_frame_address(0);
    tmp ^= 0x11111111u;
    *sp_after = (uint32_t)__builtin_frame_address(0);
    global_counter += tmp;
}

static void tests(void)
{
    uint32_t sp1 = 0;
    uint32_t sp2 = 0;
    uint32_t sum = 0;

    sum = add_pair(static_buf[0], static_buf[3]);
    static_buf[1] = sum;

    touch_stack(&sp1, &sp2);

    if (sp1 != 0u) {
        global_counter += 1u;
    }
    if (sp2 != 0u) {
        global_counter += 1u;
    }

    if (zero_buf[0] == 0 && zero_buf[3] == 0) {
        global_counter += 1u;
    }

    /* ISA sweeper disabled for now */
}

static void gpio_config_usart3_pd8_pd9(void)
{
    uint32_t v;
    /* Enable GPIOD clock (AHB2ENR bit3) */
    RCC_AHB2ENR |= (1u << 3);
    /* PD8/PD9 to AF7 */
    v = GPIO_MODER(GPIOD_BASE);
    v &= ~((3u << (8u * 2u)) | (3u << (9u * 2u)));
    v |= (2u << (8u * 2u)) | (2u << (9u * 2u)); /* AF mode */
    GPIO_MODER(GPIOD_BASE) = v;

    v = GPIO_OTYPER(GPIOD_BASE);
    v &= ~((1u << 8) | (1u << 9));
    GPIO_OTYPER(GPIOD_BASE) = v;

    v = GPIO_OSPEEDR(GPIOD_BASE);
    v &= ~((3u << (8u * 2u)) | (3u << (9u * 2u)));
    v |= (2u << (8u * 2u)) | (2u << (9u * 2u)); /* high speed */
    GPIO_OSPEEDR(GPIOD_BASE) = v;

    v = GPIO_PUPDR(GPIOD_BASE);
    v &= ~((3u << (8u * 2u)) | (3u << (9u * 2u)));
    v |= (1u << (9u * 2u)); /* pull-up RX, none TX */
    GPIO_PUPDR(GPIOD_BASE) = v;

    /* AFRH pins 8..15 */
    v = GPIO_AFRH(GPIOD_BASE);
    v &= ~((0xFu << ((8u - 8u) * 4u)) | (0xFu << ((9u - 8u) * 4u)));
    v |= (7u << ((8u - 8u) * 4u)) | (7u << ((9u - 8u) * 4u));
    GPIO_AFRH(GPIOD_BASE) = v;
}

static void usart3_init_115200(void)
{
    uint32_t brr;
    /* Enable USART3 clock (APB1LENR bit18) */
    RCC_APB1LENR |= (1u << 18);
    /* Disable before config */
    USART_CR1(USART3_BASE) = 0;
    USART_CR2(USART3_BASE) = 0;
    USART_CR3(USART3_BASE) = 0;
    /* Baudrate */
    brr = SYSCLK_HZ / 115200u;
    USART_BRR(USART3_BASE) = brr;
    /* 8N1: UE, TE, RE */
    USART_CR1(USART3_BASE) = (1u << 0) | (1u << 2) | (1u << 3);
}

void SysTick_Handler(void)
{
    systick_ms++;
}

void TIM2_IRQHandler(void)
{
    TIM_SR(TIM2_BASE) &= ~TIM_SR_UIF;
    tim2_ticks++;
}

void TIM3_IRQHandler(void)
{
    TIM_SR(TIM3_BASE) &= ~TIM_SR_UIF;
    tim3_ticks++;
}

void TIM4_IRQHandler(void)
{
    TIM_SR(TIM4_BASE) &= ~TIM_SR_UIF;
    tim4_ticks++;
}

void TIM5_IRQHandler(void)
{
    TIM_SR(TIM5_BASE) &= ~TIM_SR_UIF;
    tim5_ticks++;
}

static void systick_init_1ms(void)
{
    /* Use processor clock, 1ms tick */
    SYST_CSR = 0;
    SYST_RVR = (SYSCLK_HZ / 1000u) - 1u;
    SYST_CVR = 0;
    SYST_CSR = (1u << 0) | (1u << 1) | (1u << 2); /* ENABLE | TICKINT | CLKSOURCE */
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = systick_ms;
    while ((uint32_t)(systick_ms - start) < ms) {
        __asm volatile("wfi");
    }
}

static void tim_init_basic(void)
{
    uint32_t psc = (SYSCLK_HZ / 1000000u) - 1u; /* 1 MHz timer clock */
    /* Enable TIM2-5 clocks */
    RCC_APB1LENR |= (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);

    /* TIM2: 10 ms */
    TIM_CR1(TIM2_BASE) = 0;
    TIM_PSC(TIM2_BASE) = psc;
    TIM_ARR(TIM2_BASE) = 10000u - 1u;
    TIM_EGR(TIM2_BASE) = TIM_EGR_UG;
    TIM_DIER(TIM2_BASE) = TIM_DIER_UIE;
    TIM_CR1(TIM2_BASE) = TIM_CR1_CEN;

    /* TIM3: 20 ms */
    TIM_CR1(TIM3_BASE) = 0;
    TIM_PSC(TIM3_BASE) = psc;
    TIM_ARR(TIM3_BASE) = 20000u - 1u;
    TIM_EGR(TIM3_BASE) = TIM_EGR_UG;
    TIM_DIER(TIM3_BASE) = TIM_DIER_UIE;
    TIM_CR1(TIM3_BASE) = TIM_CR1_CEN;

    /* TIM4: 40 ms */
    TIM_CR1(TIM4_BASE) = 0;
    TIM_PSC(TIM4_BASE) = psc;
    TIM_ARR(TIM4_BASE) = 40000u - 1u;
    TIM_EGR(TIM4_BASE) = TIM_EGR_UG;
    TIM_DIER(TIM4_BASE) = TIM_DIER_UIE;
    TIM_CR1(TIM4_BASE) = TIM_CR1_CEN;

    /* TIM5: 80 ms */
    TIM_CR1(TIM5_BASE) = 0;
    TIM_PSC(TIM5_BASE) = psc;
    TIM_ARR(TIM5_BASE) = 80000u - 1u;
    TIM_EGR(TIM5_BASE) = TIM_EGR_UG;
    TIM_DIER(TIM5_BASE) = TIM_DIER_UIE;
    TIM_CR1(TIM5_BASE) = TIM_CR1_CEN;

    /* Enable TIM2..TIM5 IRQs in NVIC (IRQs 45-48) */
    NVIC_ISER1 = (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16);
}

static void usart3_putc(char c)
{
    while ((USART_ISR(USART3_BASE) & (1u << 7)) == 0u) {
    }
    USART_TDR(USART3_BASE) = (uint32_t)c;
}

static void usart3_write(const char *s)
{
    while (*s) {
        usart3_putc(*s++);
    }
}

int main(void)
{
    gpio_config_usart3_pd8_pd9();
    usart3_init_115200();
    systick_init_1ms();
    delay_ms(2000u);
    usart3_write("Test started.\n\rTesting timers...");
    tests();
    tim_init_basic();
    {
        uint32_t last = 0;
        uint32_t prints = 0;
        uint32_t timers_ok = 0;
        for (;;) {
            uint32_t now = systick_ms;
            if (!timers_ok) {
                if (tim2_ticks >= 5u && tim3_ticks >= 3u && tim4_ticks >= 2u && tim5_ticks >= 1u) {
                    usart3_write("Timers OK\r\n");
                    timers_ok = 1u;
                }
                __asm volatile("wfi");
                continue;
            }
            if ((now - last) >= 1000u) {
                if (last == 0) {
                    usart3_write("Testing Systick...\r\n");
                }
                last = now;
                usart3_write("Hello, world!\r\n");
                prints++;
                if (prints >= 10u) {
                    usart3_write("Systick test OK!\r\n");
                    delay_ms(2000u);
                    __asm volatile("bkpt #0x7f");
                }
            }
            __asm volatile("wfi");
        }
    }
    asm volatile("bkpt #0x7e");
    while (1) {
        /* stay here */
    }

}

void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;

    /* Copy .data from flash to RAM */
    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }

    /* Zero .bss */
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }

    main();
}

void HardFault_Handler(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
        /* stay here */
    }
}

void UsageFault_Handler(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
        /* stay here */
    }
}

__attribute__((section(".vectors")))
const void *vectors[] = {
    (void *)&_estack,
    Reset_Handler,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    SysTick_Handler
};
