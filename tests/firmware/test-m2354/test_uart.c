/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * M2354 console and clock-tree bring-up test.
 */
#include <stdio.h>
#include "m2354.h"

static int test_pdid(void)
{
    uint32_t pdid = SYS_PDID;
    int ok = (pdid == 0x00235400u);
    printf("pdid:    0x%08lx  exp 0x00235400  %s\n",
           (unsigned long)pdid, ok ? "PASS" : "FAIL");
    return ok;
}

static int test_reglctl(void)
{
    int locked_after_reset;
    int unlocked;
    int relocked;
    int ok;

    SYS_REGLCTL = 0u;
    locked_after_reset = (SYS_REGLCTL == 0u);

    /* A partial sequence must not unlock. */
    SYS_REGLCTL = 0x59u;
    SYS_REGLCTL = 0x16u;
    unlocked = (SYS_REGLCTL != 0u);
    if (unlocked) {
        printf("reglctl: unlocked early             FAIL\n");
        return 0;
    }

    SYS_UNLOCK();
    unlocked = (SYS_REGLCTL != 0u);
    SYS_LOCK();
    relocked = (SYS_REGLCTL == 0u);

    ok = locked_after_reset && unlocked && relocked;
    printf("reglctl: lock/unlock/relock         %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_clock(void)
{
    uint32_t sel = CLK_CLKSEL0 & CLK_CLKSEL0_HCLKSEL_Msk;
    uint32_t sts = CLK_STATUS;
    int ok = (sel == CLK_CLKSEL0_HCLKSEL_PLL) &&
             ((sts & CLK_STATUS_PLLSTB) != 0u) &&
             ((sts & CLK_STATUS_HXTSTB) != 0u) &&
             ((sts & CLK_STATUS_HIRCSTB) != 0u);
    printf("clock:   hclksel=%lu status=0x%08lx  %s\n",
           (unsigned long)sel, (unsigned long)sts, ok ? "PASS" : "FAIL");
    return ok;
}

static int test_power_level(void)
{
    /* clock_init() selected PL0; PLSTS.PLSTATUS must have followed. */
    uint32_t plsts = SYS_PLSTS;
    int ok = ((plsts & SYS_PLSTS_PLCBUSY) == 0u) &&
             (((plsts >> 8) & 0x3u) == 0u);
    printf("power:   plsts=0x%08lx             %s\n",
           (unsigned long)plsts, ok ? "PASS" : "FAIL");
    return ok;
}

static int test_fifosts(void)
{
    uint32_t sts = UART0_FIFOSTS;
    int ok = ((sts & UART_FIFOSTS_TXFULL) == 0u) &&
             ((sts & UART_FIFOSTS_TXEMPTY) != 0u);
    printf("fifosts: 0x%08lx                  %s\n",
           (unsigned long)sts, ok ? "PASS" : "FAIL");
    return ok;
}

int main(void)
{
    int all = 1;
    printf("=== M2354 uart/clock test ===\n");
    all &= test_pdid();
    all &= test_reglctl();
    all &= test_clock();
    all &= test_power_level();
    all &= test_fifosts();
    printf("=== %s ===\n", all ? "ALL PASS" : "SOME FAIL");
    return all ? 0 : 1;
}
