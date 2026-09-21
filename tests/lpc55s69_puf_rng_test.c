/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * LPC55S69 PUF + TRNG functional model tests.
 *
 * Drives the register interface the same way the NXP fsl_puf / fsl_rng
 * drivers do, including the wolfBoot DICE sequence:
 *   Enroll -> reset -> Start(AC) -> GenerateKey -> reset -> Start(AC)
 *   -> GetKey(KC).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"
#include "lpc55s69/lpc55s69_mmio.h"

#define SYSCON_BASE      0x40000000u
#define SYSCON_AHBCLKCTRL2 0x208u
#define RNG_BASE         0x4003A000u
#define PUF_BASE         0x4003B000u

#define RNG_OFF_RANDOM_NUMBER   0x00u
#define RNG_OFF_COUNTER_VAL     0x08u
#define RNG_OFF_COUNTER_CFG     0x0Cu
#define RNG_OFF_ONLINE_TEST_CFG 0x10u
#define RNG_OFF_ONLINE_TEST_VAL 0x14u
#define RNG_OFF_MODULEID        0xFFCu

#define PUF_OFF_CTRL        0x00u
#define PUF_OFF_KEYINDEX    0x04u
#define PUF_OFF_KEYSIZE     0x08u
#define PUF_OFF_STAT        0x20u
#define PUF_OFF_ALLOW       0x28u
#define PUF_OFF_KEYINPUT    0x40u
#define PUF_OFF_CODEINPUT   0x44u
#define PUF_OFF_CODEOUTPUT  0x48u
#define PUF_OFF_KEYOUTINDEX 0x60u
#define PUF_OFF_KEYOUTPUT   0x64u
#define PUF_OFF_PWRCTRL     0x108u
#define PUF_OFF_SHIFT_STATUS 0x25Cu

#define PUF_CTRL_ENROLL      0x2u
#define PUF_CTRL_START       0x4u
#define PUF_CTRL_GENERATEKEY 0x8u
#define PUF_CTRL_SETKEY      0x10u
#define PUF_CTRL_GETKEY      0x40u
#define PUF_CTRL_ZEROIZE     0x1u

#define PUF_STAT_BUSY         0x1u
#define PUF_STAT_SUCCESS      0x2u
#define PUF_STAT_ERROR        0x4u
#define PUF_STAT_KEYINREQ     0x10u
#define PUF_STAT_KEYOUTAVAIL  0x20u
#define PUF_STAT_CODEINREQ    0x40u
#define PUF_STAT_CODEOUTAVAIL 0x80u

#define PUF_AC_WORDS 298u
#define PUF_KC_WORDS_32B 13u /* 20-byte header + 32-byte key */
#define PUF_KEY_WORDS_32B 8u

static void test_init_map(struct mm_memmap *map, struct mmio_region *regions,
                          size_t cap)
{
    mm_memmap_init(map, regions, cap);
    mm_lpc55s69_mmio_reset();
    (void)mm_lpc55s69_register_mmio(&map->mmio);
    /* PUF = AHBCLKCTRL2 bit 23, RNG = bit 13; PRESETCTRL2 stays 0 */
    (void)mmio_bus_write(&map->mmio, SYSCON_BASE + SYSCON_AHBCLKCTRL2, 4u,
                         (1u << 23) | (1u << 13));
}

/* Simulates a machine reboot: full reset + re-enable clocks. */
static void test_reboot(struct mm_memmap *map)
{
    mm_lpc55s69_mmio_reset();
    (void)mmio_bus_write(&map->mmio, SYSCON_BASE + SYSCON_AHBCLKCTRL2, 4u,
                         (1u << 23) | (1u << 13));
}

static int rd(struct mm_memmap *map, mm_u32 addr, mm_u32 *value)
{
    return mmio_bus_read(&map->mmio, addr, 4u, value) ? 0 : 1;
}

static int wr(struct mm_memmap *map, mm_u32 addr, mm_u32 value)
{
    return mmio_bus_write(&map->mmio, addr, 4u, value) ? 0 : 1;
}

