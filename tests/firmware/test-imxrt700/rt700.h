/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal i.MX RT700 (MIMXRT798S) register definitions for the emulator
 * tests.  Addresses are the secure (bit 28 set) peripheral aliases.
 */
#ifndef RT700_H
#define RT700_H

#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))

/* Clock / reset controllers: PSCCTLn / PRSTCTLn at 0x10 + 4n, _SET at 0x40 + 4n, _CLR at 0x70 + 4n */
#define RSTCTL0_BASE   0x50000000u
#define CLKCTL0_BASE   0x50001000u
#define RSTCTL3_BASE   0x50060000u
#define CLKCTL3_BASE   0x50061000u
#define PSCCTL(n)      (0x10u + 4u * (n))
#define PSCCTL_SET(n)  (0x40u + 4u * (n))
#define PSCCTL_CLR(n)  (0x70u + 4u * (n))

#define SYSCON3_BASE        0x50062000u
#define SYSCON3_CPU_STATUS  REG32(SYSCON3_BASE + 0x8Cu)
#define SYSCON3_CPU1_SVTOR  REG32(SYSCON3_BASE + 0x98u)
#define SYSCON3_CPU1_NSVTOR REG32(SYSCON3_BASE + 0x9Cu)

#define GLIKEY0_BASE   0x5017CC00u
#define GLIKEY4_BASE   0x50062C00u
#define GLIKEY_CTRL_0(b)  REG32((b) + 0x0u)
#define GLIKEY_CTRL_1(b)  REG32((b) + 0x4u)
#define GLIKEY_STATUS(b)  REG32((b) + 0xCu)

/* LP_FLEXCOMM0 / LPUART0 */
#define FC0_BASE       0x50110000u
#define FC_PSELID(b)   REG32((b) + 0xFF8u)
#define LPUART_BAUD(b) REG32((b) + 0x10u)
#define LPUART_STAT(b) REG32((b) + 0x14u)
#define LPUART_CTRL(b) REG32((b) + 0x18u)
#define LPUART_DATA(b) REG32((b) + 0x1Cu)

/* RGPIO */
#define GPIO0_BASE     0x50100000u
#define GPIO_PDOR(b)   REG32((b) + 0x40u)
#define GPIO_PSOR(b)   REG32((b) + 0x44u)
#define GPIO_PCOR(b)   REG32((b) + 0x48u)
#define GPIO_PTOR(b)   REG32((b) + 0x4Cu)
#define GPIO_PDIR(b)   REG32((b) + 0x50u)
#define GPIO_PDDR(b)   REG32((b) + 0x54u)

/* CTIMER0 */
#define CTIMER0_BASE   0x50028000u
#define CT_IR(b)       REG32((b) + 0x00u)
#define CT_TCR(b)      REG32((b) + 0x04u)
#define CT_TC(b)       REG32((b) + 0x08u)
#define CT_PR(b)       REG32((b) + 0x0Cu)
#define CT_MCR(b)      REG32((b) + 0x14u)
#define CT_MR0(b)      REG32((b) + 0x18u)

/* OS event timer (CPU0 / CPU1 blocks) */
#define OSTIMER0_BASE  0x50207000u
#define OSTIMER1_BASE  0x50209000u
#define OS_EVTIMERL(b) REG32((b) + 0x00u)
#define OS_EVTIMERH(b) REG32((b) + 0x04u)
#define OS_MATCH_L(b)  REG32((b) + 0x10u)
#define OS_MATCH_H(b)  REG32((b) + 0x14u)
#define OS_CTRL(b)     REG32((b) + 0x1Cu)

/* CRC engine */
#define CRC_BASE       0x50151000u
#define CRC_DATA       REG32(CRC_BASE + 0x0u)
#define CRC_DATA8      (*(volatile uint8_t *)(CRC_BASE + 0x0u))
#define CRC_GPOLY      REG32(CRC_BASE + 0x4u)
#define CRC_CTRL       REG32(CRC_BASE + 0x8u)

/* TRNG */
#define TRNG_BASE      0x50187000u
#define TRNG_MCTL      REG32(TRNG_BASE + 0x00u)
#define TRNG_ENT(n)    REG32(TRNG_BASE + 0x40u + 4u * (n))

/* OCOTP */
#define OCOTP_BASE       0x50018000u
#define OCOTP_SHADOW(n)  REG32(OCOTP_BASE + 4u * (n))
#define OCOTP_CTRL       REG32(OCOTP_BASE + 0x800u)
#define OCOTP_WRITE_DATA REG32(OCOTP_BASE + 0x808u)
#define OCOTP_READ_CTRL  REG32(OCOTP_BASE + 0x80Cu)
#define OCOTP_READ_DATA  REG32(OCOTP_BASE + 0x810u)
#define OCOTP_STATUS     REG32(OCOTP_BASE + 0x820u)

