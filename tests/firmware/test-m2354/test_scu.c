/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * M2354 SCU attribution registers.  wolfBoot's TrustZone build panics unless
 * SCU_FNSADDR reports the flash non-secure boundary the image was linked for,
 * so that read is the one that really matters here.
 */
#include <stdio.h>
#include "m2354.h"

/* SCU peripheral attribution index for UART0: PNSSET[3] bit 16 */
#define SCU_UART0_ATTR    (96 + 16)
#define SRAM_SECURE_SIZE  0x00018000u
#define SCU_SRAM_BLOCK    16384u
#define SCU_SRAM_BLOCKS   16u

static int test_fnsaddr(void)
{
    uint32_t v = SCU_FNSADDR;
    int ok = (v == 0x00080000u);
    printf("fnsaddr: 0x%08lx  exp 0x00080000  %s\n",
           (unsigned long)v, ok ? "PASS" : "FAIL");
    return ok;
}

static int test_fnsaddr_readonly(void)
{
    uint32_t before = SCU_FNSADDR;
    int ok;
    SCU_FNSADDR = 0x12345678u;
    ok = (SCU_FNSADDR == before);
    printf("fnsro:   write ignored              %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_sramnsset(void)
{
    uint32_t expect = 0u;
    unsigned int i;
    int ok;

    for (i = SRAM_SECURE_SIZE / SCU_SRAM_BLOCK; i < SCU_SRAM_BLOCKS; ++i) {
        SCU_SRAMNSSET |= (1u << i);
        expect |= (1u << i);
    }
    ok = (SCU_SRAMNSSET == expect);
    printf("sramns:  0x%08lx  exp 0x%08lx  %s\n",
           (unsigned long)SCU_SRAMNSSET, (unsigned long)expect,
           ok ? "PASS" : "FAIL");
    return ok;
}

static int test_pnsset(void)
{
    int ok;
    SCU_PNSSET(SCU_UART0_ATTR / 32) |= (1u << (SCU_UART0_ATTR & 31));
    ok = (SCU_PNSSET(3) & (1u << 16)) != 0u;
    printf("pnsset:  UART0 handed to NS         %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_ionsset(void)
{
    int ok;
    SCU_IONSSET(0) |= (1u << 6) | (1u << 7);
    ok = (SCU_IONSSET(0) & ((1u << 6) | (1u << 7))) == ((1u << 6) | (1u << 7));
    printf("ionsset: PA6/PA7 handed to NS       %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_ns_alias(void)
{
    /* The peripheral alias is the same register file seen from 0x5xxxxxxx. */
    volatile uint32_t *ns = (volatile uint32_t *)(uintptr_t)
                            (SCU_BASE + NS_OFFSET + 0x024u);
    int ok = (*ns == SCU_SRAMNSSET);
    printf("alias:   0x%08lx via NS address   %s\n",
           (unsigned long)*ns, ok ? "PASS" : "FAIL");
    return ok;
}

int main(void)
{
    int all = 1;
    printf("=== M2354 SCU test ===\n");
    all &= test_fnsaddr();
    all &= test_fnsaddr_readonly();
    all &= test_sramnsset();
    all &= test_pnsset();
    all &= test_ionsset();
    all &= test_ns_alias();
    printf("=== %s ===\n", all ? "ALL PASS" : "SOME FAIL");
    return all ? 0 : 1;
}