static int test_rng_module_id(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u32 value;

    test_init_map(&map, regions, 64u);
    if (rd(&map, RNG_BASE + RNG_OFF_MODULEID, &value) != 0)
        return 1;
    if (value != 0xA0B83200u)
        return 2;
    return 0;
}

/* Emulates rng_accumulateEntropy + rng_readEntropy from fsl_rng.c. */
static int test_rng_entropy_flow(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u32 value, min_chi, max_chi, refresh, v1, v2;
    int i;

    test_init_map(&map, regions, 64u);

    /* activate chi-squared test */
    if (wr(&map, RNG_BASE + RNG_OFF_COUNTER_CFG, 0x10u) != 0)
        return 1;
    if (wr(&map, RNG_BASE + RNG_OFF_ONLINE_TEST_CFG, 0x1u) != 0)
        return 1;

    /* first read: min above max */
    if (rd(&map, RNG_BASE + RNG_OFF_ONLINE_TEST_VAL, &value) != 0)
        return 2;
    min_chi = (value >> 4) & 0xFu;
    max_chi = (value >> 8) & 0xFu;
    if (min_chi <= max_chi)
        return 3;

    /* settle: min below max-1 (the driver's loop exit condition) */
    for (i = 0; i < 16; i++) {
        if (rd(&map, RNG_BASE + RNG_OFF_ONLINE_TEST_VAL, &value) != 0)
            return 4;
        min_chi = (value >> 4) & 0xFu;
        max_chi = (value >> 8) & 0xFu;
        if (min_chi <= (max_chi - 1u)) {
            break;
        }
    }
    if (i == 16)
        return 5;

    /* poll REFRESH_CNT until 31 */
    for (i = 0; i < 64; i++) {
        if (rd(&map, RNG_BASE + RNG_OFF_COUNTER_VAL, &value) != 0)
            return 6;
        refresh = (value >> 8) & 0x1Fu;
        if (refresh == 31u) {
            break;
        }
    }
    if (i == 64)
        return 7;

    /* read a random number; counter resets to 0 */
    if (rd(&map, RNG_BASE + RNG_OFF_RANDOM_NUMBER, &v1) != 0)
        return 8;
    if (rd(&map, RNG_BASE + RNG_OFF_COUNTER_VAL, &value) != 0)
        return 9;
    if (((value >> 8) & 0x1Fu) != 0u)
        return 10;

    /* after a read, max chi-squared must be <= 4 */
    if (rd(&map, RNG_BASE + RNG_OFF_ONLINE_TEST_VAL, &value) != 0)
        return 11;
    max_chi = (value >> 8) & 0xFu;
    if (max_chi > 4u)
        return 12;

    /* a second draw works and differs from the first */
    if (wr(&map, RNG_BASE + RNG_OFF_ONLINE_TEST_CFG, 0x1u) != 0)
        return 13;
    for (i = 0; i < 64; i++) {
        if (rd(&map, RNG_BASE + RNG_OFF_COUNTER_VAL, &value) != 0)
            return 14;
        if (((value >> 8) & 0x1Fu) == 31u) {
            break;
        }
    }
    if (i == 64)
        return 15;
    if (rd(&map, RNG_BASE + RNG_OFF_RANDOM_NUMBER, &v2) != 0)
        return 16;
    if (v1 == v2)
        return 17;
    if (rd(&map, RNG_BASE + RNG_OFF_ONLINE_TEST_VAL, &value) != 0)
        return 18;
    if (((value >> 8) & 0xFu) > 4u)
        return 19;
    return 0;
}

