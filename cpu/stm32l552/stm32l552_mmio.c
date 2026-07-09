/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#include <string.h>
#include <sys/random.h>
#include <stdlib.h>
#include <stdio.h>
#include "m33mu/pka.h"
#ifdef M33MU_HAS_WOLFSSL
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/sha512.h>
#endif
#include "stm32l552/stm32l552_mmio.h"
#include "stm32l552/stm32l552_usb.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/gpio.h"
#include "m33mu/nvic.h"
#include "m33mu/spi_bus.h"
#include "stm32_crypto.h"
#include "stm32_gpio.h"

extern void mm_system_request_reset(void);

/* RCC base addresses (system domain) */
#define RCC_BASE     0x40021000u
#define RCC_SEC_BASE 0x50021000u
#define RCC_SIZE     0x400u

#define RCC_CR       0x000u
#define RCC_CFGR     0x008u
#define RCC_PLLCFGR  0x00cu
#define RCC_CRRCR    0x098u

/* PWR base (system domain) */
#define PWR_BASE     0x40007000u
#define PWR_SEC_BASE 0x50007000u
#define PWR_SIZE     0x400u
#define PWR_CR1      0x000u
#define PWR_SR2      0x014u

/* FLASH controller base */
#define FLASH_BASE   0x40022000u
#define FLASH_SEC_BASE 0x50022000u
#define FLASH_SIZE   0x400u

/* GTZC TZSC/TZIC (secure / non-secure aliases) */
#define GTZC_TZSC_S_BASE  0x50032400u
#define GTZC_TZSC_NS_BASE 0x40032400u
#define GTZC_TZIC_S_BASE  0x50032800u
#define GTZC_TZIC_NS_BASE 0x40032800u
#define GTZC_TZSC_SIZE 0x400u
#define GTZC_TZIC_SIZE 0x400u
#define GTZC_BLK_SIZE 0x1000u

/* RNG base addresses */
#define RNG_BASE     0x420c0800u
#define RNG_SEC_BASE 0x520c0800u
#define RNG_SIZE     0x400u

/* AES/HASH/PKA base addresses */
#define AES_BASE     0x420c0000u
#define AES_SEC_BASE 0x520c0000u
#define AES_SIZE     0x400u
#define HASH_BASE     0x420c0400u
#define HASH_SEC_BASE 0x520c0400u
#define HASH_SIZE     0x400u
#define PKA_BASE     0x420c2000u
#define PKA_SEC_BASE 0x520c2000u
#define PKA_SIZE     0x2000u

/* MPCBB base addresses */
#define MPCBB1_BASE     0x40032c00u
#define MPCBB1_SEC_BASE 0x50032c00u
#define MPCBB2_BASE     0x40033000u
#define MPCBB2_SEC_BASE 0x50033000u
#define MPCBB_SIZE      0x400u

/* EXTI base addresses */
#define EXTI_BASE     0x4002F400u
#define EXTI_SEC_BASE 0x5002F400u
#define EXTI_SIZE     0x400u

/* IWDG/WWDG base addresses */
#define IWDG_BASE     0x40003000u
#define IWDG_SEC_BASE 0x50003000u
#define IWDG_SIZE     0x400u
#define WWDG_BASE     0x40002C00u
#define WWDG_SEC_BASE 0x50002C00u
#define WWDG_SIZE     0x400u

/* UCPD1 base addresses */
#define UCPD1_BASE     0x4000DC00u
#define UCPD1_SEC_BASE 0x5000DC00u
#define UCPD1_SIZE     0x400u

/* CRS base addresses */
#define CRS_BASE     0x40006000u
#define CRS_SEC_BASE 0x50006000u
#define CRS_SIZE     0x400u

/* TAMP base addresses */
#define TAMP_BASE     0x40003400u
#define TAMP_SEC_BASE 0x50003400u
#define TAMP_SIZE     0x400u

/* SYSCFG base addresses */
#define SYSCFG_BASE     0x40010000u
#define SYSCFG_SEC_BASE 0x50010000u
#define SYSCFG_SIZE     0x400u

/* DBGMCU base address (private peripheral bus, no secure alias) */
#define DBGMCU_BASE  0xE0044000u
#define DBGMCU_SIZE  0x400u

/* ICACHE base addresses */
#define ICACHE_BASE     0x40030400u
#define ICACHE_SEC_BASE 0x50030400u
#define ICACHE_SIZE     0x400u

/* UCPD1 register offsets (subset) */
#define UCPD_CFGR1 0x000u
#define UCPD_CR    0x00Cu
#define UCPD_IMR   0x010u
#define UCPD_SR    0x014u
#define UCPD_ICR   0x018u

/* UCPD1 bits (subset) */
#define UCPD_CFGR1_UCPDEN (1u << 31)
#define UCPD_CR_ANAMODE   (1u << 9)
#define UCPD_CR_CCENABLE_0 (1u << 10)
#define UCPD_CR_CCENABLE_1 (1u << 11)
#define UCPD_CR_CCENABLE_BOTH (UCPD_CR_CCENABLE_0 | UCPD_CR_CCENABLE_1)
#define UCPD_SR_TYPECEVT1 (1u << 14)
#define UCPD_SR_TYPECEVT2 (1u << 15)
#define UCPD_SR_TYPEC_VSTATE_CC1_Pos 16u
#define UCPD_SR_TYPEC_VSTATE_CC2_Pos 18u
#define UCPD_SR_TYPEC_VSTATE_CC_Msk (0x3u << UCPD_SR_TYPEC_VSTATE_CC1_Pos)
#define UCPD_SR_TYPEC_VSTATE_CC2_Msk (0x3u << UCPD_SR_TYPEC_VSTATE_CC2_Pos)
#define UCPD_ICR_TYPECEVT1CF (1u << 14)
#define UCPD_ICR_TYPECEVT2CF (1u << 15)

#define RNG_CR_OFFSET   0x0u
#define RNG_SR_OFFSET   0x4u
#define RNG_DR_OFFSET   0x8u
#define RNG_NSCR_OFFSET 0xcu
#define RNG_HTCR_OFFSET 0x10u

/* AES register offsets (subset) */
#define AES_CR    0x000u
#define AES_SR    0x004u
#define AES_DINR  0x008u
#define AES_DOUTR 0x00cu
#define AES_KEYR0 0x010u
#define AES_KEYR1 0x014u
#define AES_KEYR2 0x018u
#define AES_KEYR3 0x01cu
#define AES_IVR0  0x020u
#define AES_IVR1  0x024u
#define AES_IVR2  0x028u
#define AES_IVR3  0x02cu
#define AES_KEYR4 0x030u
#define AES_KEYR5 0x034u
#define AES_KEYR6 0x038u
#define AES_KEYR7 0x03cu
#define AES_IER   0x300u
#define AES_ISR   0x304u
#define AES_ICR   0x308u

#define AES_CR_EN        (1u << 0)
#define AES_CR_DATATYPE_SHIFT 1u
#define AES_CR_MODE_SHIFT 3u
#define AES_CR_CHMOD_SHIFT 5u
#define AES_CR_CHMOD_MASK (0x3u << AES_CR_CHMOD_SHIFT)
#define AES_CR_DMAINEN  (1u << 11)
#define AES_CR_DMAOUTEN (1u << 12)
#define AES_CR_CHMOD2   (1u << 16)
#define AES_CR_KEYSIZE  (1u << 18)
#define AES_CR_GCMPH_SHIFT 13u
#define AES_CR_GCMPH_MASK (0x3u << AES_CR_GCMPH_SHIFT)
#define AES_CR_NPBLB_SHIFT 20u
#define AES_CR_IPRST    (1u << 31)

#define AES_SR_CCF      (1u << 0)
#define AES_SR_RDERR    (1u << 1)
#define AES_SR_WRERR    (1u << 2)
#define AES_SR_BUSY     (1u << 3)
#define AES_SR_KEYVALID (1u << 7)

/* HASH register offsets (subset) */
#define HASH_CR   0x000u
#define HASH_DIN  0x004u
#define HASH_STR  0x008u
#define HASH_HRA0 0x00cu
#define HASH_HRA1 0x010u
#define HASH_HRA2 0x014u
#define HASH_HRA3 0x018u
#define HASH_HRA4 0x01cu
#define HASH_IMR  0x020u
#define HASH_SR   0x024u
#define HASH_CSR0 0x0f8u
#define HASH_HR0  0x310u

#define HASH_CR_INIT      (1u << 2)
#define HASH_CR_DATATYPE_SHIFT 4u
#define HASH_CR_MODE      (1u << 6)
#define HASH_CR_ALGO_SHIFT 17u

#define HASH_STR_NBLW_MASK 0x1fu
#define HASH_STR_DCAL      (1u << 8)

#define HASH_SR_DINIS (1u << 0)
#define HASH_SR_DCIS  (1u << 1)
#define HASH_SR_DMAS  (1u << 2)
#define HASH_SR_BUSY  (1u << 3)
#define HASH_SR_NBWP_SHIFT 9u
#define HASH_SR_DINNE (1u << 15)
#define HASH_SR_NBWE_SHIFT 16u

#define TZSC_SECCFGR2_OFFSET 0x14u
#define TZSC_AESSEC_BIT 12u
#define TZSC_HASHSEC_BIT 13u
#define TZSC_RNGSEC_BIT 14u
#define TZSC_PKASEC_BIT 15u

/* EXTI offsets (subset) */
#define EXTI_RTSR1  0x000u
#define EXTI_FTSR1  0x004u
#define EXTI_SWIER1 0x008u
#define EXTI_RPR1   0x00Cu
#define EXTI_FPR1   0x010u
#define EXTI_EXTICR1 0x060u
#define EXTI_EXTICR2 0x064u
#define EXTI_EXTICR3 0x068u
#define EXTI_EXTICR4 0x06Cu
#define EXTI_IMR1   0x080u
#define EXTI_EMR1   0x084u

