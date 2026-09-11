/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * M2354 FMC ISP engine test.  Mirrors the coverage of
 * tools/unit-tests/unit-flash-m2354.c in wolfBoot so the emulator and the
 * bootloader agree on flash semantics.
 */
#include <stdio.h>
#include "m2354.h"

/* Well clear of the loaded image, and page-aligned. */
#define SCRATCH      0x000C0000u
#define SCRATCH_NEXT (SCRATCH + FMC_FLASH_PAGE_SIZE)

static int isp_run(uint32_t cmd, uint32_t addr, uint32_t data)
{
    FMC_ISPCMD  = cmd;
    FMC_ISPADDR = addr;
    FMC_ISPDAT  = data;
    FMC_ISPTRG  = FMC_ISPTRG_ISPGO;
    while ((FMC_ISPTRG & FMC_ISPTRG_ISPGO) != 0u) { }
    if ((FMC_ISPCTL & FMC_ISPCTL_ISPFF) != 0u) {
        FMC_ISPCTL |= FMC_ISPCTL_ISPFF;   /* write-1-to-clear */
        return -1;
    }
    return 0;
}

static uint32_t flash_read(uint32_t addr)
{
    return REG32(addr);
}

static int page_erase(uint32_t addr)
{
    return isp_run(FMC_ISPCMD_PAGE_ERASE, addr, 0u);
}

