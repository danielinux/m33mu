/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register definitions for the Nuvoton M2354, shared by the emulator tests.
 * Transcribed from the M2354 Series TRM Rev 1.01.
 */
#ifndef M33MU_TEST_M2354_H
#define M33MU_TEST_M2354_H

#include <stdint.h>

#define NS_OFFSET       0x10000000u

#define SYS_BASE        0x40000000u
#define CLK_BASE        0x40000200u
#define FMC_BASE        0x4000C000u
#define SCU_BASE        0x4002F000u
#define UART0_BASE      0x40070000u

#define REG32(a)        (*(volatile uint32_t *)(uintptr_t)(a))

/* SYS */
#define SYS_PDID        REG32(SYS_BASE + 0x000u)
#define SYS_GPA_MFPL    REG32(SYS_BASE + 0x030u)
#define SYS_REGLCTL     REG32(SYS_BASE + 0x100u)
#define SYS_PLCTL       REG32(SYS_BASE + 0x1F8u)
#define SYS_PLSTS       REG32(SYS_BASE + 0x1FCu)

#define SYS_PLCTL_PLSEL_Msk  (0x3u << 0)
#define SYS_PLCTL_WRBUSY     (1u << 7)
#define SYS_PLSTS_PLCBUSY    (1u << 0)

#define SYS_GPA_MFPL_PA6MFP_UART0_RXD (0x7u << 24)
#define SYS_GPA_MFPL_PA7MFP_UART0_TXD (0x7u << 28)
#define SYS_GPA_MFPL_PA6MFP_Msk       (0xFu << 24)
#define SYS_GPA_MFPL_PA7MFP_Msk       (0xFu << 28)

#define SYS_UNLOCK() do {      \
        SYS_REGLCTL = 0x59u;   \
        SYS_REGLCTL = 0x16u;   \
        SYS_REGLCTL = 0x88u;   \
    } while (SYS_REGLCTL == 0u)

#define SYS_LOCK()   do { SYS_REGLCTL = 0u; } while (0)

/* CLK */
#define CLK_PWRCTL      REG32(CLK_BASE + 0x000u)
#define CLK_AHBCLK      REG32(CLK_BASE + 0x004u)
#define CLK_APBCLK0     REG32(CLK_BASE + 0x008u)
#define CLK_CLKSEL0     REG32(CLK_BASE + 0x010u)
#define CLK_CLKSEL2     REG32(CLK_BASE + 0x018u)
#define CLK_CLKDIV0     REG32(CLK_BASE + 0x020u)
#define CLK_PLLCTL      REG32(CLK_BASE + 0x040u)
#define CLK_STATUS      REG32(CLK_BASE + 0x050u)

#define CLK_PWRCTL_HXTEN     (1u << 0)
#define CLK_PWRCTL_HIRCEN    (1u << 2)
#define CLK_STATUS_HXTSTB    (1u << 0)
#define CLK_STATUS_PLLSTB    (1u << 2)
#define CLK_STATUS_HIRCSTB   (1u << 4)

#define CLK_CLKSEL0_HCLKSEL_Msk  (0x7u << 0)
#define CLK_CLKSEL0_HCLKSEL_PLL  (0x2u << 0)
#define CLK_CLKSEL2_UART0SEL_Msk (0x7u << 16)
#define CLK_CLKSEL2_UART0SEL_HIRC (0x3u << 16)
#define CLK_CLKDIV0_HCLKDIV_Msk  (0xFu << 0)
#define CLK_CLKDIV0_UART0DIV_Msk (0xFu << 8)
#define CLK_APBCLK0_UART0CKEN    (1u << 16)
#define CLK_AHBCLK_SRAM0CKEN     (1u << 20)
#define CLK_AHBCLK_SRAM1CKEN     (1u << 21)
#define CLK_AHBCLK_SRAM2CKEN     (1u << 22)

/* PLL: 12 MHz HXT to 96 MHz. NR=2, NF=16, NO=2. */
#define CLK_PLLCTL_96MHZ_HXT  (0x4000u | ((2u - 1u) << 9) | (16u - 2u))
#define CLK_HXT_FREQ          12000000u
#define CLK_HIRC_FREQ         12000000u

/* FMC */
#define FMC_ISPCTL      REG32(FMC_BASE + 0x00u)
#define FMC_ISPADDR     REG32(FMC_BASE + 0x04u)
#define FMC_ISPDAT      REG32(FMC_BASE + 0x08u)
#define FMC_ISPCMD      REG32(FMC_BASE + 0x0Cu)
#define FMC_ISPTRG      REG32(FMC_BASE + 0x10u)
#define FMC_ISPSTS      REG32(FMC_BASE + 0x40u)
#define FMC_CYCCTL      REG32(FMC_BASE + 0x4Cu)
#define FMC_MPDAT0      REG32(FMC_BASE + 0x80u)
#define FMC_MPDAT1      REG32(FMC_BASE + 0x84u)
#define FMC_MPDAT2      REG32(FMC_BASE + 0x88u)
#define FMC_MPDAT3      REG32(FMC_BASE + 0x8Cu)

#define FMC_ISPCTL_ISPEN     (1u << 0)
#define FMC_ISPCTL_APUEN     (1u << 3)
#define FMC_ISPCTL_ISPFF     (1u << 6)
#define FMC_ISPTRG_ISPGO     (1u << 0)

#define FMC_ISPCMD_READ         0x00u
#define FMC_ISPCMD_READ_ALL1    0x08u
#define FMC_ISPCMD_READ_DID     0x0Cu
#define FMC_ISPCMD_PROGRAM      0x21u
#define FMC_ISPCMD_PAGE_ERASE   0x22u
#define FMC_ISPCMD_PROGRAM_MUL  0x27u
#define FMC_ISPCMD_RUN_ALL1     0x28u

#define FMC_FLASH_PAGE_SIZE  0x800u
#define FMC_APROM_END        0x00100000u
#define FMC_ALL1_BLANK       0xA11FFFFFu
#define FMC_CYCCTL_CYCLE_96MHZ 4u

/* SCU */
#define SCU_PNSSET(n)   REG32(SCU_BASE + 0x000u + ((n) * 4u))
#define SCU_SRAMNSSET   REG32(SCU_BASE + 0x024u)
#define SCU_FNSADDR     REG32(SCU_BASE + 0x028u)
#define SCU_IONSSET(n)  REG32(SCU_BASE + 0x140u + ((n) * 4u))

/* UART0 */
#define UART0_DAT       REG32(UART0_BASE + 0x000u)
#define UART0_FIFO      REG32(UART0_BASE + 0x008u)
#define UART0_LINE      REG32(UART0_BASE + 0x00Cu)
#define UART0_FIFOSTS   REG32(UART0_BASE + 0x018u)
#define UART0_BAUD      REG32(UART0_BASE + 0x024u)
#define UART0_FUNCSEL   REG32(UART0_BASE + 0x030u)

#define UART_FIFO_RXRST      (1u << 1)
#define UART_FIFO_TXRST      (1u << 2)
#define UART_LINE_WLS_8BIT   (0x3u << 0)
#define UART_FIFOSTS_TXFULL  (1u << 23)
#define UART_FIFOSTS_TXEMPTY (1u << 22)
#define UART_BAUD_BAUDM0     (1u << 28)
#define UART_BAUD_BAUDM1     (1u << 29)
#define UART_BAUD_MODE2_DIVIDER(src, baud) \
    (((((src) + ((baud) / 2u)) / (baud)) - 2u))

#endif /* M33MU_TEST_M2354_H */