/* WWDG offsets */
#define WWDG_CR  0x000u
#define WWDG_CFR 0x004u
#define WWDG_SR  0x008u

/* IWDG offsets */
#define IWDG_KR   0x000u
#define IWDG_PR   0x004u
#define IWDG_RLR  0x008u
#define IWDG_SR   0x00Cu
#define IWDG_WINR 0x010u
#define IWDG_EWCR 0x014u

#define GPIO_IDR_OFFSET 0x10u
#define GPIO_ODR_OFFSET 0x14u

/* MPCBB offsets */
#define MPCBB_CR_OFFSET      0x000u
#define MPCBB_SECCFGR_OFFSET 0x100u
#define MPCBB_CR_INVSECSTATE (1u << 30)

/* FLASH register offsets */
#define FLASH_ACR      0x000u
#define FLASH_NSKEYR   0x008u
#define FLASH_SECKEYR  0x00cu
#define FLASH_NSSR     0x020u
#define FLASH_SECSR    0x024u
#define FLASH_NSCR     0x028u
#define FLASH_SECCR    0x02cu
#define FLASH_NSCCR    0x030u
#define FLASH_SECCCR   0x034u
#define FLASH_OPTR     0x040u

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu

#define FLASH_FLAG_EOP    (1u << 0)
#define FLASH_FLAG_PGSERR (1u << 7)
#define FLASH_FLAG_BSY    (1u << 16)

#define FLASH_CR_PG        (1u << 0)
#define FLASH_CR_PER       (1u << 1)
#define FLASH_CR_MER1      (1u << 2)
#define FLASH_CR_PNB_SHIFT 3
#define FLASH_CR_PNB_MASK  (0x7fu << FLASH_CR_PNB_SHIFT)
#define FLASH_CR_BKER      (1u << 11)
#define FLASH_CR_MER2      (1u << 15)
#define FLASH_CR_STRT      (1u << 16)
#define FLASH_CR_LOCK      (1u << 31)

#define FLASH_SECTOR_COUNT_SINGLE 128u
#define FLASH_SECTOR_COUNT_DUAL   128u
#define FLASH_BANK_COUNT   2u
#define FLASH_CR_BKSEL     (1u << 31)
#define FLASH_OPTR_SWAP_BANK (1u << 20)
#define FLASH_OPTR_DBANK (1u << 22)

struct rcc_state {
    mm_u32 regs[RCC_SIZE / 4];
    mm_u64 cpu_hz;
};

struct pwr_state {
    mm_u32 regs[PWR_SIZE / 4];
};

struct simple_blk {
    mm_u32 regs[GTZC_BLK_SIZE / 4];
};

struct mpcbb_state {
    mm_u32 regs[MPCBB_SIZE / 4];
};

struct rng_state {
    mm_u32 regs[RNG_SIZE / 4];
    mm_u32 dr;
    mm_bool dr_valid;
};

#include "stm32_crypto_priv.h"

struct exti_state {
    mm_u32 regs[EXTI_SIZE / 4];
};

struct iwdg_state {
    mm_u32 regs[IWDG_SIZE / 4];
    mm_u32 counter;
    mm_u32 prescaler;
    mm_bool running;
    mm_bool write_access;
    mm_u64 accum;
};

struct wwdg_state {
    mm_u32 regs[WWDG_SIZE / 4];
    mm_u32 counter;
    mm_u64 accum;
};

struct flash_state {
    mm_u32 regs[FLASH_SIZE / 4];
    mm_u8 *flash;
    mm_u32 flash_size;
    mm_u32 base_s;
    mm_u32 base_ns;
    const struct mm_flash_persist *persist;
    mm_u32 flags;
    mm_u8 ns_key_stage;
    mm_u8 sec_key_stage;
    mm_bool dualbank_enabled;
    mm_bool swap_active;
};

/* L552 has legacy DMA, keep simple register block for now */
struct dma_state {
    mm_u32 regs[0x400 / 4];
};

struct ucpd_state {
    mm_u32 cfgr1;
    mm_u32 cr;
    mm_u32 imr;
    mm_u32 sr;
};

/* uintptr_t substitute for C90 */
typedef unsigned long mm_uptr;

static void rcc_update_ready(struct rcc_state *r);
static void rcc_update_sysclk(struct rcc_state *r);
static void pwr_update_vos(struct pwr_state *p);
static void mpcbb_init_defaults(void);
static void flash_sync_option_regs(struct flash_state *f);

static struct rcc_state rcc;
static struct pwr_state pwr;
static struct simple_blk tzsc_s;
static struct simple_blk tzsc_ns;
static struct simple_blk tzic_s;
static struct simple_blk tzic_ns;
static struct simple_blk crs;
static struct simple_blk crs_sec;
static struct simple_blk tamp;
static struct simple_blk tamp_sec;
static struct simple_blk syscfg;
static struct simple_blk syscfg_sec;
static struct simple_blk dbgmcu;
static struct simple_blk icache;
static struct simple_blk icache_sec;
static struct ucpd_state ucpd1_state;
static struct ucpd_state ucpd1_state_sec;
static struct mpcbb_state mpcbb[2];
static struct rng_state rng;
static struct hash_state hash_accel;
static struct aes_state aes_accel;
static struct pka_state pka_accel;
static struct exti_state exti;
static struct iwdg_state iwdg;
static struct wwdg_state wwdg;
static struct flash_state flash_ctl;
static struct stm32_gpio_state gpio[8]; /* A..H (L552 has 8 banks) */
static struct stm32_gpio_ctx gpio_ctx_data[16]; /* NS + S for each bank */
static struct dma_state dma1;
static struct dma_state dma2;
static void *rng_ctx[2][4];
static struct hash_ctx hash_ctx[2];
static struct aes_ctx aes_ctx[2];
static struct pka_ctx pka_ctx[2];
static struct mm_nvic *g_rng_nvic = 0;
static struct mm_nvic *g_exti_nvic = 0;
static struct mm_nvic *g_wdg_nvic = 0;

#define RNG_IRQ 94
static const mm_u32 mpcbb_words[] = { 32u, 32u };

static mm_u32 stm32l552_gpio_bank_read(void *opaque, int bank);
static mm_u32 stm32l552_gpio_bank_read_moder(void *opaque, int bank);
static mm_bool stm32l552_gpio_bank_clock(void *opaque, int bank);
static mm_u32 stm32l552_gpio_bank_read_seccfgr(void *opaque, int bank);
static void exti_gpio_update(int bank, mm_u32 old_level, mm_u32 new_level);
static mm_bool stm32l552_gpio_clock_enabled_cb(int bank);
static void exti_gpio_update_cb(int bank, mm_u32 old_level, mm_u32 new_level);

static void ucpd_update_attach(struct ucpd_state *u)
{
    if (u == 0) {
        return;
    }
    if ((u->cfgr1 & UCPD_CFGR1_UCPDEN) == 0u) {
        return;
    }
    if ((u->cr & (UCPD_CR_ANAMODE | UCPD_CR_CCENABLE_BOTH)) != (UCPD_CR_ANAMODE | UCPD_CR_CCENABLE_BOTH)) {
        return;
    }
    if ((u->sr & (UCPD_SR_TYPECEVT1 | UCPD_SR_TYPECEVT2)) == 0u) {
        u->sr |= UCPD_SR_TYPECEVT1;
    }
    u->sr &= ~(UCPD_SR_TYPEC_VSTATE_CC_Msk | UCPD_SR_TYPEC_VSTATE_CC2_Msk);
    u->sr |= (3u << UCPD_SR_TYPEC_VSTATE_CC1_Pos);
}

static mm_bool ucpd_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct ucpd_state *u = (struct ucpd_state *)opaque;
    mm_u32 value = 0;
    if (u == 0 || value_out == 0) {
        return MM_FALSE;
    }
    if (size_bytes != 4u) {
        return MM_FALSE;
    }
    ucpd_update_attach(u);
    switch (offset) {
    case UCPD_CFGR1:
        value = u->cfgr1;
        break;
    case UCPD_CR:
        value = u->cr;
        break;
    case UCPD_IMR:
        value = u->imr;
        break;
    case UCPD_SR:
        value = u->sr;
        break;
    default:
        value = 0;
        break;
    }
    *value_out = value;
    return MM_TRUE;
}

static mm_bool ucpd_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct ucpd_state *u = (struct ucpd_state *)opaque;
    if (u == 0) {
        return MM_FALSE;
    }
    if (size_bytes != 4u) {
        return MM_FALSE;
    }
    switch (offset) {
    case UCPD_CFGR1:
        u->cfgr1 = value;
        ucpd_update_attach(u);
        return MM_TRUE;
    case UCPD_CR:
        u->cr = value;
        ucpd_update_attach(u);
        return MM_TRUE;
    case UCPD_IMR:
        u->imr = value;
        return MM_TRUE;
    case UCPD_ICR:
        if ((value & UCPD_ICR_TYPECEVT1CF) != 0u) {
            u->sr &= ~UCPD_SR_TYPECEVT1;
        }
        if ((value & UCPD_ICR_TYPECEVT2CF) != 0u) {
            u->sr &= ~UCPD_SR_TYPECEVT2;
        }
        return MM_TRUE;
    default:
        return MM_TRUE;
    }
}

static mm_bool tzsc_requires_secure(const struct simple_blk *tzsc, mm_u32 bit)
{
    mm_u32 seccfgr2 = tzsc->regs[TZSC_SECCFGR2_OFFSET / 4u];
    return ((seccfgr2 >> bit) & 1u) != 0u;
}

