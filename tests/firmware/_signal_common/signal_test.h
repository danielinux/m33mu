/* STM32H5F4 registers and helpers shared by the signal-injection tests. */
#ifndef SIGNAL_TEST_H
#define SIGNAL_TEST_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define GPIOA_BASE          0x42020000u
#define GPIOB_BASE          0x42020400u
#define GPIO_MODER(base)    REG32((base) + 0x00u)
#define GPIO_IDR(base)      REG32((base) + 0x10u)
#define GPIO_ODR(base)      REG32((base) + 0x14u)

#define RCC_BASE            0x44020C00u
#define RCC_AHB2ENR         REG32(RCC_BASE + 0x8Cu)
#define RCC_AHB2ENR_GPIOAEN (1u << 0)
#define RCC_AHB2ENR_GPIOBEN (1u << 1)

#define EXTI_BASE           0x44022000u
#define EXTI_RTSR1          REG32(EXTI_BASE + 0x000u)
#define EXTI_FTSR1          REG32(EXTI_BASE + 0x004u)
#define EXTI_RPR1           REG32(EXTI_BASE + 0x00Cu)
#define EXTI_FPR1           REG32(EXTI_BASE + 0x010u)
#define EXTI_EXTICR1        REG32(EXTI_BASE + 0x060u)
#define EXTI_EXTICR2        REG32(EXTI_BASE + 0x064u)
#define EXTI_EXTICR3        REG32(EXTI_BASE + 0x068u)
#define EXTI_EXTICR4        REG32(EXTI_BASE + 0x06Cu)
#define EXTI_IMR1           REG32(EXTI_BASE + 0x080u)

#define NVIC_ISER0          REG32(0xE000E100u)

/* m33mu gives each EXTI line its own IRQ, see exti_raise_irq(). */
#define EXTI_IRQN(line)     (11 + (line))

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern void __libc_init_array(void);

static inline void crt_init(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst;

    for (dst = &_sdata; dst < &_edata; ) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0u;
    __libc_init_array();
}

/* Target of --signal-sync=signal_trigger_marker. */
__attribute__((noinline, used))
static void signal_trigger_marker(void)
{
    __asm volatile("nop");
}

__attribute__((noreturn))
static inline void test_pass(void)
{
    __asm volatile("bkpt #0x7f");
    while (1) {
        __asm volatile("wfi");
    }
}

__attribute__((noreturn))
static inline void test_fail(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
        __asm volatile("wfi");
    }
}

static inline void gpio_set_input(uint32_t base, int pin)
{
    GPIO_MODER(base) &= ~(0x3u << (pin * 2));
}

static inline void gpio_set_output(uint32_t base, int pin)
{
    GPIO_MODER(base) = (GPIO_MODER(base) & ~(0x3u << (pin * 2))) | (0x1u << (pin * 2));
}

static inline void nvic_enable_irq(int irqn)
{
    NVIC_ISER0 = (1u << irqn);
}

static inline void enable_irqs(void)
{
    __asm volatile("cpsie i");
}

static inline void exti_select_port(int line, int bank)
{
    volatile uint32_t *reg;
    int shift = (line % 4) * 8;
    uint32_t mask = 0xFFu << shift;

    switch (line / 4) {
        case 0: reg = &EXTI_EXTICR1; break;
        case 1: reg = &EXTI_EXTICR2; break;
        case 2: reg = &EXTI_EXTICR3; break;
        default: reg = &EXTI_EXTICR4; break;
    }
    *reg = (*reg & ~mask) | (((uint32_t)bank << shift) & mask);
}

static inline void exti_enable(int line, int bank, int rising, int falling)
{
    exti_select_port(line, bank);
    if (rising) EXTI_RTSR1 |= (1u << line);
    if (falling) EXTI_FTSR1 |= (1u << line);
    EXTI_IMR1 |= (1u << line);
    nvic_enable_irq(EXTI_IRQN(line));
    enable_irqs();
}

#endif /* SIGNAL_TEST_H */