static int test_erase_and_blank(void)
{
    int ok;
    if (page_erase(SCRATCH) != 0) {
        printf("erase:   command failed              FAIL\n");
        return 0;
    }
    ok = (flash_read(SCRATCH) == 0xFFFFFFFFu) &&
         (flash_read(SCRATCH + 0x7FCu) == 0xFFFFFFFFu);
    printf("erase:   page reads 0xFFFFFFFF      %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_blank_check(void)
{
    uint32_t result;
    int ok;

    if (isp_run(FMC_ISPCMD_RUN_ALL1, SCRATCH, FMC_FLASH_PAGE_SIZE) != 0) {
        printf("all1:    run failed                 FAIL\n");
        return 0;
    }
    FMC_ISPCMD  = FMC_ISPCMD_READ_ALL1;
    FMC_ISPADDR = SCRATCH;
    FMC_ISPTRG  = FMC_ISPTRG_ISPGO;
    while ((FMC_ISPTRG & FMC_ISPTRG_ISPGO) != 0u) { }
    result = FMC_ISPDAT;

    ok = (result == FMC_ALL1_BLANK);
    printf("all1:    0x%08lx blank             %s\n",
           (unsigned long)result, ok ? "PASS" : "FAIL");
    return ok;
}

static int test_program_word(void)
{
    int ok;
    if (isp_run(FMC_ISPCMD_PROGRAM, SCRATCH, 0xA5A5A5A5u) != 0) {
        printf("program: command failed             FAIL\n");
        return 0;
    }
    ok = (flash_read(SCRATCH) == 0xA5A5A5A5u);
    printf("program: 0x%08lx                  %s\n",
           (unsigned long)flash_read(SCRATCH), ok ? "PASS" : "FAIL");
    return ok;
}

static int test_program_and_semantics(void)
{
    /* A second program can only clear bits, never set them. */
    uint32_t v;
    int ok;
    if (isp_run(FMC_ISPCMD_PROGRAM, SCRATCH, 0x0F0F0F0Fu) != 0) {
        printf("and:     command failed             FAIL\n");
        return 0;
    }
    v = flash_read(SCRATCH);
    ok = (v == (0xA5A5A5A5u & 0x0F0F0F0Fu));
    printf("and:     0x%08lx exp 0x%08lx  %s\n",
           (unsigned long)v, (unsigned long)(0xA5A5A5A5u & 0x0F0F0F0Fu),
           ok ? "PASS" : "FAIL");
    return ok;
}

static int test_program_multi(void)
{
    uint32_t addr = SCRATCH + 0x10u;
    int ok;

    FMC_ISPCMD  = FMC_ISPCMD_PROGRAM_MUL;
    FMC_ISPADDR = addr;
    FMC_MPDAT0  = 0x11111111u;
    FMC_MPDAT1  = 0x22222222u;
    FMC_MPDAT2  = 0x33333333u;
    FMC_MPDAT3  = 0x44444444u;
    FMC_ISPTRG  = FMC_ISPTRG_ISPGO;
    while ((FMC_ISPTRG & FMC_ISPTRG_ISPGO) != 0u) { }
    if ((FMC_ISPCTL & FMC_ISPCTL_ISPFF) != 0u) {
        FMC_ISPCTL |= FMC_ISPCTL_ISPFF;
        printf("multi:   command failed             FAIL\n");
        return 0;
    }

    ok = (flash_read(addr + 0u)  == 0x11111111u) &&
         (flash_read(addr + 4u)  == 0x22222222u) &&
         (flash_read(addr + 8u)  == 0x33333333u) &&
         (flash_read(addr + 12u) == 0x44444444u);
    printf("multi:   16 bytes programmed        %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_page_stride(void)
{
    /* Erasing the next page must leave this one alone. */
    int ok;
    if (page_erase(SCRATCH_NEXT) != 0) {
        printf("stride:  erase failed               FAIL\n");
        return 0;
    }
    ok = (flash_read(SCRATCH) == (0xA5A5A5A5u & 0x0F0F0F0Fu)) &&
         (flash_read(SCRATCH_NEXT) == 0xFFFFFFFFu);
    printf("stride:  neighbour page intact      %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_bounds(void)
{
    /* Well past every flash block the part has: must fail and set ISPFF.
     * Note APROM_END is LDROM, not thin air, so it would not do here. */
    int rc = isp_run(FMC_ISPCMD_PROGRAM, 0x00400000u, 0u);
    int ok = (rc != 0);
    printf("bounds:  out-of-range rejected      %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_ldrom_locked(void)
{
    /* LDROM programming needs ISPCTL.LDUEN, which is not set here. */
    int rc = isp_run(FMC_ISPCMD_PROGRAM, 0x00100000u, 0x12345678u);
    int ok = (rc != 0);
    printf("lduen:   LDROM write rejected       %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_ns_alias(void)
{
    /* The ISP engine masks the alias bit, so both views hit one cell. */
    uint32_t addr = SCRATCH + 0x20u;
    int ok;
    if (isp_run(FMC_ISPCMD_PROGRAM, addr | NS_OFFSET, 0xDEADBEEFu) != 0) {
        printf("alias:   command failed             FAIL\n");
        return 0;
    }
    ok = (flash_read(addr) == 0xDEADBEEFu);
    printf("alias:   0x%08lx via NS address   %s\n",
           (unsigned long)flash_read(addr), ok ? "PASS" : "FAIL");
    return ok;
}

static int test_isp_read(void)
{
    uint32_t addr = SCRATCH + 0x20u;
    int ok;
    if (isp_run(FMC_ISPCMD_READ, addr, 0u) != 0) {
        printf("ispread: command failed             FAIL\n");
        return 0;
    }
    ok = (FMC_ISPDAT == 0xDEADBEEFu);
    printf("ispread: 0x%08lx                  %s\n",
           (unsigned long)FMC_ISPDAT, ok ? "PASS" : "FAIL");
    return ok;
}

int main(void)
{
    int all = 1;

    printf("=== M2354 FMC test ===\n");
    SYS_UNLOCK();
    FMC_ISPCTL |= FMC_ISPCTL_ISPEN | FMC_ISPCTL_APUEN;

    all &= test_erase_and_blank();
    all &= test_blank_check();
    all &= test_program_word();
    all &= test_program_and_semantics();
    all &= test_program_multi();
    all &= test_page_stride();
    all &= test_bounds();
    all &= test_ldrom_locked();
    all &= test_ns_alias();
    all &= test_isp_read();

    FMC_ISPCTL &= ~(FMC_ISPCTL_ISPEN | FMC_ISPCTL_APUEN);
    SYS_LOCK();
    printf("=== %s ===\n", all ? "ALL PASS" : "SOME FAIL");
    return all ? 0 : 1;
}