static mm_bool hash_clock_enabled(const struct rcc_state *rcc)
{
    mm_u32 ahb2enr = rcc->regs[0x4c / 4];
    return ((ahb2enr >> 17) & 1u) != 0u;
}

static mm_bool aes_clock_enabled(const struct rcc_state *rcc, mm_bool is_saes)
{
    mm_u32 ahb2enr = rcc->regs[0x4c / 4];
    (void)is_saes;
    return ((ahb2enr >> 16) & 1u) != 0u;
}

static mm_bool hash_requires_secure(const struct simple_blk *tzsc)
{
    return tzsc_requires_secure(tzsc, TZSC_HASHSEC_BIT);
}

static mm_bool aes_requires_secure(const struct simple_blk *tzsc, mm_bool is_saes)
{
    (void)is_saes;
    return tzsc_requires_secure(tzsc, TZSC_AESSEC_BIT);
}

static mm_bool pka_clock_enabled(const struct rcc_state *rcc)
{
    mm_u32 ahb2enr = rcc->regs[0x4c / 4];
    return ((ahb2enr >> 19) & 1u) != 0u;
}

static mm_bool pka_requires_secure(const struct simple_blk *tzsc)
{
    return tzsc_requires_secure(tzsc, TZSC_PKASEC_BIT);
}


static mm_bool flash_trace_enabled(void)
{
    static mm_bool init = MM_FALSE;
    static mm_bool enabled = MM_FALSE;
    if (!init) {
        const char *env = getenv("M33MU_FLASH_TRACE");
        enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
        init = MM_TRUE;
    }
    return enabled;
}

static mm_bool stm32l552_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len);

static mm_bool stm32l552_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= 8) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0u) {
        name_out[0] = (char)('A' + bank);
        if (name_len > 1u) {
            name_out[1] = '\0';
        }
    }
    if (pins_out != 0) {
        *pins_out = 16;
    }
    return MM_TRUE;
}

void mm_stm32l552_mmio_reset(void)
{
    size_t i;
    mm_u32 optr_swap = flash_ctl.regs[FLASH_OPTR / 4u] & FLASH_OPTR_SWAP_BANK;
    mm_bool dualbank = flash_ctl.dualbank_enabled;
    memset(&rcc, 0, sizeof(rcc));
    memset(&pwr, 0, sizeof(pwr));
    memset(&tzsc_s, 0, sizeof(tzsc_s));
    memset(&tzsc_ns, 0, sizeof(tzsc_ns));
    memset(&tzic_s, 0, sizeof(tzic_s));
    memset(&tzic_ns, 0, sizeof(tzic_ns));
    memset(&rng, 0, sizeof(rng));
    memset(&hash_accel, 0, sizeof(hash_accel));
    memset(&aes_accel, 0, sizeof(aes_accel));
    memset(&pka_accel, 0, sizeof(pka_accel));
    memset(&pka_accel, 0, sizeof(pka_accel));
    memset(&exti, 0, sizeof(exti));
    memset(&iwdg, 0, sizeof(iwdg));
    memset(&wwdg, 0, sizeof(wwdg));
    memset(&flash_ctl, 0, sizeof(flash_ctl));
    flash_ctl.dualbank_enabled = dualbank;
    flash_ctl.swap_active = (optr_swap != 0u) ? MM_TRUE : MM_FALSE;
    memset(&crs, 0, sizeof(crs));
    memset(&crs_sec, 0, sizeof(crs_sec));
    memset(&ucpd1_state, 0, sizeof(ucpd1_state));
    memset(&ucpd1_state_sec, 0, sizeof(ucpd1_state_sec));
    mpcbb_init_defaults();
    memset(&dma1, 0, sizeof(dma1));
    memset(&dma2, 0, sizeof(dma2));
    /* Initialize GPIO with shared implementation */
    for (i = 0; i < sizeof(gpio) / sizeof(gpio[0]); ++i) {
        stm32_gpio_reset(&gpio[i], (int)i);
        gpio_ctx_data[i * 2].gpio = &gpio[i];
        gpio_ctx_data[i * 2].is_secure_alias = MM_FALSE;
        gpio_ctx_data[i * 2].bank_index = (int)i;
        gpio_ctx_data[i * 2].clock_enabled = stm32l552_gpio_clock_enabled_cb;
        gpio_ctx_data[i * 2].exti_update = exti_gpio_update_cb;
        gpio_ctx_data[i * 2 + 1].gpio = &gpio[i];
        gpio_ctx_data[i * 2 + 1].is_secure_alias = MM_TRUE;
        gpio_ctx_data[i * 2 + 1].bank_index = (int)i;
        gpio_ctx_data[i * 2 + 1].clock_enabled = stm32l552_gpio_clock_enabled_cb;
        gpio_ctx_data[i * 2 + 1].exti_update = exti_gpio_update_cb;
    }
    mm_gpio_bank_set_reader(stm32l552_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(stm32l552_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(stm32l552_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(stm32l552_gpio_bank_read_seccfgr, 0);
    mm_gpio_set_bank_info_reader(stm32l552_gpio_bank_info, 0);
    mm_rcc_set_clock_list_reader(stm32l552_rcc_clock_list_line, 0);
    /* Enable HSI by default and mark ready flags. */
    rcc.regs[0] |= 1u;
    rcc_update_ready(&rcc);
    rcc_update_sysclk(&rcc);
    iwdg.regs[IWDG_RLR / 4u] = 0x00000FFFu;
    iwdg.regs[IWDG_WINR / 4u] = 0x00000FFFu;
    wwdg.regs[WWDG_CR / 4u] = 0x0000007Fu;
    wwdg.regs[WWDG_CFR / 4u] = 0x0000007Fu;
    wwdg.counter = 0x7Fu;
    exti.regs[EXTI_IMR1 / 4u] = 0xFFFE0000u;
    /* Power ready flags. */
    pwr_update_vos(&pwr);

    /* RNG reset values */
    rng.regs[RNG_CR_OFFSET / 4] = 0x00871f00u;
    rng.regs[RNG_HTCR_OFFSET / 4] = 0x000072acu;
    hash_reset_state(&hash_accel, MM_TRUE);
    mm_pka_reset(&pka_accel);

    /* FLASH reset values */
    flash_ctl.regs[FLASH_ACR / 4] = 0x00000013u;
    flash_ctl.regs[FLASH_NSCR / 4] = FLASH_CR_LOCK;
    flash_ctl.regs[FLASH_SECCR / 4] = FLASH_CR_LOCK;
    flash_sync_option_regs(&flash_ctl);

    mm_stm32l552_usb_reset();
}

mm_u32 *mm_stm32l552_tzsc_regs(void)
{
    return tzsc_s.regs;
}


mm_bool mm_stm32l552_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    mm_u32 word;
    mm_u32 bit;
    mm_u32 val;
    mm_bool sec;
    if (bank < 0 || bank >= (int)(sizeof(mpcbb) / sizeof(mpcbb[0]))) {
        return MM_FALSE;
    }
    word = block_index / 32u;
    if (word >= mpcbb_words[bank]) {
        return MM_FALSE;
    }
    bit = block_index % 32u;
    val = mpcbb[bank].regs[(MPCBB_SECCFGR_OFFSET / 4u) + word];
    sec = ((val >> bit) & 1u) != 0u;
    if ((mpcbb[bank].regs[MPCBB_CR_OFFSET / 4u] & MPCBB_CR_INVSECSTATE) != 0u) {
        sec = !sec;
    }
    return sec;
}

static mm_bool gpio_clock_enabled(const struct rcc_state *rcc, int index)
{
    /* AHB2ENR offset 0x4C, GPIOAEN bit0..GPIOHEN bit7 */
    mm_u32 ahb2enr = rcc->regs[0x4c / 4];
    return ((ahb2enr >> index) & 1u) != 0u;
}

enum rcc_bus_kind {
    RCC_BUS_AHB2 = 0,
    RCC_BUS_APB1 = 1,
    RCC_BUS_APB1_2 = 2,
    RCC_BUS_APB2 = 3
};

struct rcc_clk_name {
    const char *name;
    enum rcc_bus_kind bus;
    mm_u32 bit;
};

static const struct rcc_clk_name rcc_clk_names[] = {
    { "GPIOA", RCC_BUS_AHB2, 0u },
    { "GPIOB", RCC_BUS_AHB2, 1u },
    { "GPIOC", RCC_BUS_AHB2, 2u },
    { "GPIOD", RCC_BUS_AHB2, 3u },
    { "GPIOE", RCC_BUS_AHB2, 4u },
    { "GPIOF", RCC_BUS_AHB2, 5u },
    { "GPIOG", RCC_BUS_AHB2, 6u },
    { "GPIOH", RCC_BUS_AHB2, 7u },
    { "AES", RCC_BUS_AHB2, 16u },
    { "HASH", RCC_BUS_AHB2, 17u },
    { "RNG", RCC_BUS_AHB2, 18u },
    { "PKA", RCC_BUS_AHB2, 19u },
    { "TIM2", RCC_BUS_APB1, 0u },
    { "TIM3", RCC_BUS_APB1, 1u },
    { "TIM4", RCC_BUS_APB1, 2u },
    { "TIM5", RCC_BUS_APB1, 3u },
    { "USART2", RCC_BUS_APB1, 17u },
    { "USART3", RCC_BUS_APB1, 18u },
    { "UART4", RCC_BUS_APB1, 19u },
    { "UART5", RCC_BUS_APB1, 20u },
    { "USART1", RCC_BUS_APB2, 14u },
    { "LPUART1", RCC_BUS_APB1_2, 0u }
};

static mm_u32 rcc_bus_reg(const struct rcc_state *r, enum rcc_bus_kind bus)
{
    if (r == 0) {
        return 0u;
    }
    switch (bus) {
    case RCC_BUS_AHB2:
        return r->regs[0x4c / 4];
    case RCC_BUS_APB1:
        return r->regs[0x58 / 4];
    case RCC_BUS_APB1_2:
        return r->regs[0x5c / 4];
    case RCC_BUS_APB2:
        return r->regs[0x60 / 4];
    default:
        return 0u;
    }
}

static const char *rcc_bus_name(enum rcc_bus_kind bus)
{
    switch (bus) {
    case RCC_BUS_AHB2: return "AHB2";
    case RCC_BUS_APB1: return "APB1";
    case RCC_BUS_APB1_2: return "APB1_2";
    case RCC_BUS_APB2: return "APB2";
    default: return "RCC";
    }
}

static mm_bool stm32l552_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len)
{
    enum rcc_bus_kind bus;
    int line_idx = 0;
    size_t i;
    (void)opaque;
    if (out == 0 || out_len == 0u) {
        return MM_FALSE;
    }
    for (bus = RCC_BUS_AHB2; bus <= RCC_BUS_APB2; ++bus) {
        mm_u32 reg = rcc_bus_reg(&rcc, bus);
        char buf[256];
        size_t pos = 0;
        mm_bool have = MM_FALSE;
        if (reg == 0u) {
            continue;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s:", rcc_bus_name(bus));
        for (i = 0; i < sizeof(rcc_clk_names) / sizeof(rcc_clk_names[0]); ++i) {
            if (rcc_clk_names[i].bus != bus) {
                continue;
            }
            if ((reg & (1u << rcc_clk_names[i].bit)) == 0u) {
                continue;
            }
            if (pos + 2u < sizeof(buf)) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", rcc_clk_names[i].name);
                have = MM_TRUE;
            }
        }
        if (!have) {
            continue;
        }
        if (line_idx == line) {
            snprintf(out, out_len, "%s", buf);
            return MM_TRUE;
        }
        line_idx++;
    }
    return MM_FALSE;
}

static mm_u32 stm32l552_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[STM32_GPIO_ODR_OFFSET / 4];
}