static int test_puf_poweron_stat(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u32 value;

    test_init_map(&map, regions, 64u);

    /* at reset the RAM is off: PWRCTRL = 0xF8, STAT = 0 */
    if (rd(&map, PUF_BASE + PUF_OFF_PWRCTRL, &value) != 0)
        return 1;
    if (value != 0xF8u)
        return 2;
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 3;
    if (value != 0u)
        return 4;

    /* power on: RAMON=1, RAMSTAT follows (0xF8 | 0x1 | 0x2) */
    if (wr(&map, PUF_BASE + PUF_OFF_PWRCTRL, 0x1u) != 0)
        return 5;
    if (rd(&map, PUF_BASE + PUF_OFF_PWRCTRL, &value) != 0)
        return 6;
    if (value != 0xFBu)
        return 7;

    /* idle: SUCCESS, all operations allowed */
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 8;
    if (value != PUF_STAT_SUCCESS)
        return 9;
    if (rd(&map, PUF_BASE + PUF_OFF_ALLOW, &value) != 0)
        return 10;
    if (value != 0xFu)
        return 11;
    return 0;
}

static int puf_enroll(struct mm_memmap *map, mm_u8 ac[PUF_AC_WORDS * 4u])
{
    mm_u32 value;
    int i;

    if (wr(map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_ENROLL) != 0)
        return 1;
    if (rd(map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 2;
    if ((value & (PUF_STAT_BUSY | PUF_STAT_CODEOUTAVAIL)) !=
        (PUF_STAT_BUSY | PUF_STAT_CODEOUTAVAIL)) {
        return 3;
    }
    for (i = 0; i < (int)PUF_AC_WORDS; i++) {
        if (rd(map, PUF_BASE + PUF_OFF_CODEOUTPUT, &value) != 0)
            return 4;
        ac[i * 4u + 0u] = (mm_u8)(value & 0xffu);
        ac[i * 4u + 1u] = (mm_u8)((value >> 8) & 0xffu);
        ac[i * 4u + 2u] = (mm_u8)((value >> 16) & 0xffu);
        ac[i * 4u + 3u] = (mm_u8)((value >> 24) & 0xffu);
    }
    if (rd(map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 5;
    if ((value & PUF_STAT_SUCCESS) == 0u)
        return 6;
    return 0;
}

static int puf_start(struct mm_memmap *map, const mm_u8 ac[PUF_AC_WORDS * 4u])
{
    mm_u32 value;
    int i;

    if (wr(map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_START) != 0)
        return 1;
    if (rd(map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 2;
    if ((value & (PUF_STAT_BUSY | PUF_STAT_CODEINREQ)) !=
        (PUF_STAT_BUSY | PUF_STAT_CODEINREQ)) {
        return 3;
    }
    for (i = 0; i < (int)PUF_AC_WORDS; i++) {
        value = ((mm_u32)ac[i * 4u + 0u]) |
                (((mm_u32)ac[i * 4u + 1u]) << 8) |
                (((mm_u32)ac[i * 4u + 2u]) << 16) |
                (((mm_u32)ac[i * 4u + 3u]) << 24);
        if (wr(map, PUF_BASE + PUF_OFF_CODEINPUT, value) != 0)
            return 4;
    }
    if (rd(map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 5;
    return (value & PUF_STAT_SUCCESS) ? 0 : 6;
}

static int puf_power_on(struct mm_memmap *map)
{
    if (wr(map, PUF_BASE + PUF_OFF_PWRCTRL, 0x1u) != 0)
        return 1;
    return 0;
}

static int test_puf_enroll_start_roundtrip(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u8 ac[PUF_AC_WORDS * 4u];
    int rc;

    test_init_map(&map, regions, 64u);
    if (puf_power_on(&map) != 0)
        return 1;
    rc = puf_enroll(&map, ac);
    if (rc != 0)
        return 100 + rc;

    /* reboot: the AC must still be accepted */
    test_reboot(&map);
    if (puf_power_on(&map) != 0)
        return 2;
    rc = puf_start(&map, ac);
    if (rc != 0)
        return 200 + rc;
    return 0;
}

static int test_puf_start_bad_ac(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u8 ac[PUF_AC_WORDS * 4u];
    mm_u32 value;
    int rc;

    test_init_map(&map, regions, 64u);
    if (puf_power_on(&map) != 0)
        return 1;
    rc = puf_enroll(&map, ac);
    if (rc != 0)
        return 100 + rc;

    test_reboot(&map);
    if (puf_power_on(&map) != 0)
        return 2;
    ac[100] ^= 0x5Au; /* corrupt one AC byte */
    if (wr(&map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_START) != 0)
        return 3;
    {
        int i;
        for (i = 0; i < (int)PUF_AC_WORDS; i++) {
            value = ((mm_u32)ac[i * 4u + 0u]) |
                    (((mm_u32)ac[i * 4u + 1u]) << 8) |
                    (((mm_u32)ac[i * 4u + 2u]) << 16) |
                    (((mm_u32)ac[i * 4u + 3u]) << 24);
            if (wr(&map, PUF_BASE + PUF_OFF_CODEINPUT, value) != 0)
                return 4;
        }
    }
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 5;
    if ((value & PUF_STAT_ERROR) == 0u)
        return 6;
    return 0;
}

static int test_puf_genkey_getkey_dice(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u8 ac[PUF_AC_WORDS * 4u];
    mm_u8 kc[PUF_KC_WORDS_32B * 4u];
    mm_u32 key[PUF_KEY_WORDS_32B];
    mm_u32 value;
    int rc, i;

    test_init_map(&map, regions, 64u);
    if (puf_power_on(&map) != 0)
        return 1;
    rc = puf_enroll(&map, ac);
    if (rc != 0)
        return 100 + rc;
    rc = puf_start(&map, ac);
    if (rc != 0)
        return 200 + rc;

    /* GenerateKey: index 14, 32 bytes */
    if (wr(&map, PUF_BASE + PUF_OFF_KEYSIZE, 4u) != 0)
        return 2;
    if (wr(&map, PUF_BASE + PUF_OFF_KEYINDEX, 14u) != 0)
        return 3;
    if (wr(&map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_GENERATEKEY) != 0)
        return 4;
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 5;
    if ((value & (PUF_STAT_BUSY | PUF_STAT_CODEOUTAVAIL)) !=
        (PUF_STAT_BUSY | PUF_STAT_CODEOUTAVAIL)) {
        return 6;
    }
    for (i = 0; i < (int)PUF_KC_WORDS_32B; i++) {
        if (rd(&map, PUF_BASE + PUF_OFF_CODEOUTPUT, &value) != 0)
            return 7;
        kc[i * 4u + 0u] = (mm_u8)(value & 0xffu);
        kc[i * 4u + 1u] = (mm_u8)((value >> 8) & 0xffu);
        kc[i * 4u + 2u] = (mm_u8)((value >> 16) & 0xffu);
        kc[i * 4u + 3u] = (mm_u8)((value >> 24) & 0xffu);
    }
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 8;
    if ((value & PUF_STAT_SUCCESS) == 0u)
        return 9;

    /* KC header: byte1 = index, byte3 = key words */
    if ((kc[1] & 0xFu) != 14u)
        return 10;
    if (kc[3] != PUF_KEY_WORDS_32B)
        return 11;

    /* reboot, restart, GetKey */
    test_reboot(&map);
    if (puf_power_on(&map) != 0)
        return 12;
    rc = puf_start(&map, ac);
    if (rc != 0)
        return 200 + rc;

    if (wr(&map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_GETKEY) != 0)
        return 13;
    for (i = 0; i < (int)PUF_KC_WORDS_32B; i++) {
        value = ((mm_u32)kc[i * 4u + 0u]) |
                (((mm_u32)kc[i * 4u + 1u]) << 8) |
                (((mm_u32)kc[i * 4u + 2u]) << 16) |
                (((mm_u32)kc[i * 4u + 3u]) << 24);
        if (wr(&map, PUF_BASE + PUF_OFF_CODEINPUT, value) != 0)
            return 14;
    }
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 15;
    if ((value & (PUF_STAT_BUSY | PUF_STAT_KEYOUTAVAIL)) !=
        (PUF_STAT_BUSY | PUF_STAT_KEYOUTAVAIL)) {
        return 16;
    }
    if (rd(&map, PUF_BASE + PUF_OFF_KEYOUTINDEX, &value) != 0)
        return 17;
    if ((value & 0xFu) != 14u)
        return 18;
    for (i = 0; i < (int)PUF_KEY_WORDS_32B; i++) {
        if (rd(&map, PUF_BASE + PUF_OFF_KEYOUTPUT, &key[i]) != 0)
            return 19;
    }
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 20;
    if ((value & PUF_STAT_SUCCESS) == 0u)
        return 21;

    /* the key must equal the key part of the KC */
    if (memcmp(key, kc + 20u, sizeof(key)) != 0)
        return 22;
    return 0;
}

static int test_puf_getkey_tamper(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u8 ac[PUF_AC_WORDS * 4u];
    mm_u8 kc[PUF_KC_WORDS_32B * 4u];
    mm_u32 value;
    int rc, i;

    test_init_map(&map, regions, 64u);
    if (puf_power_on(&map) != 0)
        return 1;
    rc = puf_enroll(&map, ac);
    if (rc != 0)
        return 100 + rc;
    rc = puf_start(&map, ac);
    if (rc != 0)
        return 200 + rc;
    if (wr(&map, PUF_BASE + PUF_OFF_KEYSIZE, 4u) != 0)
        return 2;
    if (wr(&map, PUF_BASE + PUF_OFF_KEYINDEX, 14u) != 0)
        return 3;
    if (wr(&map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_GENERATEKEY) != 0)
        return 4;
    for (i = 0; i < (int)PUF_KC_WORDS_32B; i++) {
        if (rd(&map, PUF_BASE + PUF_OFF_CODEOUTPUT, &value) != 0)
            return 5;
        kc[i * 4u + 0u] = (mm_u8)(value & 0xffu);
        kc[i * 4u + 1u] = (mm_u8)((value >> 8) & 0xffu);
        kc[i * 4u + 2u] = (mm_u8)((value >> 16) & 0xffu);
        kc[i * 4u + 3u] = (mm_u8)((value >> 24) & 0xffu);
    }

    test_reboot(&map);
    if (puf_power_on(&map) != 0)
        return 6;
    rc = puf_start(&map, ac);
    if (rc != 0)
        return 200 + rc;

    kc[30] ^= 0x01u; /* tamper with the key part */
    if (wr(&map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_GETKEY) != 0)
        return 7;
    for (i = 0; i < (int)PUF_KC_WORDS_32B; i++) {
        value = ((mm_u32)kc[i * 4u + 0u]) |
                (((mm_u32)kc[i * 4u + 1u]) << 8) |
                (((mm_u32)kc[i * 4u + 2u]) << 16) |
                (((mm_u32)kc[i * 4u + 3u]) << 24);
        if (wr(&map, PUF_BASE + PUF_OFF_CODEINPUT, value) != 0)
            return 8;
    }
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 9;
    if ((value & PUF_STAT_ERROR) == 0u)
        return 10;
    return 0;
}

static int test_puf_hwkey_shift_status(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u8 ac[PUF_AC_WORDS * 4u];
    mm_u8 kc[PUF_KC_WORDS_32B * 4u];
    mm_u32 value;
    int rc, i;

    test_init_map(&map, regions, 64u);
    if (puf_power_on(&map) != 0)
        return 1;
    rc = puf_enroll(&map, ac);
    if (rc != 0)
        return 100 + rc;
    rc = puf_start(&map, ac);
    if (rc != 0)
        return 200 + rc;

    /* HW key: index 0, 32 bytes */
    if (wr(&map, PUF_BASE + PUF_OFF_KEYSIZE, 4u) != 0)
        return 2;
    if (wr(&map, PUF_BASE + PUF_OFF_KEYINDEX, 0u) != 0)
        return 3;
    if (wr(&map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_GENERATEKEY) != 0)
        return 4;
    for (i = 0; i < (int)PUF_KC_WORDS_32B; i++) {
        if (rd(&map, PUF_BASE + PUF_OFF_CODEOUTPUT, &value) != 0)
            return 5;
        kc[i * 4u + 0u] = (mm_u8)(value & 0xffu);
        kc[i * 4u + 1u] = (mm_u8)((value >> 8) & 0xffu);
        kc[i * 4u + 2u] = (mm_u8)((value >> 16) & 0xffu);
        kc[i * 4u + 3u] = (mm_u8)((value >> 24) & 0xffu);
    }
    if ((kc[1] & 0xFu) != 0u)
        return 6;

    test_reboot(&map);
    if (puf_power_on(&map) != 0)
        return 7;
    rc = puf_start(&map, ac);
    if (rc != 0)
        return 200 + rc;

    if (wr(&map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_GETKEY) != 0)
        return 8;
    for (i = 0; i < (int)PUF_KC_WORDS_32B; i++) {
        value = ((mm_u32)kc[i * 4u + 0u]) |
                (((mm_u32)kc[i * 4u + 1u]) << 8) |
                (((mm_u32)kc[i * 4u + 2u]) << 16) |
                (((mm_u32)kc[i * 4u + 3u]) << 24);
        if (wr(&map, PUF_BASE + PUF_OFF_CODEINPUT, value) != 0)
            return 9;
    }
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 10;
    if ((value & PUF_STAT_SUCCESS) == 0u)
        return 11;
    /* shift status slot 0 = 2*keyWords-1 = 15 */
    if (rd(&map, PUF_BASE + PUF_OFF_SHIFT_STATUS, &value) != 0)
        return 12;
    if ((value & 0xFu) != 15u)
        return 13;
    return 0;
}

static int test_puf_zeroize(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u32 value;

    test_init_map(&map, regions, 64u);
    if (puf_power_on(&map) != 0)
        return 1;
    if (wr(&map, PUF_BASE + PUF_OFF_CTRL, PUF_CTRL_ZEROIZE) != 0)
        return 2;
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 3;
    if ((value & PUF_STAT_ERROR) == 0u)
        return 4;
    if (rd(&map, PUF_BASE + PUF_OFF_ALLOW, &value) != 0)
        return 5;
    if (value != 0u)
        return 6;

    /* zeroize is permanent: survives a full machine reset */
    test_reboot(&map);
    if (puf_power_on(&map) != 0)
        return 7;
    if (rd(&map, PUF_BASE + PUF_OFF_STAT, &value) != 0)
        return 8;
    if ((value & PUF_STAT_ERROR) == 0u)
        return 9;
    if (rd(&map, PUF_BASE + PUF_OFF_ALLOW, &value) != 0)
        return 10;
    if (value != 0u)
        return 11;
    return 0;
}

int main(void)
{
    int rc;

    rc = test_rng_module_id();
    if (rc != 0) {
        printf("rng_module_id: FAIL (%d)\n", rc);
        return 1;
    }
    rc = test_rng_entropy_flow();
    if (rc != 0) {
        printf("rng_entropy_flow: FAIL (%d)\n", rc);
        return 1;
    }
    rc = test_puf_poweron_stat();
    if (rc != 0) {
        printf("puf_poweron_stat: FAIL (%d)\n", rc);
        return 1;
    }
    rc = test_puf_enroll_start_roundtrip();
    if (rc != 0) {
        printf("puf_enroll_start_roundtrip: FAIL (%d)\n", rc);
        return 1;
    }
    rc = test_puf_start_bad_ac();
    if (rc != 0) {
        printf("puf_start_bad_ac: FAIL (%d)\n", rc);
        return 1;
    }
    rc = test_puf_genkey_getkey_dice();
    if (rc != 0) {
        printf("puf_genkey_getkey_dice: FAIL (%d)\n", rc);
        return 1;
    }
    rc = test_puf_getkey_tamper();
    if (rc != 0) {
        printf("puf_getkey_tamper: FAIL (%d)\n", rc);
        return 1;
    }
    rc = test_puf_hwkey_shift_status();
    if (rc != 0) {
        printf("puf_hwkey_shift_status: FAIL (%d)\n", rc);
        return 1;
    }
    rc = test_puf_zeroize();
    if (rc != 0) {
        printf("puf_zeroize: FAIL (%d)\n", rc);
        return 1;
    }
    printf("lpc55s69_puf_rng_test: all tests passed\n");
    return 0;
}
