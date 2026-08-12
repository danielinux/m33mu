/* Shared STM32H5F4 register definitions for the signal-injection GPIO
 * test firmware family. Addresses/offsets confirmed directly against
 * cpu/stm32h5_mmio.c (not assumed from datasheet memory) during
 * implementation. */
#ifndef SIGNAL_TEST_BOARD_REGS_H
#define SIGNAL_TEST_BOARD_REGS_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* --- GPIO (bank A) --- */
#define GPIOA_BASE          0x42020000u
#define GPIOB_BASE          0x42020400u
#define GPIO_MODER(base)    REG32((base) + 0x00u)
#define GPIO_IDR(base)      REG32((base) + 0x10u)
#define GPIO_ODR(base)      REG32((base) + 0x14u)

/* --- RCC --- */
#define RCC_BASE            0x44020C00u
#define RCC_AHB2ENR         REG32(RCC_BASE + 0x8Cu)
#define RCC_AHB2ENR_GPIOAEN (1u << 0)
#define RCC_AHB2ENR_GPIOBEN (1u << 1)

/* --- EXTI (non-secure) --- */
#define EXTI_BASE           0x44022000u
#define EXTI_RTSR1          REG32(EXTI_BASE + 0x000u)
#define EXTI_FTSR1          REG32(EXTI_BASE + 0x004u)
#define EXTI_RPR1           REG32(EXTI_BASE + 0x00Cu) /* write-1-to-clear */
#define EXTI_FPR1           REG32(EXTI_BASE + 0x010u) /* write-1-to-clear */
#define EXTI_EXTICR1        REG32(EXTI_BASE + 0x060u)
#define EXTI_EXTICR2        REG32(EXTI_BASE + 0x064u) /* lines 4-7 */
#define EXTI_EXTICR3        REG32(EXTI_BASE + 0x068u)
#define EXTI_EXTICR4        REG32(EXTI_BASE + 0x06Cu)
#define EXTI_IMR1           REG32(EXTI_BASE + 0x080u)

/* --- NVIC --- */
#define NVIC_ISER0          REG32(0xE000E100u)

/* IRQn = 11 + line, per cpu/stm32h5_mmio.c's exti_raise_irq(). This is
 * m33mu's own EXTI-IRQ model (one IRQ per line); it does not reproduce
 * real STM32 silicon's shared EXTI9_5/EXTI15_10 IRQ grouping, which
 * doesn't matter for what these tests exercise (the signal-injection ->
 * EXTI -> NVIC chain), but is worth knowing if comparing against a real
 * reference manual's vector table. */
#define EXTI_IRQN(line)     (11 + (line))

static inline void nvic_enable_irq(int irqn)
{
    NVIC_ISER0 = (1u << irqn);
}

static inline void enable_irqs(void)
{
    __asm volatile("cpsie i");
}

/* Selects GPIO bank `bank` (0=A, 1=B, ...) as the source for EXTI line
 * `line` (0-15), via the correct EXTICRn/nibble. */
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

#endif /* SIGNAL_TEST_BOARD_REGS_H */