static mm_u32 stm32l552_gpio_bank_read_moder(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[STM32_GPIO_MODER_OFFSET / 4];
}

static mm_bool stm32l552_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    return gpio_clock_enabled(&rcc, bank);
}

static mm_u32 stm32l552_gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[STM32_GPIO_SECCFGR_OFFSET / 4];
}

static mm_bool stm32l552_gpio_clock_enabled_cb(int bank)
{
    return gpio_clock_enabled(&rcc, bank);
}

static void exti_gpio_update_cb(int bank, mm_u32 old_level, mm_u32 new_level)
{
    exti_gpio_update(bank, old_level, new_level);
    mm_spi_bus_poll_all();
}

static mm_bool rng_clock_enabled(const struct rcc_state *rcc)
{
    /* RCC_AHB2ENR offset 0x4C, RNGEN bit18 */
    mm_u32 ahb2enr = rcc->regs[0x4c / 4];
    return ((ahb2enr >> 18) & 1u) != 0u;
}

static mm_bool rng_requires_secure(const struct simple_blk *tzsc)
{
    return tzsc_requires_secure(tzsc, TZSC_RNGSEC_BIT);
}

static void rng_fill(struct rng_state *r)
{
    mm_u32 v = 0;
    ssize_t n = getrandom(&v, sizeof(v), GRND_NONBLOCK);
    if (n != (ssize_t)sizeof(v)) {
        v = 0;
    }
    r->dr = v;
    r->dr_valid = MM_TRUE;
    r->regs[RNG_SR_OFFSET / 4] |= 1u;
    if (g_rng_nvic != 0 && (r->regs[RNG_CR_OFFSET / 4] & (1u << 3)) != 0u) {
        mm_nvic_set_pending(g_rng_nvic, (mm_u32)RNG_IRQ, MM_TRUE);
    }
}

static mm_u32 flash_bank_count(const struct flash_state *f)
{
    if (f == 0) {
        return 1u;
    }
    return f->dualbank_enabled ? FLASH_BANK_COUNT : 1u;
}

static void flash_apply_bank_swap(struct flash_state *f)
{
    mm_u32 bank_size;
    mm_u32 i;
    if (f == 0 || f->flash == 0 || f->flash_size == 0u) {
        return;
    }
    if ((f->flash_size % 2u) != 0u) {
        return;
    }
    bank_size = f->flash_size / 2u;
    for (i = 0; i < bank_size; ++i) {
        mm_u8 tmp = f->flash[i];
        f->flash[i] = f->flash[i + bank_size];
        f->flash[i + bank_size] = tmp;
    }
    if (f->persist != 0 && f->persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)f->persist, 0u, f->flash_size);
    }
}

static void flash_set_swap_state(struct flash_state *f, mm_bool swap_active)
{
    if (f == 0) {
        return;
    }
    if (!f->dualbank_enabled) {
        if (f->swap_active) {
            flash_apply_bank_swap(f);
        }
        f->swap_active = MM_FALSE;
        return;
    }
    if (f->swap_active == swap_active) {
        return;
    }
    f->swap_active = swap_active;
    flash_apply_bank_swap(f);
}

static void flash_sync_option_regs(struct flash_state *f)
{
    mm_u32 val;
    if (f == 0) {
        return;
    }
    if (!f->dualbank_enabled) {
        f->swap_active = MM_FALSE;
    }
    val = f->regs[FLASH_OPTR / 4u];
    val &= ~(FLASH_OPTR_SWAP_BANK | FLASH_OPTR_DBANK);
    if (f->swap_active) {
        val |= FLASH_OPTR_SWAP_BANK;
    }
    if (f->dualbank_enabled) {
        val |= FLASH_OPTR_DBANK;
    }
    f->regs[FLASH_OPTR / 4u] = val;
}

static mm_u32 flash_sector_size(void)
{
    mm_u32 banks = flash_bank_count(&flash_ctl);
    mm_u32 sector_count = flash_ctl.dualbank_enabled ? FLASH_SECTOR_COUNT_DUAL
                                                     : FLASH_SECTOR_COUNT_SINGLE;
    if (flash_ctl.flash_size == 0u) {
        return 0u;
    }
    if (banks == 0u || (flash_ctl.flash_size % banks) != 0u) {
        banks = 1u;
    }
    return (sector_count == 0u) ? 0u
        : ((flash_ctl.flash_size / banks) / sector_count);
}

static void flash_set_busy(mm_u32 reg_offset, mm_bool busy)
{
    mm_u32 idx = reg_offset / 4u;
    if (busy) {
        flash_ctl.regs[idx] |= FLASH_FLAG_BSY;
    } else {
        flash_ctl.regs[idx] &= ~FLASH_FLAG_BSY;
    }
}

static void flash_set_eop(mm_u32 reg_offset)
{
    flash_ctl.regs[reg_offset / 4u] |= FLASH_FLAG_EOP;
}

static void flash_clear_eop(mm_u32 reg_offset)
{
    flash_ctl.regs[reg_offset / 4u] &= ~FLASH_FLAG_EOP;
}

static mm_bool flash_is_unlocked(mm_u32 reg_offset)
{
    return (flash_ctl.regs[reg_offset / 4u] & FLASH_CR_LOCK) == 0u;
}

static void flash_handle_key(mm_u32 offset, mm_u32 value)
{
    mm_u8 *stage = (offset == FLASH_NSKEYR) ? &flash_ctl.ns_key_stage : &flash_ctl.sec_key_stage;
    mm_u32 cr_off = (offset == FLASH_NSKEYR) ? FLASH_NSCR : FLASH_SECCR;
    if (*stage == 0u) {
        if (value == FLASH_KEY1) {
            *stage = 1u;
        } else {
            *stage = 0u;
        }
        return;
    }
    if (*stage == 1u) {
        if (value == FLASH_KEY2) {
            flash_ctl.regs[cr_off / 4u] &= ~FLASH_CR_LOCK;
        }
        *stage = 0u;
    }
}

