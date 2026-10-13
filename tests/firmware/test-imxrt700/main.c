/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * i.MX RT700 CPU0 (secure) emulator test: boot path, LPUART, GPIO, CTIMER,
 * OS event timer, CRC, TRNG, OCOTP, GLIKEY, ELS/PKC/PUF, ROM API,
 * TrustZone (SAU, SG veneer, BLXNS, TT), AHBSC SRAM rules and the second
 * Cortex-M33 (CPU1) talking over MU1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arm_cmse.h>
#include "rt700.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern uint32_t _vectors_start;
extern uint32_t __sg_start, __sg_end;
extern const uint8_t core1_bin_start[], core1_bin_end[];
extern const uint8_t ns_bin_start[], ns_bin_end[];

#define IRQ_CTIMER0 3u
#define IRQ_MU1_A 30u
#define IRQ_OS_EVENT 34u

#define CORE1_LOAD_S   0x30600000u  /* system-bus alias of the CPU1 image */
#define CORE1_VTOR     0x10600000u  /* code-bus alias CPU1 boots from */
#define CORE1_READY_MAGIC 0xC0DE0001u
#define NS_LOAD        0x20400000u
#define NS_ENTRY       0x20400101u
#define NS_STACK_TOP   0x20410000u
#define FAULT_MARK     0xFA017ED0u

static int failures;
static uint32_t boot_vtor;

static void check(const char *name, int ok)
{
    printf("%-34s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        failures++;
    }
}

/* ------------------------------------------------------------------------ */
/* Clock / reset helpers (SDK CLOCK_EnableClock / RESET_ClearPeripheralReset) */

static void clock_enable(uint32_t clkctl, unsigned n, unsigned bit)
{
    REG32(clkctl + PSCCTL_SET(n)) = 1u << bit;
    while ((REG32(clkctl + PSCCTL(n)) & (1u << bit)) == 0u) {
    }
}

static void reset_release(uint32_t rstctl, unsigned n, unsigned bit)
{
    REG32(rstctl + PSCCTL_CLR(n)) = 1u << bit;
    while ((REG32(rstctl + PSCCTL(n)) & (1u << bit)) != 0u) {
    }
}

/* ------------------------------------------------------------------------ */
/* LPUART0 console                                                           */

#define LPUART_CTRL_RE (1u << 18)
#define LPUART_CTRL_TE (1u << 19)
#define LPUART_STAT_TDRE (1u << 23)

static void uart_init(void)
{
    clock_enable(CLKCTL0_BASE, 1, 30);   /* LP_FLEXCOMM0 */
    reset_release(RSTCTL0_BASE, 2, 30);
    FC_PSELID(FC0_BASE) = 1u;            /* LPUART */
    LPUART_BAUD(FC0_BASE) = 0x0F000068u;
    LPUART_CTRL(FC0_BASE) = LPUART_CTRL_TE | LPUART_CTRL_RE;
}

void uart_putc(char c)
{
    while ((LPUART_STAT(FC0_BASE) & LPUART_STAT_TDRE) == 0u) {
    }
    LPUART_DATA(FC0_BASE) = (uint8_t)c;
}

/* ------------------------------------------------------------------------ */
/* Fault recovery for the deliberate non-secure violations                   */

volatile uint32_t fault_count;

/* A non-secure LDR.W that faults is skipped; its destination (the stacked
 * r0 of the non-secure frame) reads FAULT_MARK. */
__attribute__((naked)) void fault_handler(void)
{
    __asm volatile(
        "tst   lr, #0x40          \n" /* EXC_RETURN.S: secure frame? */
        "bne   1f                 \n"
        "tst   lr, #0x4           \n"
        "ite   eq                 \n"
        "mrseq r0, msp_ns         \n"
        "mrsne r0, psp_ns         \n"
        "ldr   r1, [r0, #24]      \n"
        "adds  r1, r1, #4         \n"
        "str   r1, [r0, #24]      \n"
        "ldr   r1, =0xFA017ED0    \n"
        "str   r1, [r0, #0]       \n"
        "ldr   r1, =fault_count   \n"
        "ldr   r2, [r1]           \n"
        "adds  r2, r2, #1         \n"
        "str   r2, [r1]           \n"
        "ldr   r1, =0xE000EDE4    \n" /* SFSR: write-one-to-clear */
        "ldr   r2, =0xFFFFFFFF    \n"
        "str   r2, [r1]           \n"
        "ldr   r1, =0xE000ED28    \n" /* CFSR */
        "str   r2, [r1]           \n"
        "ldr   r1, =0xE000ED2C    \n" /* HFSR */
        "str   r2, [r1]           \n"
        "bx    lr                 \n"
        "1: bkpt #0x7e            \n"
        "b     1b                 \n"
        ".ltorg                   \n");
}

