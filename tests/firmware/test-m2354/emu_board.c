/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal board support for M2354 tests running under the m33mu emulator.
 * The clock bring-up deliberately mirrors the sequence in wolfBoot's
 * hal/m2354.c so the tests exercise the same register paths a real
 * bootloader takes.
 */
#include <stdint.h>
#include "m2354.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void __libc_init_array(void);
extern int main(void);
extern void _exit(int status);

#define CLOCK_TIMEOUT 1000000

void uart0_putc(char c)
{
    while ((UART0_FIFOSTS & UART_FIFOSTS_TXFULL) != 0u) { }
    UART0_DAT = (uint32_t)(unsigned char)c;
}

static void clock_init(void)
{
    uint32_t timeout;

    SYS_UNLOCK();

    /* Only bank 0 is clocked out of reset. */
    CLK_AHBCLK |= CLK_AHBCLK_SRAM0CKEN | CLK_AHBCLK_SRAM1CKEN |
                  CLK_AHBCLK_SRAM2CKEN;

    CLK_PWRCTL |= CLK_PWRCTL_HIRCEN;
    timeout = CLOCK_TIMEOUT;
    while ((CLK_STATUS & CLK_STATUS_HIRCSTB) == 0u) {
        if (--timeout == 0u) goto done;
    }

    /* UART0 runs from HIRC, undivided. */
    CLK_CLKSEL2 = (CLK_CLKSEL2 & ~CLK_CLKSEL2_UART0SEL_Msk) |
                  CLK_CLKSEL2_UART0SEL_HIRC;
    CLK_CLKDIV0 &= ~CLK_CLKDIV0_UART0DIV_Msk;
    CLK_APBCLK0 |= CLK_APBCLK0_UART0CKEN;

    CLK_PWRCTL |= CLK_PWRCTL_HXTEN;
    timeout = CLOCK_TIMEOUT;
    while ((CLK_STATUS & CLK_STATUS_HXTSTB) == 0u) {
        if (--timeout == 0u) goto done;
    }

    CLK_PLLCTL = CLK_PLLCTL_96MHZ_HXT;
    timeout = CLOCK_TIMEOUT;
    while ((CLK_STATUS & CLK_STATUS_PLLSTB) == 0u) {
        if (--timeout == 0u) goto done;
    }

    /* Park HCLK on HIRC while the power level and wait states move. */
    CLK_CLKSEL0 |= CLK_CLKSEL0_HCLKSEL_Msk;

    timeout = CLOCK_TIMEOUT;
    while ((SYS_PLCTL & SYS_PLCTL_WRBUSY) != 0u) {
        if (--timeout == 0u) goto done;
    }
    SYS_PLCTL = SYS_PLCTL & ~SYS_PLCTL_PLSEL_Msk;   /* PL0: required for 96 MHz */
    timeout = CLOCK_TIMEOUT;
    while ((SYS_PLSTS & SYS_PLSTS_PLCBUSY) != 0u) {
        if (--timeout == 0u) goto done;
    }

    FMC_CYCCTL = (FMC_CYCCTL & ~0xFu) | FMC_CYCCTL_CYCLE_96MHZ;

    CLK_CLKDIV0 &= ~CLK_CLKDIV0_HCLKDIV_Msk;
    CLK_CLKSEL0 = (CLK_CLKSEL0 & ~CLK_CLKSEL0_HCLKSEL_Msk) |
                  CLK_CLKSEL0_HCLKSEL_PLL;
done:
    SYS_LOCK();
}

static void uart0_init(void)
{
    SYS_UNLOCK();
    /* NuMaker-M2354 routes the Nu-Link2-Me VCOM to PA6/PA7. */
    SYS_GPA_MFPL &= ~(SYS_GPA_MFPL_PA6MFP_Msk | SYS_GPA_MFPL_PA7MFP_Msk);
    SYS_GPA_MFPL |= SYS_GPA_MFPL_PA6MFP_UART0_RXD |
                    SYS_GPA_MFPL_PA7MFP_UART0_TXD;
    SYS_LOCK();

    UART0_FUNCSEL = 0u;
    UART0_FIFO |= UART_FIFO_RXRST | UART_FIFO_TXRST;
    UART0_LINE = UART_LINE_WLS_8BIT;
    UART0_BAUD = UART_BAUD_BAUDM1 | UART_BAUD_BAUDM0 |
                 UART_BAUD_MODE2_DIVIDER(CLK_HIRC_FREQ, 115200u);
}

__attribute__((naked)) void Reset_Handler(void)
{
    __asm volatile(
        "ldr  r0, =_estack\n"
        "mov  sp, r0\n"
        "b    reset_handler_c\n"
    );
}

void reset_handler_c(void)
{
    uint32_t *src, *dst;

    src = &_sidata;
    dst = &_sdata;
    while (dst < &_edata) { *dst++ = *src++; }

    dst = &_sbss;
    while (dst < &_ebss) { *dst++ = 0u; }

    clock_init();
    uart0_init();
    __libc_init_array();
    _exit(main());
}