static void flash_apply_erase(mm_u32 cr_off, mm_u32 sr_off)
{
    mm_u32 cr = flash_ctl.regs[cr_off / 4u];
    mm_u32 sector_size = flash_sector_size();
    mm_u32 bank_size = 0;
    mm_u32 bank_offset = 0;
    mm_u32 bank_count = flash_bank_count(&flash_ctl);
    mm_u32 start = 0;
    mm_u32 length = 0;
    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0 || sector_size == 0) {
        return;
    }
    if ((cr & FLASH_CR_MER1) != 0u || (cr & FLASH_CR_MER2) != 0u) {
        if (FLASH_BANK_COUNT > 1u && flash_ctl.flash_size >= FLASH_BANK_COUNT) {
            bank_size = flash_ctl.flash_size / FLASH_BANK_COUNT;
        } else if (bank_count != 0u) {
            bank_size = flash_ctl.flash_size / bank_count;
        }
        if (bank_size == 0u) {
            start = 0;
            length = flash_ctl.flash_size;
        } else if ((cr & FLASH_CR_MER2) != 0u) {
            start = bank_size;
            length = bank_size;
        } else {
            start = 0;
            length = bank_size;
        }
    } else if ((cr & FLASH_CR_PER) != 0u) {
        mm_u32 pnb = (cr & FLASH_CR_PNB_MASK) >> FLASH_CR_PNB_SHIFT;
        start = pnb * sector_size;
        if (FLASH_BANK_COUNT > 1u && flash_ctl.flash_size >= FLASH_BANK_COUNT) {
            bank_size = flash_ctl.flash_size / FLASH_BANK_COUNT;
        } else if (bank_count != 0u) {
            bank_size = flash_ctl.flash_size / bank_count;
        }
        /* Unlike STM32U5 (RM0456 7.5.8) and STM32H5 (RM0481), on STM32L5
         * NSBKER/SECBKER is the "page number MSB" (RM0438): {BKER, PNB}
         * addresses the page in the currently mapped (logical) view, so it
         * is applied to the logical backing array directly, with no
         * inversion when SWAP_BANK is active. Do not "fix" this to match
         * the U5/H5 physical-bank behavior. */
        if (bank_size != 0u && (cr & FLASH_CR_BKER) != 0u) {
            bank_offset = bank_size;
            start += bank_offset;
        }
        if (start >= flash_ctl.flash_size) {
            return;
        }
        length = sector_size;
        if (start + length > flash_ctl.flash_size) {
            length = flash_ctl.flash_size - start;
        }
    } else {
        return;
    }
    if (flash_trace_enabled()) {
        const char *sec = (cr_off == FLASH_SECCR) ? "S" : "NS";
        const char *mode = (cr & (FLASH_CR_MER1 | FLASH_CR_MER2)) ? "MER" : "PER";
        mm_u32 pnb = (cr & FLASH_CR_PNB_MASK) >> FLASH_CR_PNB_SHIFT;
        printf("[FLASH_ERASE] %s mode=%s cr=0x%08lx pnb=%lu start=0x%08lx len=0x%08lx\n",
               sec,
               mode,
               (unsigned long)cr,
               (unsigned long)pnb,
               (unsigned long)start,
               (unsigned long)length);
    }
    flash_set_busy(sr_off, MM_TRUE);
    memset(flash_ctl.flash + start, 0xFF, length);
    flash_set_busy(sr_off, MM_FALSE);
    flash_set_eop(sr_off);
    if (flash_ctl.persist != 0 && flash_ctl.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)flash_ctl.persist, start, length);
    }
}

static mm_bool flash_write_cb(void *opaque, enum mm_sec_state sec, mm_u32 addr, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 base;
    mm_u32 offset;
    mm_u32 cr_off;
    mm_u32 sr_off;
    mm_u32 i;
    mm_bool erase_mode;
    mm_bool erase_value;
    (void)opaque;

    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0) {
        return MM_FALSE;
    }
    if (addr >= flash_ctl.base_s && addr < flash_ctl.base_s + flash_ctl.flash_size) {
        base = flash_ctl.base_s;
    } else if (addr >= flash_ctl.base_ns && addr < flash_ctl.base_ns + flash_ctl.flash_size) {
        base = flash_ctl.base_ns;
    } else {
        return MM_FALSE;
    }
    offset = addr - base;
    cr_off = (sec == MM_SECURE) ? FLASH_SECCR : FLASH_NSCR;
    sr_off = (sec == MM_SECURE) ? FLASH_SECSR : FLASH_NSSR;

    if (offset + size_bytes > flash_ctl.flash_size) {
        return MM_FALSE;
    }
    if (flash_trace_enabled()) {
        printf("[FLASH_WRITE] %s addr=0x%08lx size=%lu value=0x%08lx\n",
               (sec == MM_SECURE) ? "S" : "NS",
               (unsigned long)addr,
               (unsigned long)size_bytes,
               (unsigned long)value);
    }
    if (!flash_is_unlocked(cr_off)) {
        return MM_TRUE;
    }
    if ((flash_ctl.regs[cr_off / 4u] & FLASH_CR_PG) == 0u) {
        return MM_TRUE;
    }

    erase_mode = ((flash_ctl.regs[cr_off / 4u] & FLASH_CR_PER) != 0u) ? MM_TRUE : MM_FALSE;
    erase_value = MM_FALSE;
    if (erase_mode) {
        if (size_bytes == 4u) {
            erase_value = (value == 0xFFFFFFFFu) ? MM_TRUE : MM_FALSE;
        } else if (size_bytes == 2u) {
            erase_value = ((value & 0xFFFFu) == 0xFFFFu) ? MM_TRUE : MM_FALSE;
        } else if (size_bytes == 1u) {
            erase_value = ((value & 0xFFu) == 0xFFu) ? MM_TRUE : MM_FALSE;
        }
    }

    if ((flash_ctl.flags & MM_TARGET_FLAG_NVM_WRITEONCE) != 0u && !(erase_mode && erase_value)) {
        for (i = 0; i < size_bytes; ++i) {
            if (flash_ctl.flash[offset + i] != 0xFFu) {
                flash_ctl.regs[sr_off / 4u] |= FLASH_FLAG_PGSERR;
                return MM_TRUE;
            }
        }
    }

    if (size_bytes == 4u) {
        flash_ctl.flash[offset] &= (mm_u8)(value & 0xFFu);
        flash_ctl.flash[offset + 1u] &= (mm_u8)((value >> 8) & 0xFFu);
        flash_ctl.flash[offset + 2u] &= (mm_u8)((value >> 16) & 0xFFu);
        flash_ctl.flash[offset + 3u] &= (mm_u8)((value >> 24) & 0xFFu);
    } else if (size_bytes == 2u) {
        flash_ctl.flash[offset] &= (mm_u8)(value & 0xFFu);
        flash_ctl.flash[offset + 1u] &= (mm_u8)((value >> 8) & 0xFFu);
    } else if (size_bytes == 1u) {
        flash_ctl.flash[offset] &= (mm_u8)(value & 0xFFu);
    } else {
        return MM_FALSE;
    }
    flash_set_eop(sr_off);
    if (flash_ctl.persist != 0 && flash_ctl.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)flash_ctl.persist, offset, size_bytes);
    }
    return MM_TRUE;
}

/* Reset bits currently unused (could clear state if asserted). */

static mm_bool rcc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)r->regs + offset, size_bytes);
    return MM_TRUE;
}

static void rcc_update_ready(struct rcc_state *r)
{
    mm_u32 cr = r->regs[0]; /* offset 0x0 */
    mm_u32 crrcr = r->regs[RCC_CRRCR / 4u];
    /* Mirror RDY bits to match ON bits (immediate ready). */
    /* MSIRDY bit1 follows MSION bit0 */
    if ((cr & (1u << 0)) != 0u) cr |= (1u << 1); else cr &= ~(1u << 1);
    /* HSIRDY bit10 follows HSION bit8 */
    if ((cr & (1u << 8)) != 0u) cr |= (1u << 10); else cr &= ~(1u << 10);
    /* HSERDY bit17 follows HSEON bit16 */
    if ((cr & (1u << 16)) != 0u) cr |= (1u << 17); else cr &= ~(1u << 17);
    /* PLLRDY bit25 follows PLLON bit24 */
    if ((cr & (1u << 24)) != 0u) cr |= (1u << 25); else cr &= ~(1u << 25);
    r->regs[0] = cr;

    /* HSI48RDY bit1 follows HSI48ON bit0 (RCC_CRRCR) */
    if ((crrcr & (1u << 0)) != 0u) crrcr |= (1u << 1); else crrcr &= ~(1u << 1);
    r->regs[RCC_CRRCR / 4u] = crrcr;
}

static mm_u64 rcc_msi_hz(const struct rcc_state *r)
{
    static const mm_u64 ranges[] = {
        100000ull, 200000ull, 400000ull, 800000ull,
        1000000ull, 2000000ull, 4000000ull, 8000000ull,
        16000000ull, 24000000ull, 32000000ull, 48000000ull
    };
    mm_u32 cr = r->regs[RCC_CR / 4];
    mm_u32 idx = (cr >> 4) & 0x0fu;
    if (idx >= (mm_u32)(sizeof(ranges) / sizeof(ranges[0]))) {
        idx = 6u;
    }
    return ranges[idx];
}

static mm_u64 rcc_pll_p_clk(const struct rcc_state *r)
{
    mm_u32 pllcfgr = r->regs[RCC_PLLCFGR / 4];
    mm_u32 src = pllcfgr & 0x3u;
    mm_u64 fin = 0;
    mm_u32 divm = ((pllcfgr >> 4) & 0x0fu) + 1u;
    mm_u32 n = (pllcfgr >> 8) & 0x7fu;
    mm_u32 rdiv_sel = (pllcfgr >> 25) & 0x3u;
    mm_u32 rdiv = 2u;

    if ((pllcfgr & (1u << 24)) == 0u) {
        return 0;
    }

    if (src == 1u) fin = rcc_msi_hz(r); /* MSI */
    else if (src == 2u) fin = 16000000ull; /* HSI16 */
    else if (src == 3u) fin = 8000000ull; /* HSE */
    else fin = 0;

    switch (rdiv_sel) {
    case 0u: rdiv = 2u; break;
    case 1u: rdiv = 4u; break;
    case 2u: rdiv = 6u; break;
    case 3u: rdiv = 8u; break;
    default: rdiv = 2u; break;
    }

    if (fin == 0 || divm == 0u || n == 0u || rdiv == 0u) {
        return 0;
    }
    return (fin / (mm_u64)divm) * (mm_u64)n / (mm_u64)rdiv;
}

static void rcc_update_sysclk(struct rcc_state *r)
{
    mm_u32 cfgr = r->regs[RCC_CFGR / 4];
    mm_u32 sw = cfgr & 0x3u;
    mm_u32 hpre = (cfgr >> 4) & 0xfu;
    mm_u64 sys = 0;
    mm_u32 div = 1u;

    if (sw == 0u) sys = rcc_msi_hz(r); /* MSI */
    else if (sw == 1u) sys = 16000000ull; /* HSI16 */
    else if (sw == 2u) sys = 8000000ull; /* HSE */
    else if (sw == 3u) sys = rcc_pll_p_clk(r);

    if (hpre >= 8u) {
        switch (hpre) {
        case 8u: div = 2u; break;
        case 9u: div = 4u; break;
        case 10u: div = 8u; break;
        case 11u: div = 16u; break;
        case 12u: div = 64u; break;
        case 13u: div = 128u; break;
        case 14u: div = 256u; break;
        case 15u: div = 512u; break;
        default: div = 1u; break;
        }
    }
    if (sys == 0) {
        r->cpu_hz = 0;
    } else {
        r->cpu_hz = sys / (mm_u64)div;
        if (r->cpu_hz == 0) r->cpu_hz = 1;
    }
    r->regs[RCC_CFGR / 4] = (r->regs[RCC_CFGR / 4] & ~(0x3u << 2)) | (sw << 2);
}