/* Synthetic m33mu ELS / PKC / PUF command interface */
#define ELS_BASE       0x50190000u
#define PKC_BASE       0x50011000u
#define PUF_BASE       0x50194000u
#define SEC_CMD(b)     REG32((b) + 0x000u)
#define SEC_STATUS(b)  REG32((b) + 0x004u)
#define SEC_ARG(b, n)  REG32((b) + 0x008u + 4u * (n))
#define SEC_RES(b, n)  REG32((b) + 0x020u + 4u * (n))
#define SEC_KEYIN(b, n) REG32((b) + 0x080u + 4u * (n))
#define SEC_DATA(b, n) REG32((b) + 0x100u + 4u * (n))
#define SEC_ST_DONE    (1u << 1)
#define SEC_ST_ERROR   (1u << 2)

/* AHB secure controller 0 */
#define AHBSC0_BASE          0x5017C000u
#define AHBSC0_SRAM_RULE(off) REG32(AHBSC0_BASE + (off))
#define AHBSC0_MISC_CTRL     REG32(AHBSC0_BASE + 0xFFCu)

/* MU1 */
#define MU1_A_BASE     0x50202000u
#define MU1_B_BASE     0x50203000u
#define MU_GIER(b)     REG32((b) + 0x110u)
#define MU_GCR(b)      REG32((b) + 0x114u)
#define MU_GSR(b)      REG32((b) + 0x118u)
#define MU_TCR(b)      REG32((b) + 0x120u)
#define MU_TSR(b)      REG32((b) + 0x124u)
#define MU_RCR(b)      REG32((b) + 0x128u)
#define MU_RSR(b)      REG32((b) + 0x12Cu)
#define MU_TR(b, n)    REG32((b) + 0x200u + 4u * (n))
#define MU_RR(b, n)    REG32((b) + 0x280u + 4u * (n))

/* ROM API tree */
#define ROM_API_TREE   0x1303FC00u

/* Cortex-M33 system registers */
#define SCB_VTOR       REG32(0xE000ED08u)
#define SCB_SHCSR      REG32(0xE000ED24u)
#define SCB_CPACR      REG32(0xE000ED88u)
#define SAU_CTRL       REG32(0xE000EDD0u)
#define SAU_RNR        REG32(0xE000EDD8u)
#define SAU_RBAR       REG32(0xE000EDDCu)
#define SAU_RLAR       REG32(0xE000EDE0u)
#define SAU_SFSR       REG32(0xE000EDE4u)
#define NVIC_ISER(n)   REG32(0xE000E100u + 4u * (n))
#define NVIC_ICPR(n)   REG32(0xE000E280u + 4u * (n))

/* Shared SRAM mailbox used by both cores (secure system-bus alias). */
#define SHARED_MAILBOX 0x30700000u
#define MBOX_CORE1_COUNT REG32(SHARED_MAILBOX + 0x0u)
#define MBOX_CORE1_OSTIMER REG32(SHARED_MAILBOX + 0x4u)

static inline void nvic_enable(unsigned irq)
{
    NVIC_ICPR(irq / 32u) = 1u << (irq % 32u);
    NVIC_ISER(irq / 32u) = 1u << (irq % 32u);
}

/* Sleep until *flag >= min (or the spin budget runs out) without losing a
 * wake-up: the flag is tested with interrupts masked, and WFI still wakes
 * on an interrupt that becomes pending while PRIMASK is set. */
static inline void wait_flag(volatile uint32_t *flag, uint32_t min, uint32_t budget)
{
    while (budget-- != 0u) {
        __asm volatile("cpsid i" ::: "memory");
        if (*flag >= min) {
            __asm volatile("cpsie i" ::: "memory");
            return;
        }
        __asm volatile("wfi");
        __asm volatile("cpsie i" ::: "memory");
    }
}

static inline uint64_t gray_encode(uint64_t v) { return v ^ (v >> 1); }

static inline uint64_t gray_decode(uint64_t g)
{
    uint64_t v = g;
    unsigned s;
    for (s = 1; s < 64u; s <<= 1) {
        v ^= v >> s;
    }
    return v;
}

static inline uint64_t ostimer_now(uint32_t base)
{
    uint32_t lo = OS_EVTIMERL(base);
    uint32_t hi = OS_EVTIMERH(base);
    return gray_decode(((uint64_t)(hi & 0x3FFu) << 32) | lo);
}

static inline void ostimer_arm(uint32_t base, uint64_t ticks_from_now)
{
    uint64_t g = gray_encode(ostimer_now(base) + ticks_from_now);
    OS_CTRL(base) = 1u; /* clear flag */
    OS_MATCH_L(base) = (uint32_t)g;
    OS_MATCH_H(base) = (uint32_t)(g >> 32) & 0x3FFu;
    OS_CTRL(base) = 2u; /* INTENA */
}

#endif /* RT700_H */
