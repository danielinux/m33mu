/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;
extern void __libc_init_array(void);

/* MRCC */
#define MRCC_BASE        0x4001C000u
#define MRCC_LPUART0     0xE0u
#define MRCC_LPSPI0      0xD8u
#define MRCC_LPIT0       0xBCu
#define MRCC_PORTA       0x108u
#define MRCC_GPIOA       0x404u

static inline void mrcc_enable(uint32_t off)
{
    volatile uint32_t *reg = (volatile uint32_t *)(MRCC_BASE + off);
    *reg = (1u << 31) | (1u << 30) | 1u; /* PR=1, RSTB=1, CC=1 */
}

/* PORTA */
#define PORTA_BASE       0x40042000u
#define PORTA_PCR0       (*(volatile uint32_t *)(PORTA_BASE + 0x80u))

/* GPIOA */
#define GPIOA_BASE       0x48010000u
#define GPIO_PDOR(b)     (*(volatile uint32_t *)((b) + 0x40u))
#define GPIO_PSOR(b)     (*(volatile uint32_t *)((b) + 0x44u))
#define GPIO_PCOR(b)     (*(volatile uint32_t *)((b) + 0x48u))
#define GPIO_PDDR(b)     (*(volatile uint32_t *)((b) + 0x54u))

/* LPUART0 */
#define LPUART0_BASE     0x40038000u
#define LPUART_STAT(b)   (*(volatile uint32_t *)((b) + 0x14u))
#define LPUART_CTRL(b)   (*(volatile uint32_t *)((b) + 0x18u))
#define LPUART_DATA(b)   (*(volatile uint32_t *)((b) + 0x1Cu))
#define LPUART_STAT_TDRE (1u << 23)
#define LPUART_CTRL_TE   (1u << 19)

/* LPSPI0 */
#define LPSPI0_BASE      0x40036000u
#define LPSPI_CR(b)      (*(volatile uint32_t *)((b) + 0x10u))
#define LPSPI_SR(b)      (*(volatile uint32_t *)((b) + 0x14u))
#define LPSPI_TCR(b)     (*(volatile uint32_t *)((b) + 0x60u))
#define LPSPI_TDR(b)     (*(volatile uint32_t *)((b) + 0x64u))
#define LPSPI_RDR(b)     (*(volatile uint32_t *)((b) + 0x74u))
#define LPSPI_CR_MEN     (1u << 0)
#define LPSPI_SR_TDF     (1u << 0)
#define LPSPI_TCR_FRAMESZ(x) ((x) & 0x1Fu)
#define LPSPI_TCR_CONT     (1u << 21)

/* SysTick */
#define SYST_CSR          (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR          (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR          (*(volatile uint32_t *)0xE000E018u)
#define SYST_CSR_ENABLE   (1u << 0)
#define SYST_CSR_TICKINT  (1u << 1)
#define SYST_CSR_CLKSRC   (1u << 2)

volatile uint32_t systick_ms = 0;

static void lpuart0_init(void)
{
    mrcc_enable(MRCC_LPUART0);
    LPUART_CTRL(LPUART0_BASE) = LPUART_CTRL_TE;
}

void lpuart0_putc(char c)
{
    while ((LPUART_STAT(LPUART0_BASE) & LPUART_STAT_TDRE) == 0u) {
    }
    LPUART_DATA(LPUART0_BASE) = (uint32_t)c;
}

static void gpio_cs_init(void)
{
    mrcc_enable(MRCC_PORTA);
    mrcc_enable(MRCC_GPIOA);
    PORTA_PCR0 = (1u << 8); /* GPIO mux */
    GPIO_PDDR(GPIOA_BASE) |= 1u << 0;
    GPIO_PSOR(GPIOA_BASE) = 1u << 0;
}

static void spi0_init(void)
{
    mrcc_enable(MRCC_LPSPI0);
    LPSPI_CR(LPSPI0_BASE) = 0;
    LPSPI_TCR(LPSPI0_BASE) = LPSPI_TCR_FRAMESZ(7u) | LPSPI_TCR_CONT;
    LPSPI_CR(LPSPI0_BASE) = LPSPI_CR_MEN;
}

static uint8_t spi0_xfer(uint8_t v)
{
    while ((LPSPI_SR(LPSPI0_BASE) & LPSPI_SR_TDF) == 0u) {
    }
    LPSPI_TDR(LPSPI0_BASE) = v;
    return (uint8_t)(LPSPI_RDR(LPSPI0_BASE) & 0xFFu);
}

static void systick_init(uint32_t cpu_hz)
{
    uint32_t reload = (cpu_hz / 1000u) - 1u;
    SYST_CVR = 0u;
    SYST_RVR = reload;
    SYST_CSR = SYST_CSR_CLKSRC | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

void SysTick_Handler(void)
{
    ++systick_ms;
}

static uint32_t wait_systick(uint32_t target_ms)
{
    uint32_t start = systick_ms;
    volatile uint32_t spin = 0;
    while ((uint32_t)(systick_ms - start) < target_ms) {
        if (++spin > 2000000u) {
            break;
        }
    }
    return (uint32_t)(systick_ms - start);
}

int main(void)
{
    uint8_t id0, id1, id2;

    lpuart0_init();
    gpio_cs_init();
    spi0_init();
    __asm volatile("cpsie i");
    systick_init(48000000u);

    printf("MCXW test start\n");

    GPIO_PCOR(GPIOA_BASE) = 1u << 0; /* CS low */
    spi0_xfer(0x9Fu);
    id0 = spi0_xfer(0xFFu);
    id1 = spi0_xfer(0xFFu);
    id2 = spi0_xfer(0xFFu);
    GPIO_PSOR(GPIOA_BASE) = 1u << 0; /* CS high */

    printf("JEDEC ID: %02x %02x %02x\n", id0, id1, id2);
    printf("MCXW test done\n");
    printf("SysTick ms: %lu\n", (unsigned long)systick_ms);
    printf("SysTick +%lu ms\n", (unsigned long)wait_systick(5u));

    for (int i = 0; i < 5; ++i) {
        wait_systick(1000u);
        printf("Hello, world!\n");
    }

    __asm volatile("bkpt #0x7f");
    while (1) {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }

    __libc_init_array();
    main();
}

void HardFault_Handler(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
    }
}