static mm_bool rcc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    static int rcc_trace = -1;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    if (rcc_trace < 0) {
        const char *v = getenv("M33MU_RCC_TRACE");
        rcc_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    memcpy((mm_u8 *)r->regs + offset, &value, size_bytes);
    if (offset == RCC_CR) {
        rcc_update_ready(r);
    }
    if (offset == RCC_CRRCR) {
        rcc_update_ready(r);
        if (rcc_trace) {
            printf("[RCC_CRRCR] write=0x%08lx read=0x%08lx\n",
                   (unsigned long)value,
                   (unsigned long)r->regs[RCC_CRRCR / 4u]);
        }
    }
    if (offset == RCC_CFGR || offset == RCC_PLLCFGR || offset == RCC_CR) {
        rcc_update_sysclk(r);
    }
    return MM_TRUE;
}

static mm_bool pwr_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct pwr_state *p = (struct pwr_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)p->regs + offset, size_bytes);
    return MM_TRUE;
}

static void pwr_update_vos(struct pwr_state *p)
{
    mm_u32 sr2 = p->regs[PWR_SR2 / 4];
    sr2 &= ~(1u << 10); /* VOSF cleared -> ready */
    p->regs[PWR_SR2 / 4] = sr2;
}

static mm_bool pwr_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct pwr_state *p = (struct pwr_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)p->regs + offset, &value, size_bytes);
    if (offset == PWR_CR1) {
        pwr_update_vos(p);
    }
    return MM_TRUE;
}

static mm_bool simple_blk_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct simple_blk *b = (struct simple_blk *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GTZC_BLK_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)b->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool simple_blk_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct simple_blk *b = (struct simple_blk *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GTZC_BLK_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)b->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool mpcbb_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct mpcbb_state *b = (struct mpcbb_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > MPCBB_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)b->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool mpcbb_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct mpcbb_state *b = (struct mpcbb_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > MPCBB_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)b->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static void mpcbb_init_defaults(void)
{
    mm_u32 bank;
    for (bank = 0; bank < (mm_u32)(sizeof(mpcbb) / sizeof(mpcbb[0])); ++bank) {
        mm_u32 word;
        memset(&mpcbb[bank], 0, sizeof(mpcbb[bank]));
        for (word = 0; word < mpcbb_words[bank]; ++word) {
            mpcbb[bank].regs[(MPCBB_SECCFGR_OFFSET / 4u) + word] = 0xFFFFFFFFu;
        }
    }
}

static mm_bool dma_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct dma_state *d = (struct dma_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > sizeof(d->regs)) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)d->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool dma_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct dma_state *d = (struct dma_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > sizeof(d->regs)) return MM_FALSE;
    memcpy((mm_u8 *)d->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool flash_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct flash_state *f = (struct flash_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FLASH_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)f->regs + offset, size_bytes);
    if (flash_trace_enabled() && offset == FLASH_OPTR && size_bytes == 4) {
        printf("[FLASH_OPTR] read=0x%08lx\n", (unsigned long)*value_out);
    }
    return MM_TRUE;
}

static mm_bool flash_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct flash_state *f = (struct flash_state *)opaque;
    mm_u32 cr_off;
    mm_u32 sr_off;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FLASH_SIZE) return MM_FALSE;

    if (offset == FLASH_NSKEYR || offset == FLASH_SECKEYR) {
        flash_handle_key(offset, value);
        return MM_TRUE;
    }

    if (offset == FLASH_OPTR) {
        mm_u32 new_val = value;
        if (!f->dualbank_enabled) {
            new_val &= ~FLASH_OPTR_SWAP_BANK;
        }
        if (f->dualbank_enabled) {
            new_val |= FLASH_OPTR_DBANK;
        } else {
            new_val &= ~FLASH_OPTR_DBANK;
        }
        f->regs[FLASH_OPTR / 4u] = new_val;
        flash_set_swap_state(f, (new_val & FLASH_OPTR_SWAP_BANK) != 0u);
        return MM_TRUE;
    }

    if (offset == FLASH_NSCCR || offset == FLASH_SECCCR) {
        sr_off = (offset == FLASH_NSCCR) ? FLASH_NSSR : FLASH_SECSR;
        if ((value & (1u << 16)) != 0u) {
            flash_clear_eop(sr_off);
        }
        return MM_TRUE;
    }

    if (offset == FLASH_NSCR || offset == FLASH_SECCR) {
        cr_off = offset;
        sr_off = (offset == FLASH_NSCR) ? FLASH_NSSR : FLASH_SECSR;
        if (!flash_is_unlocked(cr_off) && (value & FLASH_CR_LOCK) == 0u) {
            return MM_TRUE;
        }
        if (flash_trace_enabled() && size_bytes == 4) {
            const char *sec = (offset == FLASH_SECCR) ? "S" : "NS";
            printf("[FLASH_CR] %s write=0x%08lx\n", sec, (unsigned long)value);
        }
        memcpy((mm_u8 *)f->regs + offset, &value, size_bytes);
        if ((value & FLASH_CR_LOCK) != 0u) {
            f->regs[cr_off / 4u] |= FLASH_CR_LOCK;
        }
        if ((value & FLASH_CR_STRT) != 0u) {
            flash_apply_erase(cr_off, sr_off);
            f->regs[cr_off / 4u] &= ~FLASH_CR_STRT;
        }
        return MM_TRUE;
    }

    memcpy((mm_u8 *)f->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool rng_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rng_state *r = ((struct rng_state *)((void **)opaque)[0]);
    mm_bool secure_alias = ((void **)opaque)[1] != 0;
    struct rcc_state *rcc = (struct rcc_state *)((void **)opaque)[2];
    struct simple_blk *tzsc = (struct simple_blk *)((void **)opaque)[3];

    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RNG_SIZE) return MM_FALSE;

    if (!secure_alias && rng_requires_secure(tzsc)) {
        *value_out = 0;
        return MM_TRUE;
    }
    if (!rng_clock_enabled(rcc)) {
        *value_out = 0;
        return MM_TRUE;
    }

    if (offset == RNG_SR_OFFSET) {
        if (!r->dr_valid) {
            rng_fill(r);
        }
        *value_out = r->regs[RNG_SR_OFFSET / 4];
        return MM_TRUE;
    }
    if (offset == RNG_DR_OFFSET) {
        if (!r->dr_valid) {
            rng_fill(r);
        }
        *value_out = r->dr;
        r->dr_valid = MM_FALSE;
        r->regs[RNG_SR_OFFSET / 4] &= ~1u;
        return MM_TRUE;
    }

    memcpy(value_out, (mm_u8 *)r->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool rng_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rng_state *r = ((struct rng_state *)((void **)opaque)[0]);
    mm_bool secure_alias = ((void **)opaque)[1] != 0;
    struct rcc_state *rcc = (struct rcc_state *)((void **)opaque)[2];
    struct simple_blk *tzsc = (struct simple_blk *)((void **)opaque)[3];

    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RNG_SIZE) return MM_FALSE;

    if (!secure_alias && rng_requires_secure(tzsc)) {
        return MM_TRUE;
    }
    if (!rng_clock_enabled(rcc)) {
        return MM_TRUE;
    }

    if (offset == RNG_SR_OFFSET || offset == RNG_DR_OFFSET) {
        return MM_TRUE;
    }

    memcpy((mm_u8 *)r->regs + offset, &value, size_bytes);
    if (offset == RNG_CR_OFFSET) {
        if ((value & (1u << 2)) == 0u) {
            r->dr_valid = MM_FALSE;
            r->regs[RNG_SR_OFFSET / 4] &= ~1u;
        }
        if ((value & (1u << 3)) != 0u) {
            if (g_rng_nvic != 0 && (r->regs[RNG_SR_OFFSET / 4] & 1u) != 0u) {
                mm_nvic_set_pending(g_rng_nvic, (mm_u32)RNG_IRQ, MM_TRUE);
            }
        }
    }
    return MM_TRUE;
}

static mm_bool wwdg_clock_enabled(void)
{
    return ((rcc.regs[0x58 / 4] >> 11) & 1u) != 0u;
}

static int exti_line_bank(int line)
{
    mm_u32 reg;
    mm_u32 shift;
    mm_u32 val;
    if (line < 0 || line > 15) return -1;
    reg = exti.regs[(EXTI_EXTICR1 + (mm_u32)(line / 4) * 4u) / 4u];
    shift = (mm_u32)(line % 4) * 8u;
    val = (reg >> shift) & 0xFFu;
    if (val > 8u) return -1;
    return (int)val;
}

static void exti_raise_irq(int line)
{
    if (g_exti_nvic != 0) {
        mm_nvic_set_pending(g_exti_nvic, (mm_u32)(11 + line), MM_TRUE);
    }
}

static void exti_set_pending_rise(int line)
{
    exti.regs[EXTI_RPR1 / 4u] |= (1u << line);
    if ((exti.regs[EXTI_IMR1 / 4u] & (1u << line)) != 0u) {
        exti_raise_irq(line);
    }
}