/* ------------------------------------------------------------------------ */
/* Interrupt handlers                                                        */

static volatile uint32_t ctimer_hits;
static volatile uint32_t os_hits;
static volatile uint32_t mu_rx;
static volatile uint32_t mu_got;

void CTIMER0_IRQHandler(void)
{
    uint32_t ir = CT_IR(CTIMER0_BASE);
    CT_IR(CTIMER0_BASE) = ir;
    if (ir & 1u) {
        ctimer_hits++;
    }
}

void OS_EVENT_IRQHandler(void)
{
    OS_CTRL(OSTIMER0_BASE) = 1u;
    os_hits++;
}

void MU1_A_IRQHandler(void)
{
    while (MU_RSR(MU1_A_BASE) & 1u) {
        mu_rx = MU_RR(MU1_A_BASE, 0);
        mu_got = 1u;
    }
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */

static void test_boot(void)
{
    uint32_t expected = (uint32_t)&_vectors_start;
    printf("boot vector table at 0x%08lx\n", (unsigned long)boot_vtor);
    check("boot: vector table", boot_vtor == expected);
    check("boot: running secure", (cmse_TT((void *)expected).flags.secure) != 0);
}

static void test_gpio(void)
{
    clock_enable(CLKCTL0_BASE, 1, 18);
    reset_release(RSTCTL0_BASE, 2, 18);
    GPIO_PDDR(GPIO0_BASE) = 0xFFu;
    GPIO_PDOR(GPIO0_BASE) = 0u;
    GPIO_PSOR(GPIO0_BASE) = 0x0Fu;
    GPIO_PCOR(GPIO0_BASE) = 0x03u;
    GPIO_PTOR(GPIO0_BASE) = 0x30u;
    check("gpio: set/clear/toggle", GPIO_PDIR(GPIO0_BASE) == 0x3Cu);
}

static void test_ctimer(void)
{
    clock_enable(CLKCTL0_BASE, 2, 21);
    reset_release(RSTCTL0_BASE, 3, 21);
    CT_TCR(CTIMER0_BASE) = 2u;           /* reset */
    CT_PR(CTIMER0_BASE) = 0u;
    CT_MR0(CTIMER0_BASE) = 2000u;
    CT_MCR(CTIMER0_BASE) = 3u;           /* MR0I | MR0R */
    nvic_enable(IRQ_CTIMER0);
    CT_TCR(CTIMER0_BASE) = 1u;           /* enable */
    wait_flag(&ctimer_hits, 3u, 1000000u);
    CT_TCR(CTIMER0_BASE) = 0u;
    check("ctimer0: match interrupts", ctimer_hits >= 3u);
}

static void test_ostimer(void)
{
    uint64_t t0 = ostimer_now(OSTIMER0_BASE);
    nvic_enable(IRQ_OS_EVENT);
    ostimer_arm(OSTIMER0_BASE, 50u);
    wait_flag(&os_hits, 1u, 1000000u);
    check("ostimer: counter advances", ostimer_now(OSTIMER0_BASE) > t0);
    check("ostimer: match interrupt", os_hits == 1u);
}

static uint32_t crc_run(uint32_t ctrl, uint32_t poly, uint32_t seed, const char *s)
{
    CRC_CTRL = ctrl | (1u << 25);        /* WAS: next write is the seed */
    CRC_GPOLY = poly;
    CRC_DATA = seed;
    CRC_CTRL = ctrl;
    while (*s) {
        CRC_DATA8 = (uint8_t)*s++;
    }
    return CRC_DATA;
}

static void test_crc(void)
{
    /* CRC-32: reflected in/out (TOT=TOTR=bits+bytes), final XOR. */
    uint32_t c32 = crc_run((2u << 30) | (2u << 28) | (1u << 26) | (1u << 24), 0x04C11DB7u, 0xFFFFFFFFu,
                           "123456789");
    /* CRC-16/CCITT-FALSE: no transposition. */
    uint32_t c16 = crc_run(0u, 0x1021u, 0xFFFFu, "123456789") & 0xFFFFu;
    check("crc: CRC-32(\"123456789\")", c32 == 0xCBF43926u);
    check("crc: CRC-16/CCITT(\"123456789\")", c16 == 0x29B1u);
}

static void test_trng(void)
{
    uint32_t a[16];
    uint32_t b[16];
    uint32_t i;
    int nonzero = 0;
    TRNG_MCTL = 1u << 16;                    /* program mode */
    TRNG_MCTL = 0u;                          /* run mode */
    while ((TRNG_MCTL & (1u << 10)) == 0u) { /* ENT_VAL */
    }
    for (i = 0; i < 16u; ++i) a[i] = TRNG_ENT(i);
    for (i = 0; i < 16u; ++i) b[i] = TRNG_ENT(i);
    for (i = 0; i < 16u; ++i) nonzero |= (a[i] != 0u);
    check("trng: entropy words", nonzero && memcmp(a, b, sizeof(a)) != 0);
    check("trng: no error", (TRNG_MCTL & (1u << 12)) == 0u);
}

static void test_ocotp(void)
{
    OCOTP_CTRL = 0x10u;                  /* ADDR */
    OCOTP_READ_CTRL = 1u;
    while (OCOTP_STATUS & (1u << 22)) {  /* BUSY */
    }
    check("ocotp: shadow read", OCOTP_SHADOW(0x10) == 0x52543730u);
    check("ocotp: fuse read", OCOTP_READ_DATA == 0x52543730u);
    OCOTP_CTRL = (0x3E77u << 16) | 0x40u;
    OCOTP_WRITE_DATA = 0x00A5A500u;
    check("ocotp: fuse program", OCOTP_SHADOW(0x40) == 0x00A5A500u);
    OCOTP_CTRL = 0x41u;                  /* no unlock key: ignored */
    OCOTP_WRITE_DATA = 0xFFFFFFFFu;
    check("ocotp: program needs unlock key", OCOTP_SHADOW(0x41) == 0u);
}

/* SDK board.c GlikeyWriteEnable(): SyncReset, StartEnable, codewords. */
static void glikey_codeword(uint32_t b, uint32_t cw)
{
    if ((cw >> 24) == 0xF0u) {
        GLIKEY_CTRL_1(b) = (GLIKEY_CTRL_1(b) & ~0x30000u) | (cw & 0x30000u);
    } else {
        GLIKEY_CTRL_0(b) = (GLIKEY_CTRL_0(b) & ~0x30000u) | ((cw << 16) & 0x30000u);
    }
}

static void glikey_write_enable(uint32_t b, uint32_t idx)
{
    uint32_t c0;
    GLIKEY_CTRL_0(b) |= 1u << 18;        /* SFT_RST */
    c0 = GLIKEY_CTRL_0(b);
    c0 = (c0 & ~0xFFu) | idx;
    c0 = (c0 & ~0x30000u) | (1u << 16);
    GLIKEY_CTRL_0(b) = c0;
    GLIKEY_CTRL_1(b) &= ~0x30000u;
    glikey_codeword(b, 0xF0C10F3Eu);
    glikey_codeword(b, 0x0F1DF0E2u);
    glikey_codeword(b, 0xF0B00F4Fu);
    glikey_codeword(b, 0x0FFFF000u);
}

static void glikey_clear(uint32_t b)
{
    GLIKEY_CTRL_0(b) |= 1u << 18;
}

static void test_glikey(void)
{
    uint32_t before = SYSCON3_CPU1_SVTOR;
    SYSCON3_CPU1_SVTOR = 0x12345u;
    check("glikey: VTOR write-protected", SYSCON3_CPU1_SVTOR == before);
    glikey_write_enable(GLIKEY4_BASE, 1u);
    check("glikey: FSM reaches WR_EN", (GLIKEY_STATUS(GLIKEY4_BASE) >> 19) == 0x1802u);
    SYSCON3_CPU1_SVTOR = 0x12345u;
    check("glikey: VTOR write enabled", SYSCON3_CPU1_SVTOR == 0x12345u);
    glikey_clear(GLIKEY4_BASE);
    SYSCON3_CPU1_SVTOR = before;
    check("glikey: cleared again", SYSCON3_CPU1_SVTOR == 0x12345u);
}

static uint32_t sec_run(uint32_t base, uint32_t cmd)
{
    SEC_CMD(base) = cmd;
    while ((SEC_STATUS(base) & (SEC_ST_DONE | SEC_ST_ERROR)) == 0u) {
    }
    return SEC_STATUS(base);
}

static void sec_result(uint32_t base, uint8_t out[32])
{
    uint32_t i;
    for (i = 0; i < 4u; ++i) {
        uint32_t r = SEC_RES(base, i);
        uint32_t d = SEC_DATA(base, i);
        memcpy(out + 4u * i, &r, 4u);
        memcpy(out + 16u + 4u * i, &d, 4u);
    }
}

static void test_crypto(void)
{
    static const uint8_t sha_abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    static uint8_t msg[64] = "abc";
    static uint8_t digest[32];
    static uint8_t mac[32];
    static uint8_t plain[33] = "sixteen byte blk sixteen byte bl";
    static uint8_t cipher[32];
    static uint8_t back[32];
    static uint8_t rnd[16];
    static const uint8_t base[4] = { 0, 0, 0, 3 };
    static const uint8_t expo[4] = { 0, 0, 0, 5 };
    static const uint8_t mod[4] = { 0, 0, 0, 7 };
    static uint8_t res[4];
    uint32_t st;
    uint32_t fp1, fp2, fp3;
    uint32_t i;
    int nonzero = 0;

    /* SHA-256 */
    SEC_ARG(ELS_BASE, 1) = (uint32_t)msg;
    SEC_ARG(ELS_BASE, 2) = 3u;
    SEC_ARG(ELS_BASE, 3) = (uint32_t)digest;
    st = sec_run(ELS_BASE, 8u);
    check("els: SHA-256(\"abc\")", (st & SEC_ST_DONE) && memcmp(digest, sha_abc, 32) == 0);

    /* HMAC sign / verify with a generated key */
    SEC_ARG(ELS_BASE, 0) = 1u;
    st = sec_run(ELS_BASE, 1u);
    SEC_ARG(ELS_BASE, 1) = (uint32_t)msg;
    SEC_ARG(ELS_BASE, 2) = 3u;
    st &= sec_run(ELS_BASE, 4u);
    sec_result(ELS_BASE, mac);
    SEC_ARG(ELS_BASE, 3) = (uint32_t)mac;
    st &= sec_run(ELS_BASE, 5u);
    check("els: HMAC sign + verify", (st & SEC_ST_DONE) && SEC_RES(ELS_BASE, 0) == 1u);
    mac[0] ^= 1u;
    st = sec_run(ELS_BASE, 5u);
    check("els: verify rejects bad MAC", (st & SEC_ST_ERROR) != 0u);

    /* AES-256-CBC round trip with an imported key */
    SEC_ARG(ELS_BASE, 0) = 2u;
    for (i = 0; i < 4u; ++i) {
        SEC_KEYIN(ELS_BASE, i) = 0x01020304u * (i + 1u);
        SEC_DATA(ELS_BASE, i) = 0x11111111u * (i + 1u);
    }
    st = sec_run(ELS_BASE, 3u);
    for (i = 0; i < 4u; ++i) {
        SEC_KEYIN(ELS_BASE, i) = 0xA5A5A5A5u; /* IV */
    }
    SEC_ARG(ELS_BASE, 1) = (uint32_t)plain;
    SEC_ARG(ELS_BASE, 2) = 32u;
    SEC_ARG(ELS_BASE, 3) = (uint32_t)cipher;
    st &= sec_run(ELS_BASE, 9u);
    SEC_ARG(ELS_BASE, 1) = (uint32_t)cipher;
    SEC_ARG(ELS_BASE, 3) = (uint32_t)back;
    st &= sec_run(ELS_BASE, 10u);
    check("els: AES-CBC round trip",
          (st & SEC_ST_DONE) && memcmp(cipher, plain, 32) != 0 && memcmp(back, plain, 32) == 0);

    /* RNG */
    SEC_ARG(ELS_BASE, 1) = (uint32_t)rnd;
    SEC_ARG(ELS_BASE, 2) = sizeof(rnd);
    st = sec_run(ELS_BASE, 7u);
    for (i = 0; i < sizeof(rnd); ++i) nonzero |= rnd[i];
    check("els: RNG", (st & SEC_ST_DONE) && nonzero);

    /* PUF: same context -> same key, different context -> different key */
    st = sec_run(PUF_BASE, 1u);
    for (i = 0; i < 4u; ++i) SEC_KEYIN(PUF_BASE, i) = 0x50554600u + i;
    SEC_ARG(PUF_BASE, 0) = 3u;
    st &= sec_run(PUF_BASE, 2u);
    fp1 = SEC_RES(PUF_BASE, 1);
    SEC_ARG(PUF_BASE, 0) = 4u;
    st &= sec_run(PUF_BASE, 2u);
    fp2 = SEC_RES(PUF_BASE, 1);
    SEC_KEYIN(PUF_BASE, 0) = 0x12345678u;
    st &= sec_run(PUF_BASE, 2u);
    fp3 = SEC_RES(PUF_BASE, 1);
    check("puf: deterministic key derivation", (st & SEC_ST_DONE) && fp1 == fp2 && fp1 != fp3);

    /* PKC: 3^5 mod 7 = 5 */
    SEC_ARG(PKC_BASE, 0) = (uint32_t)base;
    SEC_ARG(PKC_BASE, 1) = (uint32_t)expo;
    SEC_ARG(PKC_BASE, 2) = (uint32_t)mod;
    SEC_ARG(PKC_BASE, 3) = 4u;
    SEC_DATA(PKC_BASE, 0) = (uint32_t)res;
    st = sec_run(PKC_BASE, 1u);
    check("pkc: modexp", (st & SEC_ST_DONE) && res[3] == 5u && res[0] == 0u);
}

typedef struct {
    uint32_t version;
    void (*init)(uint32_t src_clk_freq);
    int32_t (*deinit)(void);
    int32_t (*efuse_read)(uint32_t addr, uint32_t *data);
    int32_t (*efuse_program)(uint32_t addr, uint32_t data);
} ocotp_driver_t;

static void test_romapi(void)
{
    const uint32_t *tree = (const uint32_t *)ROM_API_TREE;
    const ocotp_driver_t *otp = (const ocotp_driver_t *)tree[12];
    uint32_t v = 0;
    int32_t rc;
    check("romapi: bootloader version", tree[1] != 0u);
    otp->init(192000000u);
    rc = otp->efuse_read(0x11u, &v);
    check("romapi: OTP driver fuse read", rc == 0 && v == 0x00000798u);
}

/* ---- TrustZone ---- */

/* Secure entry function (veneers.c).  Inside the secure image the linker
 * binds 'secure_cb' to the function body, so the non-secure side is handed
 * the address of its SG veneer: the only one in .gnu.sgstubs. */
extern volatile uint32_t secure_cb_hits;

typedef uint32_t __attribute__((cmse_nonsecure_call)) (*ns_entry_t)(uint32_t, uint32_t, uint32_t);

static uint32_t call_ns(uint32_t op, uint32_t arg)
{
    ns_entry_t fn = (ns_entry_t)cmse_nsfptr_create((ns_entry_t)NS_ENTRY);
    return fn(op, arg, (uint32_t)&__sg_start | 1u);
}

static void sau_region(uint32_t n, uint32_t base, uint32_t limit, int nsc)
{
    SAU_RNR = n;
    SAU_RBAR = base & ~0x1Fu;
    SAU_RLAR = (limit & ~0x1Fu) | (nsc ? 2u : 0u) | 1u;
}

static void test_trustzone(void)
{
    uint32_t r;
    cmse_address_info_t tt_ns, tt_s;

    memcpy((void *)NS_LOAD, ns_bin_start, (size_t)(ns_bin_end - ns_bin_start));
    sau_region(0, NS_LOAD, NS_STACK_TOP - 1u, 0);
    sau_region(1, (uint32_t)&__sg_start, (uint32_t)&__sg_end - 1u, 1);
    SAU_CTRL = 1u;
    SCB_SHCSR |= (1u << 19) | (1u << 16) | (1u << 17) | (1u << 18); /* Secure/Mem/Bus/UsageFault */
    REG32(0xE002ED08u) = NS_LOAD;                                    /* VTOR_NS */
    __asm volatile("msr msp_ns, %0" : : "r"(NS_STACK_TOP));

    tt_ns = cmse_TT((void *)(NS_LOAD + 0x100u));
    tt_s = cmse_TT((void *)0x30000000u);
    check("tz: TT non-secure SRAM", tt_ns.flags.secure == 0 && tt_ns.flags.sau_region_valid);
    check("tz: TT secure SRAM", tt_s.flags.secure == 1);

    r = call_ns(1u, 21u);
    check("tz: BLXNS + SG veneer callback", r == 43u && secure_cb_hits == 1u);
}

static void test_ahbsc(void)
{
    uint32_t v;
    uint32_t misc = AHBSC0_MISC_CTRL;

    /* MISC_CTRL is GLIKEY0-protected. */
    AHBSC0_MISC_CTRL = 0x86AAu;
    check("ahbsc: MISC_CTRL write-protected", AHBSC0_MISC_CTRL == misc);

    /* 0x20404000 (P14 sub-region 1) stays non-secure, 0x20408000 (sub-region
     * 2) becomes secure through SRAM_14_RULE0. */
    REG32(0x30404000u) = 0x0000AB5Eu;
    REG32(0x30408000u) = 0x5EC0DA7Au;
    AHBSC0_SRAM_RULE(0x250u) = 3u << 8;
    fault_count = 0u;
    v = call_ns(2u, 0x20404000u);
    check("ahbsc: NS read of NS-ruled block", v == 0x0000AB5Eu && fault_count == 0u);
    v = call_ns(2u, 0x20408000u);
    check("ahbsc: NS read of S-ruled block faults", v == FAULT_MARK && fault_count == 1u);
    AHBSC0_SRAM_RULE(0x250u) = 0u;
    v = call_ns(2u, 0x20408000u);
    check("ahbsc: rule cleared", v == 0x5EC0DA7Au && fault_count == 1u);
}

static void test_dual_core(void)
{
    uint32_t spin;
    uint32_t i;
    int ok = 1;

    MBOX_CORE1_COUNT = 0u;
    MBOX_CORE1_OSTIMER = 0u;
    clock_enable(CLKCTL3_BASE, 0, 5);    /* MU1 */
    reset_release(RSTCTL3_BASE, 1, 2);
    memcpy((void *)CORE1_LOAD_S, core1_bin_start, (size_t)(core1_bin_end - core1_bin_start));

    /* SDK mcmgr_start_core_internal() */
    glikey_write_enable(GLIKEY4_BASE, 1u);
    SYSCON3_CPU1_NSVTOR = CORE1_VTOR >> 7;
    SYSCON3_CPU1_SVTOR = CORE1_VTOR >> 7;
    glikey_clear(GLIKEY4_BASE);
    clock_enable(CLKCTL3_BASE, 0, 0);    /* CPU1 clock */
    reset_release(RSTCTL3_BASE, 0, 31);  /* CPU1 reset */
    SYSCON3_CPU_STATUS &= ~1u;           /* CPU_WAIT */

    for (spin = 0; spin < 2000000u && (MU_RSR(MU1_A_BASE) & 2u) == 0u; ++spin) {
    }
    check("cpu1: boots and reports ready", (MU_RSR(MU1_A_BASE) & 2u) && MU_RR(MU1_A_BASE, 1) == CORE1_READY_MAGIC);
    check("cpu1: private OS timer interrupt", MBOX_CORE1_OSTIMER == 1u);

    MU_RCR(MU1_A_BASE) = 1u;
    nvic_enable(IRQ_MU1_A);
    for (i = 0; i < 8u; ++i) {
        mu_got = 0u;
        MU_TR(MU1_A_BASE, 0) = 7u + 3u * i;
        wait_flag(&mu_got, 1u, 1000000u);
        if (!mu_got || mu_rx != 8u + 3u * i) {
            ok = 0;
        }
    }
    check("cpu1: MU1 ping-pong with interrupts", ok);
    check("cpu1: shared SRAM mailbox", MBOX_CORE1_COUNT == 8u);
}

int main(void)
{
    uart_init();
    printf("=== i.MX RT700 emulator test ===\n");
    test_boot();
    test_gpio();
    test_ctimer();
    test_ostimer();
    test_crc();
    test_trng();
    test_ocotp();
    test_glikey();
    test_crypto();
    test_romapi();
    test_trustzone();
    test_ahbsc();
    test_dual_core();
    printf("=== %s ===\n", failures == 0 ? "ALL PASS" : "SOME FAIL");
    return failures == 0 ? 0 : 1;
}

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    uint32_t vtor = SCB_VTOR;            /* where the boot ROM model started us */
    SCB_CPACR |= (0xFu << 20);           /* CP10/CP11 */
    if (src != dst) {
        while (dst < &_edata) {
            *dst++ = *src++;
        }
    }
    for (dst = &_sbss; dst < &_ebss; ++dst) {
        *dst = 0u;
    }
    boot_vtor = vtor;
    SCB_VTOR = (uint32_t)&_vectors_start;
    __asm volatile("cpsie i");
    exit(main());
}
