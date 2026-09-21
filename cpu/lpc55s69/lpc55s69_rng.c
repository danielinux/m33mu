/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * LPC55S69 TRNG functional model.
 *
 * Register layout per CMSIS RNG_Type (LPC55S69, verified against the SVD):
 *   0x00  RANDOM_NUMBER   32-bit random value, computed on each read
 *   0x08  COUNTER_VAL     CLK_RATIO[7:0], REFRESH_CNT[12:8]
 *   0x0C  COUNTER_CFG     MODE[1:0], CLOCK_SEL[4:2], SHIFT4X[7:5]
 *   0x10  ONLINE_TEST_CFG ACTIVATE[0], DATA_SEL[2:1]
 *   0x14  ONLINE_TEST_VAL LIVE_CHI_SQUARED[3:0], MIN_CHI_SQUARED[7:4],
 *                         MAX_CHI_SQUARED[11:8]
 *   0xFFC MODULEID        0xA0B83200
 *
 * The NXP fsl_rng driver (rng_1) relies on this sequence:
 *  - RNG_Init: activate the chi-squared test; at power-on the min value
 *    reads above the max, and after the first read it settles below.
 *  - rng_readEntropy: poll COUNTER_VAL until REFRESH_CNT reaches 31,
 *    read RANDOM_NUMBER (which resets the counter to 0), then require
 *    MAX_CHI_SQUARED <= 4.
 */

#include "lpc55s69/lpc55s69_mmio.h"
#include "lpc55s69/lpc55s69_rng.h"
#include "m33mu/host_rng.h"
#include "m33mu/mmio.h"
#include "m33mu/types.h"

#define RNG_BASE_NS 0x4003A000u
#define RNG_BASE_S  0x5003A000u
#define RNG_SIZE    0x1000u

/* SYSCON AHBCLKCTRL2 bit 13 = RNG clock/reset (kCLOCK_Rng) */
#define RNG_AHBCLK_OFFSET 0x208u
#define RNG_AHBCLK_BIT    13u

#define RNG_OFF_RANDOM_NUMBER   0x00u
#define RNG_OFF_COUNTER_VAL     0x08u
#define RNG_OFF_COUNTER_CFG     0x0Cu
#define RNG_OFF_ONLINE_TEST_CFG 0x10u
#define RNG_OFF_ONLINE_TEST_VAL 0x14u
#define RNG_OFF_MODULEID        0xFFCu

#define RNG_MODULEID_VALUE 0xA0B83200u

struct lpc55s69_rng_state {
    mm_u32 counter_cfg;
    mm_u32 online_test_cfg;
    /* model state */
    mm_u32 ref_cnt; /* 5 bits, saturates at 31 */
    mm_bool chi_armed;
    mm_bool chi_converged;
    mm_bool random_read;
};

static struct lpc55s69_rng_state rng;

void mm_lpc55s69_rng_reset(void)
{
    rng.counter_cfg     = 0;
    rng.online_test_cfg = 0;
    rng.ref_cnt         = 31u;
    rng.chi_armed       = MM_FALSE;
    rng.chi_converged   = MM_FALSE;
    rng.random_read     = MM_FALSE;
}

static mm_u32 rng_online_test_val(struct lpc55s69_rng_state *s)
{
    if (!s->chi_armed) {
        return 0;
    }
    if (!s->chi_converged) {
        /* first read after activation: min above max */
        s->chi_converged = MM_TRUE;
        return (15u << 4) | (0u << 8);
    }
    if (!s->random_read) {
        return (10u << 4) | (15u << 8);
    }
    return (10u << 4) | (4u << 8);
}

static mm_bool rng_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    struct lpc55s69_rng_state *s = (struct lpc55s69_rng_state *)opaque;
    mm_u32 value = 0;

    if (s == 0 || value_out == 0 || size_bytes != 4u) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > RNG_SIZE) {
        return MM_FALSE;
    }
    if (!mm_lpc55s69_syscon_periph_active(RNG_AHBCLK_OFFSET, RNG_AHBCLK_BIT)) {
        return MM_FALSE;
    }

    switch (offset) {
    case RNG_OFF_RANDOM_NUMBER:
        value = mm_host_rng_u32();
        s->ref_cnt       = 0;
        s->random_read   = MM_TRUE;
        s->chi_converged = MM_TRUE;
        break;
    case RNG_OFF_COUNTER_VAL:
        value = 0x01u | ((s->ref_cnt & 0x1Fu) << 8);
        if (s->ref_cnt < 31u) {
            s->ref_cnt++;
        }
        break;
    case RNG_OFF_COUNTER_CFG:
        value = s->counter_cfg;
        break;
    case RNG_OFF_ONLINE_TEST_CFG:
        value = s->online_test_cfg;
        break;
    case RNG_OFF_ONLINE_TEST_VAL:
        value = rng_online_test_val(s);
        break;
    case RNG_OFF_MODULEID:
        value = RNG_MODULEID_VALUE;
        break;
    default:
        value = 0;
        break;
    }
    *value_out = value;
    return MM_TRUE;
}

static mm_bool rng_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    struct lpc55s69_rng_state *s = (struct lpc55s69_rng_state *)opaque;

    if (s == 0 || size_bytes != 4u) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > RNG_SIZE) {
        return MM_FALSE;
    }
    if (!mm_lpc55s69_syscon_periph_active(RNG_AHBCLK_OFFSET, RNG_AHBCLK_BIT)) {
        return MM_FALSE;
    }

    switch (offset) {
    case RNG_OFF_COUNTER_CFG:
        s->counter_cfg = value;
        break;
    case RNG_OFF_ONLINE_TEST_CFG:
        s->online_test_cfg = value;
        if ((value & 0x1u) != 0u) {
            s->chi_armed     = MM_TRUE;
            s->chi_converged = MM_FALSE;
            s->random_read   = MM_FALSE;
        } else {
            s->chi_armed = MM_FALSE;
        }
        break;
    default:
        break;
    }
    return MM_TRUE;
}

mm_bool mm_lpc55s69_rng_register(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) {
        return MM_FALSE;
    }
    reg.size   = RNG_SIZE;
    reg.opaque = &rng;
    reg.read   = rng_read;
    reg.write  = rng_write;
    reg.base   = RNG_BASE_NS;
    if (!mmio_bus_register_region(bus, &reg)) {
        return MM_FALSE;
    }
    reg.base = RNG_BASE_S;
    return mmio_bus_register_region(bus, &reg);
}