static void exti_set_pending_fall(int line)
{
    exti.regs[EXTI_FPR1 / 4u] |= (1u << line);
    if ((exti.regs[EXTI_IMR1 / 4u] & (1u << line)) != 0u) {
        exti_raise_irq(line);
    }
}

static void exti_gpio_update(int bank, mm_u32 old_level, mm_u32 new_level)
{
    int line;
    mm_u32 changed = old_level ^ new_level;
    if (changed == 0u) return;
    for (line = 0; line < 16; ++line) {
        mm_u32 mask = (1u << line);
        if ((changed & mask) == 0u) continue;
        if (exti_line_bank(line) != bank) continue;
        if ((new_level & mask) != 0u) {
            if ((exti.regs[EXTI_RTSR1 / 4u] & mask) != 0u) {
                exti_set_pending_rise(line);
            }
        } else {
            if ((exti.regs[EXTI_FTSR1 / 4u] & mask) != 0u) {
                exti_set_pending_fall(line);
            }
        }
    }
}

static mm_bool exti_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct exti_state *e = (struct exti_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > EXTI_SIZE) return MM_FALSE;
    if (offset == EXTI_SWIER1) {
        *value_out = 0;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)e->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool exti_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct exti_state *e = (struct exti_state *)opaque;
    int line;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > EXTI_SIZE) return MM_FALSE;
    if (offset == EXTI_SWIER1) {
        mm_u32 v = value & 0xFFFFu;
        for (line = 0; line < 16; ++line) {
            if ((v & (1u << line)) != 0u) {
                exti_set_pending_rise(line);
            }
        }
        return MM_TRUE;
    }
    if (offset == EXTI_RPR1) {
        e->regs[offset / 4u] &= ~value;
        return MM_TRUE;
    }
    if (offset == EXTI_FPR1) {
        e->regs[offset / 4u] &= ~value;
        return MM_TRUE;
    }
    memcpy((mm_u8 *)e->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool iwdg_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct iwdg_state *w = (struct iwdg_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > IWDG_SIZE) return MM_FALSE;
    if (offset == IWDG_KR) {
        *value_out = 0;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)w->regs + offset, size_bytes);
    return MM_TRUE;
}

static void iwdg_apply_key(struct iwdg_state *w, mm_u32 key)
{
    if (key == 0x5555u) {
        w->write_access = MM_TRUE;
    } else if (key == 0xAAAAu) {
        w->counter = w->regs[IWDG_RLR / 4u] & 0x0FFFu;
    } else if (key == 0xCCCCu) {
        w->running = MM_TRUE;
        w->write_access = MM_FALSE;
        w->counter = w->regs[IWDG_RLR / 4u] & 0x0FFFu;
    }
}

static mm_bool iwdg_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct iwdg_state *w = (struct iwdg_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > IWDG_SIZE) return MM_FALSE;
    if (offset == IWDG_KR) {
        iwdg_apply_key(w, value & 0xFFFFu);
        return MM_TRUE;
    }
    if (!w->write_access) {
        return MM_TRUE;
    }
    if (offset == IWDG_PR) {
        w->regs[IWDG_PR / 4u] = value & 0x7u;
        return MM_TRUE;
    }
    if (offset == IWDG_RLR) {
        w->regs[IWDG_RLR / 4u] = value & 0x0FFFu;
        return MM_TRUE;
    }
    if (offset == IWDG_WINR) {
        w->regs[IWDG_WINR / 4u] = value & 0x0FFFu;
        return MM_TRUE;
    }
    if (offset == IWDG_EWCR) {
        w->regs[IWDG_EWCR / 4u] = value;
        return MM_TRUE;
    }
    return MM_TRUE;
}

static mm_bool wwdg_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct wwdg_state *w = (struct wwdg_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > WWDG_SIZE) return MM_FALSE;
    if (!wwdg_clock_enabled()) {
        *value_out = 0;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)w->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool wwdg_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct wwdg_state *w = (struct wwdg_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > WWDG_SIZE) return MM_FALSE;
    if (!wwdg_clock_enabled()) {
        return MM_TRUE;
    }
    if (offset == WWDG_CR) {
        w->regs[WWDG_CR / 4u] = value & 0xFFu;
        w->counter = w->regs[WWDG_CR / 4u] & 0x7Fu;
        return MM_TRUE;
    }
    if (offset == WWDG_CFR) {
        w->regs[WWDG_CFR / 4u] = value;
        return MM_TRUE;
    }
    if (offset == WWDG_SR) {
        w->regs[WWDG_SR / 4u] &= ~value;
        return MM_TRUE;
    }
    memcpy((mm_u8 *)w->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

mm_u32 *mm_stm32l552_rcc_regs(void)
{
    return rcc.regs;
}

mm_u64 mm_stm32l552_cpu_hz(void)
{
    return rcc.cpu_hz;
}

void mm_stm32l552_exti_set_nvic(struct mm_nvic *nvic)
{
    g_exti_nvic = nvic;
    g_wdg_nvic = nvic;
}

void mm_stm32l552_watchdog_tick(mm_u64 cycles)
{
    static const mm_u32 iwdg_presc_div[8] = { 4u, 8u, 16u, 32u, 64u, 128u, 256u, 256u };
    mm_u64 cpu_hz = mm_stm32l552_cpu_hz();

    if (wwdg_clock_enabled() && wwdg.counter != 0u && (wwdg.regs[WWDG_CR / 4u] & 0x80u) != 0u) {
        mm_u32 wdgtb = (wwdg.regs[WWDG_CFR / 4u] >> 11) & 0x7u;
        mm_u64 step = 4096u * (mm_u64)(1u << wdgtb);
        wwdg.accum += cycles;
        while (wwdg.accum >= step) {
            wwdg.accum -= step;
            if (wwdg.counter > 0u) {
                wwdg.counter--;
                wwdg.regs[WWDG_CR / 4u] = (wwdg.regs[WWDG_CR / 4u] & ~0x7Fu) | (wwdg.counter & 0x7Fu);
                if (wwdg.counter == 0x40u && (wwdg.regs[WWDG_CFR / 4u] & (1u << 9)) != 0u) {
                    wwdg.regs[WWDG_SR / 4u] |= 1u;
                    if (g_wdg_nvic != 0) {
                        mm_nvic_set_pending(g_wdg_nvic, 0u, MM_TRUE);
                    }
                }
                if (wwdg.counter == 0x3Fu) {
                    mm_system_request_reset();
                    break;
                }
            }
        }
    }

    if (iwdg.running) {
        mm_u32 pr = iwdg.regs[IWDG_PR / 4u] & 0x7u;
        mm_u64 ticks_per_sec;
        mm_u64 cycles_per_tick;
        mm_u64 lsi = 32000u;
        mm_u32 div = iwdg_presc_div[pr];
        ticks_per_sec = lsi / (mm_u64)div;
        if (ticks_per_sec == 0) ticks_per_sec = 1;
        if (cpu_hz == 0) cpu_hz = 1;
        cycles_per_tick = cpu_hz / ticks_per_sec;
        if (cycles_per_tick == 0) cycles_per_tick = 1;
        iwdg.accum += cycles;
        while (iwdg.accum >= cycles_per_tick) {
            iwdg.accum -= cycles_per_tick;
            if (iwdg.counter > 0u) {
                iwdg.counter--;
            }
            if (iwdg.counter == 0u) {
                mm_system_request_reset();
                break;
            }
        }
    }
}

mm_bool mm_stm32l552_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;

    memset(&rcc, 0, sizeof(rcc));
    memset(&pwr, 0, sizeof(pwr));
    memset(&tzsc_s, 0, sizeof(tzsc_s));
    memset(&tzsc_ns, 0, sizeof(tzsc_ns));
    memset(&tzic_s, 0, sizeof(tzic_s));
    memset(&tzic_ns, 0, sizeof(tzic_ns));
    memset(&rng, 0, sizeof(rng));
    memset(&hash_accel, 0, sizeof(hash_accel));
    memset(&aes_accel, 0, sizeof(aes_accel));
    memset(&exti, 0, sizeof(exti));
    memset(&iwdg, 0, sizeof(iwdg));
    memset(&wwdg, 0, sizeof(wwdg));
    memset(&flash_ctl, 0, sizeof(flash_ctl));
    memset(gpio, 0, sizeof(gpio));
    mpcbb_init_defaults();
    memset(&dma1, 0, sizeof(dma1));
    memset(&dma2, 0, sizeof(dma2));
    rcc.regs[RCC_CR / 4] = 0x00000063u;
    rcc_update_ready(&rcc);
    rcc_update_sysclk(&rcc);
    hash_ctx[0].state = &hash_accel;
    hash_ctx[0].secure_alias = MM_FALSE;
    hash_ctx[0].rcc = &rcc;
    hash_ctx[0].tzsc = &tzsc_s;
    hash_ctx[0].clock_enabled = hash_clock_enabled;
    hash_ctx[0].requires_secure = hash_requires_secure;
    hash_ctx[1].state = &hash_accel;
    hash_ctx[1].secure_alias = MM_TRUE;
    hash_ctx[1].rcc = &rcc;
    hash_ctx[1].tzsc = &tzsc_s;
    hash_ctx[1].clock_enabled = hash_clock_enabled;
    hash_ctx[1].requires_secure = hash_requires_secure;
    aes_ctx[0].state = &aes_accel;
    aes_ctx[0].secure_alias = MM_FALSE;
    aes_ctx[0].rcc = &rcc;
    aes_ctx[0].tzsc = &tzsc_s;
    aes_ctx[0].is_saes = MM_FALSE;
    aes_ctx[0].clock_enabled = aes_clock_enabled;
    aes_ctx[0].requires_secure = aes_requires_secure;
    aes_ctx[1].state = &aes_accel;
    aes_ctx[1].secure_alias = MM_TRUE;
    aes_ctx[1].rcc = &rcc;
    aes_ctx[1].tzsc = &tzsc_s;
    aes_ctx[1].is_saes = MM_FALSE;
    aes_ctx[1].clock_enabled = aes_clock_enabled;
    aes_ctx[1].requires_secure = aes_requires_secure;
    pka_ctx[0].state = &pka_accel;
    pka_ctx[0].secure_alias = MM_FALSE;
    pka_ctx[0].rcc = &rcc;
    pka_ctx[0].tzsc = &tzsc_s;
    pka_ctx[0].clock_enabled = pka_clock_enabled;
    pka_ctx[0].requires_secure = pka_requires_secure;
    pka_ctx[1].state = &pka_accel;
    pka_ctx[1].secure_alias = MM_TRUE;
    pka_ctx[1].rcc = &rcc;
    pka_ctx[1].tzsc = &tzsc_s;
    pka_ctx[1].clock_enabled = pka_clock_enabled;
    pka_ctx[1].requires_secure = pka_requires_secure;
    rng.regs[RNG_CR_OFFSET / 4] = 0x00871f00u;
    rng.regs[RNG_HTCR_OFFSET / 4] = 0x000072acu;
    flash_ctl.regs[FLASH_ACR / 4] = 0x00000013u;
    flash_ctl.regs[FLASH_NSCR / 4] = 0x00000001u;
    flash_ctl.regs[FLASH_SECCR / 4] = 0x00000001u;
    iwdg.regs[IWDG_RLR / 4] = 0x00000FFFu;
    iwdg.regs[IWDG_WINR / 4] = 0x00000FFFu;
    wwdg.regs[WWDG_CR / 4] = 0x0000007Fu;
    wwdg.regs[WWDG_CFR / 4] = 0x0000007Fu;
    wwdg.counter = 0x7Fu;
    exti.regs[EXTI_IMR1 / 4u] = 0xFFFE0000u;

    /* RCC */
    reg.base = RCC_BASE;
    reg.size = RCC_SIZE;
    reg.opaque = &rcc;
    reg.read = rcc_read;
    reg.write = rcc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* RCC secure alias */
    reg.base = RCC_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* PWR */
    reg.base = PWR_BASE;
    reg.size = PWR_SIZE;
    reg.opaque = &pwr;
    reg.read = pwr_read;
    reg.write = pwr_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* PWR secure alias */
    reg.base = PWR_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* UCPD1 */
    reg.base = UCPD1_BASE;
    reg.size = UCPD1_SIZE;
    reg.opaque = &ucpd1_state;
    reg.read = ucpd_read;
    reg.write = ucpd_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* UCPD1 secure alias */
    reg.base = UCPD1_SEC_BASE;
    reg.opaque = &ucpd1_state_sec;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* CRS */
    reg.base = CRS_BASE;
    reg.size = CRS_SIZE;
    reg.opaque = &crs;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* CRS secure alias */
    reg.base = CRS_SEC_BASE;
    reg.opaque = &crs_sec;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* TAMP */
    reg.base = TAMP_BASE;
    reg.size = TAMP_SIZE;
    reg.opaque = &tamp;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* TAMP secure alias */
    reg.base = TAMP_SEC_BASE;
    reg.opaque = &tamp_sec;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* SYSCFG */
    reg.base = SYSCFG_BASE;
    reg.size = SYSCFG_SIZE;
    reg.opaque = &syscfg;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* SYSCFG secure alias */
    reg.base = SYSCFG_SEC_BASE;
    reg.opaque = &syscfg_sec;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* DBGMCU (private peripheral bus, no secure alias) */
    reg.base = DBGMCU_BASE;
    reg.size = DBGMCU_SIZE;
    reg.opaque = &dbgmcu;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* ICACHE */
    reg.base = ICACHE_BASE;
    reg.size = ICACHE_SIZE;
    reg.opaque = &icache;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* ICACHE secure alias */
    reg.base = ICACHE_SEC_BASE;
    reg.opaque = &icache_sec;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* FLASH controller */
    reg.base = FLASH_BASE;
    reg.size = FLASH_SIZE;
    reg.opaque = &flash_ctl;
    reg.read = flash_read;
    reg.write = flash_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* FLASH secure alias */
    reg.base = FLASH_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GTZC TZSC secure */
    reg.base = GTZC_TZSC_S_BASE;
    reg.size = GTZC_TZSC_SIZE;
    reg.opaque = &tzsc_s;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GTZC TZSC non-secure alias */
    reg.base = GTZC_TZSC_NS_BASE;
    reg.opaque = &tzsc_ns;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GTZC TZIC secure */
    reg.base = GTZC_TZIC_S_BASE;
    reg.size = GTZC_TZIC_SIZE;
    reg.opaque = &tzic_s;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GTZC TZIC non-secure alias */
    reg.base = GTZC_TZIC_NS_BASE;
    reg.opaque = &tzic_ns;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* MPCBB1..2 (non-secure and secure aliases) */
    reg.size = MPCBB_SIZE;
    reg.read = mpcbb_read;
    reg.write = mpcbb_write;
    reg.base = MPCBB1_BASE;
    reg.opaque = &mpcbb[0];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = MPCBB1_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = MPCBB2_BASE;
    reg.opaque = &mpcbb[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = MPCBB2_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* AES (non-secure and secure aliases) */
    reg.base = AES_BASE;
    reg.size = AES_SIZE;
    reg.opaque = &aes_ctx[0];
    reg.read = aes_read;
    reg.write = aes_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = AES_SEC_BASE;
    reg.opaque = &aes_ctx[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* HASH (non-secure and secure aliases) */
    reg.base = HASH_BASE;
    reg.size = HASH_SIZE;
    reg.opaque = &hash_ctx[0];
    reg.read = hash_read;
    reg.write = hash_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = HASH_SEC_BASE;
    reg.opaque = &hash_ctx[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* PKA (non-secure and secure aliases) */
    reg.base = PKA_BASE;
    reg.size = PKA_SIZE;
    reg.opaque = &pka_ctx[0];
    reg.read = pka_read;
    reg.write = pka_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PKA_SEC_BASE;
    reg.opaque = &pka_ctx[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;


    /* RNG (non-secure and secure aliases) */
    reg.base = RNG_BASE;
    reg.size = RNG_SIZE;
    rng_ctx[0][0] = &rng;
    rng_ctx[0][1] = (void *)0;
    rng_ctx[0][2] = &rcc;
    rng_ctx[0][3] = &tzsc_s;
    reg.opaque = rng_ctx[0];
    reg.read = rng_read;
    reg.write = rng_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = RNG_SEC_BASE;
    rng_ctx[1][0] = &rng;
    rng_ctx[1][1] = (void *)1;
    rng_ctx[1][2] = &rcc;
    rng_ctx[1][3] = &tzsc_s;
    reg.opaque = rng_ctx[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* EXTI (non-secure and secure aliases) */
    reg.base = EXTI_BASE;
    reg.size = EXTI_SIZE;
    reg.opaque = &exti;
    reg.read = exti_read;
    reg.write = exti_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = EXTI_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* IWDG (non-secure and secure aliases) */
    reg.base = IWDG_BASE;
    reg.size = IWDG_SIZE;
    reg.opaque = &iwdg;
    reg.read = iwdg_read;
    reg.write = iwdg_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = IWDG_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* WWDG (non-secure and secure aliases) */
    reg.base = WWDG_BASE;
    reg.size = WWDG_SIZE;
    reg.opaque = &wwdg;
    reg.read = wwdg_read;
    reg.write = wwdg_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = WWDG_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* DMA1/DMA2 (non-secure and secure aliases) */
    reg.size = 0x400u;
    reg.read = dma_read;
    reg.write = dma_write;
    reg.base = 0x40020000u; /* DMA1 */
    reg.opaque = &dma1;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = 0x50020000u; /* SEC_DMA1 */
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = 0x40020400u; /* DMA2 */
    reg.opaque = &dma2;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = 0x50020400u; /* SEC_DMA2 */
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GPIO A..H: NS alias 0x4202xxxx, Secure alias 0x5202xxxx - using shared implementation */
    {
        mm_u32 base_ns = 0x42020000u;
        mm_u32 base_s  = 0x52020000u;
        int i;
        for (i = 0; i < 8; ++i) {
            /* NS */
            reg.base = base_ns + (mm_u32)(i * 0x400u);
            reg.size = STM32_GPIO_REG_SIZE;
            reg.opaque = &gpio_ctx_data[i * 2];
            reg.read = stm32_gpio_read;
            reg.write = stm32_gpio_write;
            if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

            /* Secure */
            reg.base = base_s + (mm_u32)(i * 0x400u);
            reg.opaque = &gpio_ctx_data[i * 2 + 1];
            if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
        }
    }

    if (!mm_stm32l552_usb_register_mmio(bus)) return MM_FALSE;

    return MM_TRUE;
}

void mm_stm32l552_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags)
{
    if (map == 0) {
        return;
    }
    flash_ctl.flash = flash;
    flash_ctl.flash_size = flash_size;
    flash_ctl.persist = persist;
    flash_ctl.flags = flags;
    flash_ctl.base_s = map->flash_base_s;
    flash_ctl.base_ns = map->flash_base_ns;
    flash_ctl.dualbank_enabled = (flags & MM_TARGET_FLAG_DUALBANK) != 0u;
    flash_sync_option_regs(&flash_ctl);
    mm_memmap_set_flash_writer(map, flash_write_cb, &flash_ctl);
}

void mm_stm32l552_rng_set_nvic(struct mm_nvic *nvic)
{
    g_rng_nvic = nvic;
}
