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
#include "m33mu/otfdec.h"
#include "m33mu/spiflash.h"
#ifdef M33MU_HAS_WOLFSSL
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/sha512.h>
#endif
#include "stm32h5_mmio.h"
#include "stm32h5_eth.h"
#include "stm32h5_i2c.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/otp.h"
#include "m33mu/spi_bus.h"
#include "m33mu/gpio.h"
#include "m33mu/nvic.h"
#include "stm32_crypto.h"
#include "stm32_gpio.h"
#include "stm32_gpdma.h"

extern void mm_system_request_reset(void);

/* RCC base addresses (system domain) */
#define RCC_BASE     0x44020c00u
#define RCC_SEC_BASE 0x54020c00u
#define RCC_SIZE     0x400u

#define RCC_CR       0x000u
#define RCC_CFGR1    0x01cu
#define RCC_CFGR2    0x020u
#define RCC_PLL1CFGR 0x028u
#define RCC_PLL1DIVR 0x034u
#define RCC_PLL2CFGR 0x02cu
#define RCC_PLL3CFGR 0x030u
#define RCC_PLL2DIVR 0x03cu
#define RCC_PLL2FRACR 0x040u
#define RCC_PLL3DIVR 0x044u
#define RCC_PLL3FRACR 0x048u
#define RCC_BDCR     0x0f0u

/* PWR base (system domain) */
#define PWR_BASE     0x44020800u
#define PWR_SEC_BASE 0x54020800u
#define PWR_SIZE     0x400u
/* TAMP base */
#define TAMP_BASE     0x44007C00u
#define TAMP_SEC_BASE 0x54007C00u
#define TAMP_SIZE     0x400u
/* SBS base (system domain) */
#define SBS_BASE     0x44000400u
#define SBS_SEC_BASE 0x54000400u
#define SBS_SIZE     0x400u
/* BSEC base (system domain) */
#define BSEC_BASE    0x46009000u
#define BSEC_SIZE    0x400u

/* FLASH controller base */
#define FLASH_BASE   0x40022000u
#define FLASH_SEC_BASE 0x50022000u
#define FLASH_SIZE   0x400u

#define FLASH_OTP_BASE_NS 0x08FFF000u
#define FLASH_OTP_BASE_S  0x0CFFF000u
#define FLASH_OTP_SIZE    0x800u

/* 96-bit factory unique device ID (RM0517 sec. 72.1), just past the OTP window. */
#define UID_BASE_NS 0x08FFF800u
#define UID_BASE_S  0x0CFFF800u
#define UID_SIZE    0x10u
#define FLASH_OTP_BLOCK_SIZE 64u
#define FLASH_OTP_BLOCK_COUNT 32u

/* GTZC TZSC/TZIC (secure / non-secure aliases) */
#define GTZC_TZSC_S_BASE 0x50032400u
#define GTZC_TZSC_NS_BASE 0x40032400u
#define GTZC_TZIC_S_BASE 0x50032800u
#define GTZC_TZIC_NS_BASE 0x40032800u
#define GTZC_TZSC_SIZE 0x400u
#define GTZC_TZIC_SIZE 0x400u
#define GTZC_BLK_SIZE 0x1000u

/* ICACHE/DCACHE base addresses (secure / non-secure aliases) */
#define ICACHE_BASE     0x40030400u
#define ICACHE_SEC_BASE 0x50030400u
#define DCACHE_BASE     0x40031400u
#define DCACHE_SEC_BASE 0x50031400u
#define CACHE_SIZE      0x400u

/* RNG base addresses */
#define RNG_BASE     0x420c0800u
#define RNG_SEC_BASE 0x520c0800u
#define RNG_SIZE     0x400u

/* AES/HASH/SAES/PKA base addresses */
#define AES_BASE     0x420c0000u
#define AES_SEC_BASE 0x520c0000u
#define AES_SIZE     0x400u
#define HASH_BASE     0x420c0400u
#define HASH_SEC_BASE 0x520c0400u
#define HASH_SIZE     0x400u
#define SAES_BASE     0x420c0c00u
#define SAES_SEC_BASE 0x520c0c00u
#define SAES_SIZE     0x400u
#define PKA_BASE     0x420c2000u
#define PKA_SEC_BASE 0x520c2000u
#define PKA_SIZE     0x2000u

/* MPCBB base addresses */
#define MPCBB1_BASE     0x40032c00u
#define MPCBB1_SEC_BASE 0x50032c00u
#define MPCBB_SIZE      0x400u

/* EXTI base addresses */
#define EXTI_BASE     0x44022000u
#define EXTI_SEC_BASE 0x54022000u
#define EXTI_SIZE     0x400u

/* DBGMCU base address */
#define DBGMCU_BASE   0x44024000u
#define DBGMCU_SIZE   0x1000u

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
#define RNG_HTCR_OFFSET  0x10u
#define RNG_HTCR1_OFFSET 0x14u
#define RNG_HTCR2_OFFSET 0x18u
#define RNG_HTCR3_OFFSET 0x1cu

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

#define TZSC_SECCFGR3_OFFSET 0x18u
#define TZSC_AESSEC_BIT 16u
#define TZSC_HASHSEC_BIT 17u
#define TZSC_RNGSEC_BIT 18u
#define TZSC_SAESSEC_BIT 19u
#define TZSC_PKASEC_BIT 20u

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

/* GPIO register offsets for local use */
#define GPIO_IDR_OFFSET STM32_GPIO_IDR_OFFSET
#define GPIO_ODR_OFFSET STM32_GPIO_ODR_OFFSET
#define GPIO_MODER_OFFSET STM32_GPIO_MODER_OFFSET

/* MPCBB offsets */
#define MPCBB_CR_OFFSET      0x000u
#define MPCBB_SECCFGR_OFFSET 0x100u
#define MPCBB_CR_INVSECSTATE (1u << 30)

/* FLASH register offsets */
#define FLASH_ACR      0x000u
#define FLASH_NSKEYR   0x004u
#define FLASH_SECKEYR  0x008u
#define FLASH_NSSR     0x020u
#define FLASH_SECSR    0x024u
#define FLASH_NSCR     0x028u
#define FLASH_SECCR    0x02cu
#define FLASH_NSCCR    0x030u
#define FLASH_SECCCR   0x034u
#define FLASH_OPTSR_CUR 0x050u
#define FLASH_OPTSR_PRG 0x054u
#define FLASH_OTPBLR_CUR 0x090u
#define FLASH_OTPBLR_PRG 0x094u

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu

#define FLASH_FLAG_BSY (1u << 0)
#define FLASH_FLAG_EOP (1u << 16)
#define FLASH_FLAG_PGSERR (1u << 18)

#define FLASH_CR_LOCK (1u << 0)
#define FLASH_CR_PG   (1u << 1)
#define FLASH_CR_SER  (1u << 2)
#define FLASH_CR_BER  (1u << 3)
#define FLASH_CR_STRT (1u << 5)
#define FLASH_CR_SNB_SHIFT 6
#define FLASH_CR_SNB_MASK (V->flash_snb_mask << FLASH_CR_SNB_SHIFT)

#define FLASH_SECTOR_COUNT (V->flash_sector_count)
#define FLASH_BANK_COUNT   2u
#define FLASH_CR_BKSEL     (1u << 31)
#define FLASH_OPTSR_SWAP_BANK (1u << 31)

/* TrustZone flash security attribution (RM0517).
 * SECWMx: per-bank secure watermark; sectors [STRT, END] are secure.
 * SECBBxRy: per-bank block-based attribution; each bit marks one sector
 * secure, on top of the watermark (used by wolfBoot's temporary claim
 * of non-secure areas during flash operations). Bank indices are
 * physical, whatever the SWAP_BANK setting. */
#define FLASH_SECBB1R      0x0A0u /* 8 registers, 0xA0..0xBC */
#define FLASH_SECBB2R      0x1A0u /* 8 registers, 0x1A0..0x1BC */
#define FLASH_SECBB_NREGS  (V->flash_secbb_nregs)
#define FLASH_SECWM1R_CUR  0x0E0u
#define FLASH_SECWM1R_PRG  0x0E4u
#define FLASH_SECWM2R_CUR  0x1E0u
#define FLASH_SECWM2R_PRG  0x1E4u
#define FLASH_SECWM_STRT_MASK  (V->flash_secwm_strt_mask)
#define FLASH_SECWM_END_SHIFT  16
#define FLASH_SECWM_EMPTY  (V->flash_secwm_strt_mask) /* STRT=all ones, END=0: no secure sector */

/* Emulator fiction: real STM32H5 silicon has no UID in BSEC, it lives at UID_BASE_NS. */
#define BSEC_UID0_OFFSET 0x014u
#define BSEC_UID1_OFFSET 0x018u
#define BSEC_UID2_OFFSET 0x01Cu

#define BSEC_UID0_VALUE 0xB5EC0001u
#define BSEC_UID1_VALUE 0xB5EC0002u
#define BSEC_UID2_VALUE 0xB5EC0003u

#define UID0_OFFSET 0x000u
#define UID1_OFFSET 0x004u
#define UID2_OFFSET 0x008u

/* Deliberately unlike BSEC_UID*_VALUE so a regression to the BSEC read is detectable. */
#define UID0_VALUE 0xF1D00001u
#define UID1_VALUE 0xF1D00002u
#define UID2_VALUE 0xF1D00003u

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

struct ucpd_state {
    mm_u32 cfgr1;
    mm_u32 cr;
    mm_u32 imr;
    mm_u32 sr;
};

#include "stm32_crypto_priv.h"

static void rcc_update_ready(struct rcc_state *r);
static void rcc_update_sysclk(struct rcc_state *r);
static void pwr_update_vos(struct pwr_state *p);
static void mpcbb_init_defaults(void);
static void flash_sync_option_regs(struct flash_state *f);

static struct rcc_state rcc;
static struct rcc_state rcc_s;
static struct pwr_state pwr;
static struct simple_blk tzsc_s;
static struct simple_blk tzsc_ns;
static struct simple_blk tzic_s;
static struct simple_blk tzic_ns;
static struct simple_blk sbs;
static struct simple_blk sbs_sec;
static struct simple_blk tamp;
static struct simple_blk bsec;
static struct simple_blk uid;
static struct simple_blk ucpd1;
static struct simple_blk ucpd1_sec;
static struct simple_blk crs;
static struct simple_blk crs_sec;
static struct simple_blk dbgmcu;
static struct simple_blk icache;
static struct simple_blk dcache;
static struct ucpd_state ucpd1_state;
static struct ucpd_state ucpd1_state_sec;
static struct mpcbb_state mpcbb[STM32H5_MPCBB_MAX];
static struct rng_state rng;
static struct hash_state hash_accel;
static struct aes_state aes_accel;
static struct aes_state saes_accel;
static struct pka_state pka_accel;
static struct mm_otfdec *otfdec_state = 0;
static struct exti_state exti;
static struct iwdg_state iwdg;
static struct wwdg_state wwdg;
static struct flash_state flash_ctl;
static void flash_init_secwm_defaults(struct flash_state *f);
static mm_bool flash_sector_attr_secure(struct flash_state *f, mm_u32 offset);
static struct mm_otp otp_state;
static struct stm32_gpio_state gpio[STM32H5_GPIO_MAX]; /* A..K */
static struct stm32_gpdma_state gpdma1;
static struct stm32_gpdma_state gpdma2;
static struct stm32_gpio_ctx gpio_ctx_data[STM32H5_GPIO_MAX * 2]; /* NS + S for each bank */
static void *rng_ctx[2][4];
static struct hash_ctx hash_ctx[2];
static struct aes_ctx aes_ctx[2];
static struct aes_ctx saes_ctx[2];
static struct pka_ctx pka_ctx[2];
static struct mm_nvic *g_rng_nvic = 0;
static struct mm_nvic *g_exti_nvic = 0;
static struct mm_nvic *g_wdg_nvic = 0;

#define RNG_IRQ 114

/* Set by whichever entry point the target reaches first; see
 * stm32h5_mmio.h. Every part-dependent value is read through it. */
static const struct stm32h5_mmio_variant *V;

static void stm32h5_mmio_reset_impl(void);
static mm_bool stm32h5_register_mmio_impl(struct mmio_bus *bus);
static void stm32h5_flash_bind_impl(struct mm_memmap *map, mm_u8 *flash, mm_u32 flash_size,
                                    const struct mm_flash_persist *persist, mm_u32 flags);
static void stm32h5_otp_init_impl(const char *target_name);

#define mpcbb_words (V->mpcbb_words)

/* GPDMA identity and channel wiring. Applied from both the reset path and
 * the registration path: registration runs after mm_target_soc_reset(),
 * so anything it clears has to be re-established here. */
static void gpdma_configure_and_reset(void)
{
    gpdma1.instance = 0;
    gpdma1.num_channels = V->gpdma_channels;
    gpdma1.irq_base = 27; /* GPDMA1 channel IRQs start at 27 */
    gpdma1.irq_map = V->gpdma1_irq_map;
    stm32_gpdma_reset(&gpdma1);
    gpdma2.instance = 1;
    gpdma2.num_channels = V->gpdma_channels;
    gpdma2.irq_base = 90; /* GPDMA2 channel IRQs start at 90 */
    gpdma2.irq_map = V->gpdma2_irq_map;
    stm32_gpdma_reset(&gpdma2);
}

static mm_u32 stm32h5_gpio_bank_read(void *opaque, int bank);
static mm_u32 stm32h5_gpio_bank_read_moder(void *opaque, int bank);
static mm_bool stm32h5_gpio_bank_clock(void *opaque, int bank);
static mm_u32 stm32h5_gpio_bank_read_seccfgr(void *opaque, int bank);
static void exti_gpio_update(int bank, mm_u32 old_level, mm_u32 new_level);
static mm_bool stm32h5_gpio_clock_enabled_cb(int bank);
static void exti_gpio_update_cb(int bank, mm_u32 old_level, mm_u32 new_level);
static mm_bool hash_requires_secure(const struct simple_blk *tzsc);
static mm_bool aes_requires_secure(const struct simple_blk *tzsc, mm_bool is_saes);
static mm_bool pka_requires_secure(const struct simple_blk *tzsc);

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

static mm_bool hash_clock_enabled(const struct rcc_state *rcc)
{
    return ((rcc->regs[0x8c / 4u] >> 17) & 1u) != 0u;
}

static mm_bool aes_clock_enabled(const struct rcc_state *rcc, mm_bool is_saes)
{
    mm_u32 bit = is_saes ? 20u : 16u;
    return ((rcc->regs[0x8c / 4u] >> bit) & 1u) != 0u;
}

static mm_bool pka_clock_enabled(const struct rcc_state *rcc)
{
    return ((rcc->regs[0x8c / 4u] >> 19) & 1u) != 0u;
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

static mm_bool stm32h5_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len);

static mm_bool stm32h5_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= V->gpio_count) {
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

static void stm32h5_mmio_reset_impl(void)
{
    size_t i;
    mm_u32 optsr_swap = flash_ctl.regs[FLASH_OPTSR_CUR / 4u] & FLASH_OPTSR_SWAP_BANK;
    mm_bool dualbank = flash_ctl.dualbank_enabled;
    memset(&rcc, 0, sizeof(rcc));
    memset(&rcc_s, 0, sizeof(rcc_s));
    memset(&pwr, 0, sizeof(pwr));
    memset(&tzsc_s, 0, sizeof(tzsc_s));
    memset(&tzsc_ns, 0, sizeof(tzsc_ns));
    memset(&tzic_s, 0, sizeof(tzic_s));
    memset(&tzic_ns, 0, sizeof(tzic_ns));
    memset(&sbs, 0, sizeof(sbs));
    memset(&sbs_sec, 0, sizeof(sbs_sec));
    memset(&tamp, 0, sizeof(tamp));
    memset(&bsec, 0, sizeof(bsec));
    memset(&ucpd1, 0, sizeof(ucpd1));
    memset(&ucpd1_sec, 0, sizeof(ucpd1_sec));
    memset(&ucpd1_state, 0, sizeof(ucpd1_state));
    memset(&ucpd1_state_sec, 0, sizeof(ucpd1_state_sec));
    memset(&crs, 0, sizeof(crs));
    memset(&crs_sec, 0, sizeof(crs_sec));
    memset(&dbgmcu, 0, sizeof(dbgmcu));
    memset(&icache, 0, sizeof(icache));
    memset(&dcache, 0, sizeof(dcache));
    memset(&rng, 0, sizeof(rng));
    memset(&hash_accel, 0, sizeof(hash_accel));
    memset(&aes_accel, 0, sizeof(aes_accel));
    memset(&saes_accel, 0, sizeof(saes_accel));
    memset(&pka_accel, 0, sizeof(pka_accel));
    memset(&exti, 0, sizeof(exti));
    memset(&iwdg, 0, sizeof(iwdg));
    memset(&wwdg, 0, sizeof(wwdg));
    memset(&flash_ctl, 0, sizeof(flash_ctl));
    flash_ctl.dualbank_enabled = dualbank;
    flash_ctl.swap_active = (optsr_swap != 0u) ? MM_TRUE : MM_FALSE;
    flash_init_secwm_defaults(&flash_ctl);
    mpcbb_init_defaults();
    /* Initialize GPDMA with shared implementation */
    gpdma_configure_and_reset();
    /* Initialize GPIO with shared implementation */
    for (i = 0; i < sizeof(gpio) / sizeof(gpio[0]); ++i) {
        stm32_gpio_reset(&gpio[i], (int)i);
        gpio_ctx_data[i * 2].gpio = &gpio[i];
        gpio_ctx_data[i * 2].is_secure_alias = MM_FALSE;
        gpio_ctx_data[i * 2].bank_index = (int)i;
        gpio_ctx_data[i * 2].clock_enabled = stm32h5_gpio_clock_enabled_cb;
        gpio_ctx_data[i * 2].exti_update = exti_gpio_update_cb;
        gpio_ctx_data[i * 2 + 1].gpio = &gpio[i];
        gpio_ctx_data[i * 2 + 1].is_secure_alias = MM_TRUE;
        gpio_ctx_data[i * 2 + 1].bank_index = (int)i;
        gpio_ctx_data[i * 2 + 1].clock_enabled = stm32h5_gpio_clock_enabled_cb;
        gpio_ctx_data[i * 2 + 1].exti_update = exti_gpio_update_cb;
    }
    if (V->usb_reset != 0) {
        V->usb_reset();
    }
    stm32h5_i2c_reset();
    mm_gpio_bank_set_reader(stm32h5_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(stm32h5_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(stm32h5_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(stm32h5_gpio_bank_read_seccfgr, 0);
    mm_gpio_set_bank_info_reader(stm32h5_gpio_bank_info, 0);
    mm_rcc_set_clock_list_reader(stm32h5_rcc_clock_list_line, 0);
    /* Enable HSI by default. Model the H5 reset divider as HSIDIV=/2. */
    rcc.regs[RCC_CR / 4u] |= 1u | (1u << 3);
    /* RCC_AHB2ENR reset value (see STM32H5F4 SVD): SRAM2..5 clocks on. */
    rcc.regs[0x8c / 4u] = V->ahb2enr_reset;
    rcc_update_ready(&rcc);
    rcc_update_sysclk(&rcc);
    rcc_s = rcc;
    iwdg.regs[IWDG_RLR / 4u] = 0x00000FFFu;
    iwdg.regs[IWDG_WINR / 4u] = 0x00000FFFu;
    wwdg.regs[WWDG_CR / 4u] = 0x0000007Fu;
    wwdg.regs[WWDG_CFR / 4u] = 0x0000007Fu;
    wwdg.counter = 0x7Fu;
    exti.regs[EXTI_IMR1 / 4u] = 0xFFFE0000u;
    bsec.regs[BSEC_UID0_OFFSET / 4u] = BSEC_UID0_VALUE;
    bsec.regs[BSEC_UID1_OFFSET / 4u] = BSEC_UID1_VALUE;
    bsec.regs[BSEC_UID2_OFFSET / 4u] = BSEC_UID2_VALUE;
    uid.regs[UID0_OFFSET / 4u] = UID0_VALUE;
    uid.regs[UID1_OFFSET / 4u] = UID1_VALUE;
    uid.regs[UID2_OFFSET / 4u] = UID2_VALUE;
    /* Power ready flags. */
    pwr_update_vos(&pwr);

    /* RNG reset values */
    rng.regs[RNG_CR_OFFSET / 4] = V->rng_cr_reset;
    rng.regs[RNG_HTCR_OFFSET / 4] = 0x000072acu;
    if (V->rng_has_htcr123) {
        rng.regs[RNG_HTCR1_OFFSET / 4] = 0x0000ffffu;
        rng.regs[RNG_HTCR2_OFFSET / 4] = 0x0000ffffu;
        rng.regs[RNG_HTCR3_OFFSET / 4] = 0x0000ffffu;
    }
    hash_reset_state(&hash_accel, MM_TRUE);
    mm_pka_reset(&pka_accel);

    /* OTFDEC reset */
    if (otfdec_state == 0) {
        otfdec_state = mm_otfdec_new();
    }
    if (otfdec_state != 0) {
        mm_otfdec_reset(otfdec_state);
    }
    mm_spiflash_set_decrypt_hook(mm_otfdec_decrypt_block, otfdec_state);

    /* FLASH reset values */
    flash_ctl.regs[FLASH_ACR / 4] = 0x00000013u;
    flash_ctl.regs[FLASH_NSCR / 4] = 0x00000001u;
    flash_ctl.regs[FLASH_SECCR / 4] = 0x00000001u;
    if ((mm_otp_flags(&otp_state) & MM_OTP_FLAG_FINAL_LOCK) != 0u) {
        flash_ctl.regs[FLASH_OTPBLR_CUR / 4u] = 0xFFFFFFFFu;
    }
    flash_sync_option_regs(&flash_ctl);
}

mm_u32 *stm32h5_tzsc_regs(void)
{
    return tzsc_s.regs;
}

static mm_bool tzsc_bit_is_secure(mm_u32 offset, mm_u32 bit)
{
    return ((tzsc_s.regs[offset / 4u] >> bit) & 1u) != 0u;
}

mm_bool stm32h5_tz_attr_for_addr(mm_u32 addr,
                                      enum mm_sau_attr *attr_out,
                                      mm_u32 *region_out)
{
    mm_bool secure = MM_FALSE;

    if (attr_out == 0) {
        return MM_FALSE;
    }
    if (region_out != 0) {
        *region_out = 0u;
    }

    if (addr >= 0x50000000u && addr < 0x60000000u) {
        *attr_out = MM_SAU_SECURE;
        return MM_TRUE;
    }

    if (addr >= 0x40013800u && addr < 0x40013C00u) {
        secure = tzsc_bit_is_secure(0x14u, 11u); /* USART1SEC */
    } else if (addr >= 0x40004400u && addr < 0x40004800u) {
        secure = tzsc_bit_is_secure(0x10u, 13u); /* USART2SEC */
    } else if (addr >= 0x40004800u && addr < 0x40004C00u) {
        secure = tzsc_bit_is_secure(0x10u, 14u); /* USART3SEC */
    } else {
        return MM_FALSE;
    }

    *attr_out = secure ? MM_SAU_SECURE : MM_SAU_NONSECURE;
    return MM_TRUE;
}

mm_bool stm32h5_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    mm_u32 word;
    mm_u32 bit;
    mm_u32 val;
    mm_bool sec;
    if (bank < 0 || bank >= (int)V->mpcbb_count) {
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
    /* AHB2ENR offset 0x8C: GPIOAEN bit0..GPIOJEN bit9 are contiguous, but
     * GPIOKEN sits at bit 26 (RM0517 Section 11.8.27). */
    mm_u32 ahb2enr = rcc->regs[0x8c / 4];
    mm_u32 bit;
    if (index < 0 || index > 10) {
        return MM_FALSE;
    }
    bit = (index == 10) ? 26u : (mm_u32)index;
    return ((ahb2enr >> bit) & 1u) != 0u;
}

static mm_bool stm32h5_gpio_clock_enabled_cb(int bank)
{
    return gpio_clock_enabled(&rcc, bank) || gpio_clock_enabled(&rcc_s, bank);
}

static void exti_gpio_update_cb(int bank, mm_u32 old_level, mm_u32 new_level)
{
    exti_gpio_update(bank, old_level, new_level);
    mm_spi_bus_poll_all();
}

enum rcc_bus_kind {
    RCC_BUS_AHB1 = 0,
    RCC_BUS_AHB2 = 1,
    RCC_BUS_APB1L = 2,
    RCC_BUS_APB1H = 3,
    RCC_BUS_APB2 = 4,
    RCC_BUS_APB3 = 5
};

struct rcc_clk_name {
    const char *name;
    enum rcc_bus_kind bus;
    mm_u32 bit;
};

static const struct rcc_clk_name rcc_clk_names[] = {
    { "ETH", RCC_BUS_AHB1, 19u },
    { "ETHTX", RCC_BUS_AHB1, 20u },
    { "ETHRX", RCC_BUS_AHB1, 21u },
    { "GPIOA", RCC_BUS_AHB2, 0u },
    { "GPIOB", RCC_BUS_AHB2, 1u },
    { "GPIOC", RCC_BUS_AHB2, 2u },
    { "GPIOD", RCC_BUS_AHB2, 3u },
    { "GPIOE", RCC_BUS_AHB2, 4u },
    { "GPIOF", RCC_BUS_AHB2, 5u },
    { "GPIOG", RCC_BUS_AHB2, 6u },
    { "GPIOH", RCC_BUS_AHB2, 7u },
    { "GPIOI", RCC_BUS_AHB2, 8u },
    { "GPIOJ", RCC_BUS_AHB2, 9u },
    { "GPIOK", RCC_BUS_AHB2, 26u },
    { "RNG", RCC_BUS_AHB2, 18u },
    { "PKA", RCC_BUS_AHB2, 19u },
    { "TIM2", RCC_BUS_APB1L, 0u },
    { "TIM3", RCC_BUS_APB1L, 1u },
    { "TIM4", RCC_BUS_APB1L, 2u },
    { "TIM5", RCC_BUS_APB1L, 3u },
    { "USART2", RCC_BUS_APB1L, 17u },
    { "USART3", RCC_BUS_APB1L, 18u },
    { "UART4", RCC_BUS_APB1L, 19u },
    { "UART5", RCC_BUS_APB1L, 20u },
    { "USART6", RCC_BUS_APB1L, 25u },
    { "UART7", RCC_BUS_APB1L, 30u },
    { "UART8", RCC_BUS_APB1L, 31u },
    { "UART9", RCC_BUS_APB1H, 0u },
    { "UART12", RCC_BUS_APB1H, 1u },
    { "USART1", RCC_BUS_APB2, 14u }
};

static mm_u32 rcc_bus_reg(const struct rcc_state *r, enum rcc_bus_kind bus)
{
    if (r == 0) {
        return 0u;
    }
    switch (bus) {
    case RCC_BUS_AHB1:
        return r->regs[0x88 / 4];
    case RCC_BUS_AHB2:
        return r->regs[0x8c / 4];
    case RCC_BUS_APB1L:
        return r->regs[0x9c / 4];
    case RCC_BUS_APB1H:
        return r->regs[0xa0 / 4];
    case RCC_BUS_APB2:
        return r->regs[0xa4 / 4];
    case RCC_BUS_APB3:
        return r->regs[0xa8 / 4];
    default:
        return 0u;
    }
}

static const char *rcc_bus_name(enum rcc_bus_kind bus)
{
    switch (bus) {
    case RCC_BUS_AHB1: return "AHB1";
    case RCC_BUS_AHB2: return "AHB2";
    case RCC_BUS_APB1L: return "APB1L";
    case RCC_BUS_APB1H: return "APB1H";
    case RCC_BUS_APB2: return "APB2";
    case RCC_BUS_APB3: return "APB3";
    default: return "RCC";
    }
}

static mm_bool stm32h5_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len)
{
    enum rcc_bus_kind bus;
    int line_idx = 0;
    size_t i;
    (void)opaque;
    if (out == 0 || out_len == 0u) {
        return MM_FALSE;
    }
    for (bus = RCC_BUS_AHB1; bus <= RCC_BUS_APB3; ++bus) {
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
            /* The GPIO entries run GPIOA.. in order; hide the banks this
             * part does not carry. */
            if (rcc_clk_names[i].name[0] == 'G' && rcc_clk_names[i].name[1] == 'P' &&
                (int)(rcc_clk_names[i].name[4] - 'A') >= V->gpio_count) {
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

static mm_u32 stm32h5_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[0x14u / 4];
}

static mm_u32 stm32h5_gpio_bank_read_moder(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[0x00u / 4];
}

static mm_bool stm32h5_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    return gpio_clock_enabled(&rcc, bank);
}

static mm_u32 stm32h5_gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)(sizeof(gpio) / sizeof(gpio[0]))) {
        return 0u;
    }
    return gpio[bank].regs[0x30u / 4];
}

static mm_bool rng_clock_enabled(const struct rcc_state *rcc)
{
    /* RCC_AHB2ENR offset 0x8C, RNGEN bit18 */
    mm_u32 ahb2enr = rcc->regs[0x8c / 4];
    return ((ahb2enr >> 18) & 1u) != 0u;
}

static mm_bool rng_clock_on_any(void)
{
    /* The RNG clock enable is shared hardware state: the secure and
     * non-secure RCC views both act on the same clock, so honor the
     * enable written through either alias. */
    return rng_clock_enabled(&rcc) || rng_clock_enabled(&rcc_s);
}

static mm_bool rng_requires_secure(const struct simple_blk *tzsc)
{
    /* GTZC1_TZSC_SECCFGR3 offset 0x18, RNGSEC bit18 */
    mm_u32 seccfgr3 = tzsc->regs[TZSC_SECCFGR3_OFFSET / 4u];
    return ((seccfgr3 >> TZSC_RNGSEC_BIT) & 1u) != 0u;
}

static mm_bool tzsc_requires_secure(const struct simple_blk *tzsc, mm_u32 bit)
{
    mm_u32 seccfgr3;
    if (bit >= 32u) {
        return MM_FALSE;
    }
    seccfgr3 = tzsc->regs[TZSC_SECCFGR3_OFFSET / 4u];
    return ((seccfgr3 >> bit) & 1u) != 0u;
}

static mm_bool hash_requires_secure(const struct simple_blk *tzsc)
{
    return tzsc_requires_secure(tzsc, TZSC_HASHSEC_BIT);
}

static mm_bool aes_requires_secure(const struct simple_blk *tzsc, mm_bool is_saes)
{
    return tzsc_requires_secure(tzsc, is_saes ? TZSC_SAESSEC_BIT : TZSC_AESSEC_BIT);
}

static mm_bool pka_requires_secure(const struct simple_blk *tzsc)
{
    return tzsc_requires_secure(tzsc, TZSC_PKASEC_BIT);
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
    mm_u32 swap_bit;
    if (f == 0) {
        return;
    }
    if (!f->dualbank_enabled) {
        f->swap_active = MM_FALSE;
    }
    swap_bit = f->swap_active ? FLASH_OPTSR_SWAP_BANK : 0u;
    f->regs[FLASH_OPTSR_CUR / 4u] = (f->regs[FLASH_OPTSR_CUR / 4u] & ~FLASH_OPTSR_SWAP_BANK) | swap_bit;
    f->regs[FLASH_OPTSR_PRG / 4u] = (f->regs[FLASH_OPTSR_PRG / 4u] & ~FLASH_OPTSR_SWAP_BANK) | swap_bit;
}

static mm_u32 flash_sector_size(void)
{
    /* FLASH_SECTOR_COUNT is the total sector count over the whole flash:
     * sectors are 8 Kbytes on STM32H5 (RM0517, 256 sectors per bank), so
     * the bank count does not enter the division. */
    return (FLASH_SECTOR_COUNT == 0u) ? 0u
        : (flash_ctl.flash_size / FLASH_SECTOR_COUNT);
}

static void flash_init_secwm_defaults(struct flash_state *f)
{
    /* Out of reset the emulated device is not provisioned: no secure
     * watermark. Firmware (or a provisioning step) may program the
     * watermark via the _PRG registers. */
    f->regs[FLASH_SECWM1R_CUR / 4u] = FLASH_SECWM_EMPTY;
    f->regs[FLASH_SECWM1R_PRG / 4u] = FLASH_SECWM_EMPTY;
    f->regs[FLASH_SECWM2R_CUR / 4u] = FLASH_SECWM_EMPTY;
    f->regs[FLASH_SECWM2R_PRG / 4u] = FLASH_SECWM_EMPTY;
}

static mm_bool flash_sector_attr_secure(struct flash_state *f, mm_u32 offset)
{
    mm_u32 sector_size = flash_sector_size();
    mm_u32 bank_size;
    mm_u32 sector;
    mm_bool phys_hi;
    mm_u32 secwm;
    mm_u32 strt;
    mm_u32 end;
    mm_u32 bb_base;

    if (f == 0 || f->flash_size == 0u || sector_size == 0u ||
            FLASH_BANK_COUNT < 2u) {
        return MM_FALSE;
    }
    bank_size = f->flash_size / FLASH_BANK_COUNT;
    if (bank_size == 0u || offset >= f->flash_size) {
        return MM_FALSE;
    }
    /* The backing array holds the logical (post-swap) view; SECWM/SECBB
     * refer to physical banks. */
    phys_hi = (offset >= bank_size) ? MM_TRUE : MM_FALSE;
    if (f->swap_active) {
        phys_hi = phys_hi ? MM_FALSE : MM_TRUE;
    }
    sector = (offset % bank_size) / sector_size;

    secwm = f->regs[(phys_hi ? FLASH_SECWM2R_CUR : FLASH_SECWM1R_CUR) / 4u];
    strt = secwm & FLASH_SECWM_STRT_MASK;
    end = (secwm >> FLASH_SECWM_END_SHIFT) & FLASH_SECWM_STRT_MASK;
    if (sector >= strt && sector <= end) {
        return MM_TRUE;
    }

    bb_base = phys_hi ? FLASH_SECBB2R : FLASH_SECBB1R;
    if ((sector / 32u) < FLASH_SECBB_NREGS) {
        mm_u32 reg = f->regs[(bb_base + 4u * (sector / 32u)) / 4u];
        if ((reg >> (sector % 32u)) & 1u) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool flash_sector_secure_cb(void *opaque, mm_u32 byte_offset)
{
    struct flash_state *f = (struct flash_state *)opaque;
    return flash_sector_attr_secure(f, byte_offset);
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
    mm_bool bank2;
    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0 || sector_size == 0) {
        return;
    }
    /* BKSEL always refers to the physical bank, whatever the SWAP_BANK
     * setting (RM0517), and selects the physical bank even when dualbank
     * is disabled. The backing array holds the logical (post-swap) view,
     * so the physical bank selection must be inverted while the banks are
     * swapped. */
    if (flash_ctl.flash_size != 0u) {
        if (FLASH_BANK_COUNT > 1u && flash_ctl.flash_size >= FLASH_BANK_COUNT) {
            bank_size = flash_ctl.flash_size / FLASH_BANK_COUNT;
        } else {
            bank_size = (bank_count != 0u) ? (flash_ctl.flash_size / bank_count) : 0u;
        }
    }
    bank2 = ((cr & FLASH_CR_BKSEL) != 0u) ? MM_TRUE : MM_FALSE;
    if (flash_ctl.swap_active) {
        bank2 = bank2 ? MM_FALSE : MM_TRUE;
    }
    if ((cr & FLASH_CR_BER) != 0u) {
        /* Bank erase targets the physical bank selected by BKSEL (RM0517:
         * "swap setting is ignored"). */
        if (bank_size != 0u) {
            start = bank2 ? bank_size : 0u;
            length = bank_size;
        } else {
            start = 0;
            length = flash_ctl.flash_size;
        }
    } else if ((cr & FLASH_CR_SER) != 0u) {
        mm_u32 snb = (cr & FLASH_CR_SNB_MASK) >> FLASH_CR_SNB_SHIFT;
        start = snb * sector_size;
        if (bank2 && bank_size != 0u) {
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
        const char *mode = (cr & FLASH_CR_BER) ? "BER" : "SER";
        mm_u32 snb = (cr & FLASH_CR_SNB_MASK) >> FLASH_CR_SNB_SHIFT;
        printf("[FLASH_ERASE] %s mode=%s snb=%lu start=0x%08lx len=0x%08lx\n",
               sec,
               mode,
               (unsigned long)snb,
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
    if (base == flash_ctl.base_ns &&
            flash_sector_attr_secure(&flash_ctl, offset)) {
        /* NS-alias write to a secure sector: write ignored (RM0517). */
        return MM_TRUE;
    }
    if (!flash_is_unlocked(cr_off)) {
        return MM_TRUE;
    }
    if ((flash_ctl.regs[cr_off / 4u] & FLASH_CR_PG) == 0u) {
        return MM_TRUE;
    }

    erase_mode = ((flash_ctl.regs[cr_off / 4u] & FLASH_CR_SER) != 0u) ? MM_TRUE : MM_FALSE;
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

static mm_bool otp_block_locked(mm_u32 offset)
{
    mm_u32 block = offset / FLASH_OTP_BLOCK_SIZE;
    if (block >= FLASH_OTP_BLOCK_COUNT) return MM_TRUE;
    if ((flash_ctl.regs[FLASH_OTPBLR_CUR / 4u] & (1u << block)) != 0u) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool otp_mem_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u8 buf[4];
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > FLASH_OTP_SIZE) return MM_FALSE;
    if (!mm_otp_read(&otp_state, offset, buf, size_bytes)) return MM_FALSE;
    *value_out = 0u;
    if (size_bytes >= 1u) *value_out |= buf[0];
    if (size_bytes >= 2u) *value_out |= (mm_u32)buf[1] << 8;
    if (size_bytes >= 3u) *value_out |= (mm_u32)buf[2] << 16;
    if (size_bytes >= 4u) *value_out |= (mm_u32)buf[3] << 24;
    return MM_TRUE;
}

static mm_bool otp_mem_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u8 buf[4];
    enum mm_sec_state sec = mmio_active_sec();
    mm_u32 cr_off = (sec == MM_SECURE) ? FLASH_SECCR : FLASH_NSCR;
    mm_u32 sr_off = (sec == MM_SECURE) ? FLASH_SECSR : FLASH_NSSR;
    (void)opaque;
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > FLASH_OTP_SIZE) return MM_FALSE;
    if (!flash_is_unlocked(cr_off) || (flash_ctl.regs[cr_off / 4u] & FLASH_CR_PG) == 0u) {
        flash_ctl.regs[sr_off / 4u] |= FLASH_FLAG_PGSERR;
        return MM_TRUE;
    }
    if (otp_block_locked(offset)) {
        flash_ctl.regs[sr_off / 4u] |= FLASH_FLAG_PGSERR;
        return MM_TRUE;
    }
    buf[0] = (mm_u8)(value & 0xffu);
    buf[1] = (mm_u8)((value >> 8) & 0xffu);
    buf[2] = (mm_u8)((value >> 16) & 0xffu);
    buf[3] = (mm_u8)((value >> 24) & 0xffu);
    if (!mm_otp_write(&otp_state, offset, buf, size_bytes)) {
        flash_ctl.regs[sr_off / 4u] |= FLASH_FLAG_PGSERR;
        return MM_TRUE;
    }
    return MM_TRUE;
}

/* Reset bits currently unused (could clear state if asserted). */

static mm_bool rcc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    if (offset <= RCC_CFGR1 && (offset + size_bytes) > RCC_CFGR1) {
        mm_u32 cfgr1 = r->regs[RCC_CFGR1 / 4];
        mm_u32 sw = cfgr1 & 0x7u;
        r->regs[RCC_CFGR1 / 4] = (cfgr1 & ~(0x7u << 3)) | (sw << 3);
    }
    memcpy(value_out, (mm_u8 *)r->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_u64 rcc_hsi_hz(const struct rcc_state *r)
{
    mm_u32 cr = r->regs[RCC_CR / 4u];
    mm_u32 divsel = (cr >> 3) & 0x3u;
    mm_u32 div = 1u << divsel;
    return 64000000ull / (mm_u64)div;
}

static void rcc_update_ready(struct rcc_state *r)
{
    mm_u32 cr = r->regs[RCC_CR / 4u];
    /* Mirror RDY bits to match ON bits (immediate ready). */
    /* HSIRDY bit1 follows HSION bit0 */
    if ((cr & (1u << 0)) != 0u) cr |= (1u << 1); else cr &= ~(1u << 1);
    /* HSIDIVF bit5 reflects immediate divider propagation while HSI is on. */
    if ((cr & (1u << 0)) != 0u) cr |= (1u << 5); else cr &= ~(1u << 5);
    /* CSIRDY bit9 follows CSION bit8 */
    if ((cr & (1u << 8)) != 0u) cr |= (1u << 9); else cr &= ~(1u << 9);
    /* HSI48RDY bit13 follows HSI48ON bit12 */
    if ((cr & (1u << 12)) != 0u) cr |= (1u << 13); else cr &= ~(1u << 13);
    /* HSERDY bit17 follows HSEON bit16 */
    if ((cr & (1u << 16)) != 0u) cr |= (1u << 17); else cr &= ~(1u << 17);
    /* PLL1RDY bit25 follows PLL1ON bit24 */
    if ((cr & (1u << 24)) != 0u) cr |= (1u << 25); else cr &= ~(1u << 25);
    /* PLL2RDY bit27 follows PLL2ON bit26 */
    if ((cr & (1u << 26)) != 0u) cr |= (1u << 27); else cr &= ~(1u << 27);
    /* PLL3RDY bit29 follows PLL3ON bit28 */
    if ((cr & (1u << 28)) != 0u) cr |= (1u << 29); else cr &= ~(1u << 29);
    r->regs[0] = cr;
}

static void rcc_update_bdcr(struct rcc_state *r)
{
    mm_u32 bdcr = r->regs[RCC_BDCR / 4];
    if ((bdcr & (1u << 0)) != 0u) bdcr |= (1u << 1); else bdcr &= ~(1u << 1);
    if ((bdcr & (1u << 26)) != 0u) bdcr |= (1u << 27); else bdcr &= ~(1u << 27);
    r->regs[RCC_BDCR / 4] = bdcr;
}

static mm_u64 rcc_pll1_p_clk(const struct rcc_state *r)
{
    mm_u32 pllcfgr = r->regs[RCC_PLL1CFGR / 4];
    mm_u32 plldivr = r->regs[RCC_PLL1DIVR / 4];
    mm_u32 src = pllcfgr & 0x3u;
    mm_u64 fin = 0;
    mm_u32 divm = (pllcfgr >> 8) & 0x3fu;
    mm_u32 n = (plldivr & 0x1ffu) + 1u;
    mm_u32 p = ((plldivr >> 9) & 0x7fu) + 1u;

    if (src == 1u) fin = rcc_hsi_hz(r); /* HSI */
    else if (src == 2u) fin = 4000000ull; /* CSI */
    else if (src == 3u) fin = 8000000ull; /* HSE */
    else fin = 0;

    if (fin == 0 || divm == 0u || p == 0u) {
        return 0;
    }
    return (fin / (mm_u64)divm) * (mm_u64)n / (mm_u64)p;
}

static void rcc_update_sysclk(struct rcc_state *r)
{
    mm_u32 cfgr1 = r->regs[RCC_CFGR1 / 4];
    mm_u32 cfgr2 = r->regs[RCC_CFGR2 / 4];
    mm_u32 sw = cfgr1 & 0x7u;
    mm_u32 hpre = cfgr2 & 0xfu;
    mm_u64 sys = 0;
    mm_u32 div = 1u;

    if (sw == 0u) sys = rcc_hsi_hz(r);
    else if (sw == 1u) sys = 4000000ull;
    else if (sw == 2u) sys = 8000000ull;
    else if (sw == 3u) sys = rcc_pll1_p_clk(r);

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
    r->regs[RCC_CFGR1 / 4] = (r->regs[RCC_CFGR1 / 4] & ~(0x7u << 3)) | (sw << 3);
}

static mm_bool rcc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)r->regs + offset, &value, size_bytes);
    if (offset == RCC_CR) {
        rcc_update_ready(r);
    }
    if (offset == RCC_BDCR) {
        rcc_update_bdcr(r);
    }
    if (offset == RCC_CFGR1 || offset == RCC_CFGR2 ||
        offset == RCC_PLL1CFGR || offset == RCC_PLL1DIVR ||
        offset == RCC_PLL2CFGR || offset == RCC_PLL3CFGR ||
        offset == RCC_PLL2DIVR || offset == RCC_PLL2FRACR ||
        offset == RCC_PLL3DIVR || offset == RCC_PLL3FRACR ||
        offset == RCC_CR) {
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
    mm_u32 voscr = p->regs[0x10 / 4];
    mm_u32 vos = (voscr >> 4) & 0x3u;
    mm_u32 vossr = p->regs[0x14 / 4];
    vossr &= ~((1u << 14) | (1u << 13));
    vossr |= (vos << 14);      /* ACTVOS mirrors VOS */
    vossr |= (1u << 13);       /* ACTVOSRDY set */
    vossr |= (1u << 3);        /* VOSRDY set */
    p->regs[0x14 / 4] = vossr;
}

static void pwr_update_usb(struct pwr_state *p)
{
    mm_u32 usbscr = p->regs[0x38 / 4];
    mm_u32 vmsr = p->regs[0x3c / 4];
    if ((usbscr & (1u << 24)) != 0u && (usbscr & (1u << 25)) != 0u) {
        vmsr |= (1u << 24); /* USB33RDY */
    } else {
        vmsr &= ~(1u << 24);
    }
    p->regs[0x3c / 4] = vmsr;
}

static mm_bool pwr_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct pwr_state *p = (struct pwr_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)p->regs + offset, &value, size_bytes);
    if (offset == 0x10u) {
        pwr_update_vos(p);
    }
    if (offset == 0x38u) {
        pwr_update_usb(p);
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
    for (bank = 0; bank < V->mpcbb_count; ++bank) {
        mm_u32 word;
        memset(&mpcbb[bank], 0, sizeof(mpcbb[bank]));
        for (word = 0; word < mpcbb_words[bank]; ++word) {
            mpcbb[bank].regs[(MPCBB_SECCFGR_OFFSET / 4u) + word] = 0xFFFFFFFFu;
        }
    }
}

static mm_bool flash_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct flash_state *f = (struct flash_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FLASH_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)f->regs + offset, size_bytes);
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

    if (offset == FLASH_OPTSR_CUR || offset == FLASH_OPTSR_PRG) {
        mm_u32 new_val = value;
        if (!f->dualbank_enabled) {
            new_val &= ~FLASH_OPTSR_SWAP_BANK;
        }
        f->regs[offset / 4u] = (f->regs[offset / 4u] & ~FLASH_OPTSR_SWAP_BANK) | (new_val & FLASH_OPTSR_SWAP_BANK);
        if (offset == FLASH_OPTSR_PRG) {
            f->regs[FLASH_OPTSR_CUR / 4u] = (f->regs[FLASH_OPTSR_CUR / 4u] & ~FLASH_OPTSR_SWAP_BANK) | (new_val & FLASH_OPTSR_SWAP_BANK);
        }
        flash_set_swap_state(f, (new_val & FLASH_OPTSR_SWAP_BANK) != 0u);
        return MM_TRUE;
    }

    if (offset == FLASH_OTPBLR_CUR) {
        return MM_TRUE;
    }
    if (offset == FLASH_OTPBLR_PRG) {
        mm_u32 cur = f->regs[FLASH_OTPBLR_CUR / 4u];
        mm_u32 set_bits = value & ~cur;
        cur |= set_bits;
        f->regs[FLASH_OTPBLR_CUR / 4u] = cur;
        f->regs[FLASH_OTPBLR_PRG / 4u] = value;
        if (cur == 0xFFFFFFFFu) {
            (void)mm_otp_set_flags(&otp_state, MM_OTP_FLAG_FINAL_LOCK);
        }
        return MM_TRUE;
    }

    if (offset == FLASH_SECWM1R_CUR || offset == FLASH_SECWM2R_CUR) {
        /* _CUR registers are loaded from option bytes, not writable. */
        return MM_TRUE;
    }

    if (offset == FLASH_SECWM1R_PRG || offset == FLASH_SECWM2R_PRG) {
        /* The model applies option-byte programming immediately, in line
         * with the OPTSR_PRG handling. */
        mm_u32 cur_off = (offset == FLASH_SECWM1R_PRG) ? FLASH_SECWM1R_CUR
                                                       : FLASH_SECWM2R_CUR;
        f->regs[offset / 4u] = value;
        f->regs[cur_off / 4u] = value;
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
    struct simple_blk *tzsc = (struct simple_blk *)((void **)opaque)[3];

    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RNG_SIZE) return MM_FALSE;

    if (!secure_alias && rng_requires_secure(tzsc)) {
        *value_out = 0;
        return MM_TRUE;
    }
    if (!rng_clock_on_any()) {
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
    struct simple_blk *tzsc = (struct simple_blk *)((void **)opaque)[3];

    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RNG_SIZE) return MM_FALSE;

    if (!secure_alias && rng_requires_secure(tzsc)) {
        return MM_TRUE;
    }
    if (!rng_clock_on_any()) {
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
    return ((rcc.regs[0x9c / 4] >> 11) & 1u) != 0u ||
           ((rcc_s.regs[0x9c / 4] >> 11) & 1u) != 0u;
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
    if (val >= (mm_u32)V->gpio_count) return -1;
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

mm_u32 *stm32h5_rcc_regs(void)
{
    return rcc.regs;
}

mm_u32 *stm32h5_rcc_secure_regs(void)
{
    return rcc_s.regs;
}

mm_u64 stm32h5_cpu_hz(void)
{
    return rcc_s.cpu_hz != 0u ? rcc_s.cpu_hz : rcc.cpu_hz;
}

void stm32h5_exti_set_nvic(struct mm_nvic *nvic)
{
    g_exti_nvic = nvic;
    g_wdg_nvic = nvic;
}

void stm32h5_watchdog_tick(mm_u64 cycles)
{
    static const mm_u32 iwdg_presc_div[8] = { 4u, 8u, 16u, 32u, 64u, 128u, 256u, 256u };
    mm_u64 cpu_hz = stm32h5_cpu_hz();

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

static mm_bool stm32h5_register_mmio_impl(struct mmio_bus *bus)
{
    struct mmio_region reg;

    memset(&rcc, 0, sizeof(rcc));
    memset(&rcc_s, 0, sizeof(rcc_s));
    memset(&pwr, 0, sizeof(pwr));
    memset(&tzsc_s, 0, sizeof(tzsc_s));
    memset(&tzsc_ns, 0, sizeof(tzsc_ns));
    memset(&tzic_s, 0, sizeof(tzic_s));
    memset(&tzic_ns, 0, sizeof(tzic_ns));
    memset(&bsec, 0, sizeof(bsec));
    memset(&rng, 0, sizeof(rng));
    memset(&hash_accel, 0, sizeof(hash_accel));
    memset(&aes_accel, 0, sizeof(aes_accel));
    memset(&saes_accel, 0, sizeof(saes_accel));
    memset(&pka_accel, 0, sizeof(pka_accel));
    memset(&exti, 0, sizeof(exti));
    memset(&iwdg, 0, sizeof(iwdg));
    memset(&wwdg, 0, sizeof(wwdg));
    {
        /* SWAP_BANK is a non-volatile option bit: it must survive MMIO
         * re-registration on system reset, like in the reset handler. */
        mm_bool swap_active = flash_ctl.swap_active;
        mm_bool dualbank = flash_ctl.dualbank_enabled;
        memset(&flash_ctl, 0, sizeof(flash_ctl));
        flash_ctl.swap_active = swap_active;
        flash_ctl.dualbank_enabled = dualbank;
    }
    flash_init_secwm_defaults(&flash_ctl);
    mpcbb_init_defaults();
    {
        size_t gi;
        for (gi = 0; gi < (size_t)V->gpio_count; ++gi) {
            stm32_gpio_reset(&gpio[gi], (int)gi);
        }
    }
    gpdma_configure_and_reset();
    rcc.regs[RCC_CR / 4] |= 1u;
    rcc.regs[0x8c / 4u] = V->ahb2enr_reset;
    rcc_update_ready(&rcc);
    rcc_update_sysclk(&rcc);
    rcc_s = rcc;
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
    saes_ctx[0].state = &saes_accel;
    saes_ctx[0].secure_alias = MM_FALSE;
    saes_ctx[0].rcc = &rcc;
    saes_ctx[0].tzsc = &tzsc_s;
    saes_ctx[0].is_saes = MM_TRUE;
    saes_ctx[0].clock_enabled = aes_clock_enabled;
    saes_ctx[0].requires_secure = aes_requires_secure;
    saes_ctx[1].state = &saes_accel;
    saes_ctx[1].secure_alias = MM_TRUE;
    saes_ctx[1].rcc = &rcc;
    saes_ctx[1].tzsc = &tzsc_s;
    saes_ctx[1].is_saes = MM_TRUE;
    saes_ctx[1].clock_enabled = aes_clock_enabled;
    saes_ctx[1].requires_secure = aes_requires_secure;
    rng.regs[RNG_CR_OFFSET / 4] = V->rng_cr_reset;
    rng.regs[RNG_HTCR_OFFSET / 4] = 0x000072acu;
    if (V->rng_has_htcr123) {
        rng.regs[RNG_HTCR1_OFFSET / 4] = 0x0000ffffu;
        rng.regs[RNG_HTCR2_OFFSET / 4] = 0x0000ffffu;
        rng.regs[RNG_HTCR3_OFFSET / 4] = 0x0000ffffu;
    }
    flash_ctl.regs[FLASH_ACR / 4] = 0x00000013u;
    flash_ctl.regs[FLASH_NSCR / 4] = 0x00000001u;
    flash_ctl.regs[FLASH_SECCR / 4] = 0x00000001u;
    iwdg.regs[IWDG_RLR / 4] = 0x00000FFFu;
    iwdg.regs[IWDG_WINR / 4] = 0x00000FFFu;
    wwdg.regs[WWDG_CR / 4] = 0x0000007Fu;
    wwdg.regs[WWDG_CFR / 4] = 0x0000007Fu;
    wwdg.counter = 0x7Fu;
    exti.regs[EXTI_IMR1 / 4u] = 0xFFFE0000u;
    bsec.regs[BSEC_UID0_OFFSET / 4u] = BSEC_UID0_VALUE;
    bsec.regs[BSEC_UID1_OFFSET / 4u] = BSEC_UID1_VALUE;
    bsec.regs[BSEC_UID2_OFFSET / 4u] = BSEC_UID2_VALUE;
    uid.regs[UID0_OFFSET / 4u] = UID0_VALUE;
    uid.regs[UID1_OFFSET / 4u] = UID1_VALUE;
    uid.regs[UID2_OFFSET / 4u] = UID2_VALUE;

    /* RCC */
    reg.base = RCC_BASE;
    reg.size = RCC_SIZE;
    reg.opaque = &rcc;
    reg.read = rcc_read;
    reg.write = rcc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* RCC secure alias */
    reg.base = RCC_SEC_BASE;
    reg.opaque = &rcc_s;
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

    /* TAMP */
    reg.base = TAMP_BASE;
    reg.size = TAMP_SIZE;
    reg.opaque = &tamp;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* TAMP secure alias */
    reg.base = TAMP_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* SBS */
    reg.base = SBS_BASE;
    reg.size = SBS_SIZE;
    reg.opaque = &sbs;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* SBS secure alias */
    reg.base = SBS_SEC_BASE;
    reg.opaque = &sbs_sec;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* BSEC */
    reg.base = BSEC_BASE;
    reg.size = BSEC_SIZE;
    reg.opaque = &bsec;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
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

    /* FLASH OTP memory (non-secure and secure aliases) */
    reg.base = FLASH_OTP_BASE_NS;
    reg.size = FLASH_OTP_SIZE;
    reg.opaque = 0;
    reg.read = otp_mem_read;
    reg.write = otp_mem_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = FLASH_OTP_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* Unique device ID (non-secure and secure aliases) */
    reg.base = UID_BASE_NS;
    reg.size = UID_SIZE;
    reg.opaque = &uid;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = UID_BASE_S;
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

    /* ICACHE (non-secure and secure aliases) */
    reg.base = ICACHE_BASE;
    reg.size = CACHE_SIZE;
    reg.opaque = &icache;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = ICACHE_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* DCACHE (non-secure and secure aliases) */
    reg.base = DCACHE_BASE;
    reg.size = CACHE_SIZE;
    reg.opaque = &dcache;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = DCACHE_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* MPCBB1..n (non-secure and secure aliases). The controllers are
     * evenly spaced from MPCBB1_BASE, one per SRAM bank. */
    reg.size = MPCBB_SIZE;
    reg.read = mpcbb_read;
    reg.write = mpcbb_write;
    {
        mm_u32 n;
        for (n = 0; n < V->mpcbb_count; ++n) {
            reg.opaque = &mpcbb[n];
            reg.base = MPCBB1_BASE + (n * MPCBB_SIZE);
            if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
            reg.base = MPCBB1_SEC_BASE + (n * MPCBB_SIZE);
            if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
        }
    }

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

    /* OTFDEC (non-secure and secure aliases) */
    if (otfdec_state == 0) {
        otfdec_state = mm_otfdec_new();
    }
    if (otfdec_state != 0) {
        reg.base  = 0x420C5000u;
        reg.size  = 0x400u;
        reg.opaque = otfdec_state;
        reg.read  = mm_otfdec_read;
        reg.write = mm_otfdec_write;
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
        reg.base  = 0x520C5000u;
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
        mm_spiflash_set_decrypt_hook(mm_otfdec_decrypt_block, otfdec_state);
    }

    /* SAES (non-secure and secure aliases) */
    reg.base = SAES_BASE;
    reg.size = SAES_SIZE;
    reg.opaque = &saes_ctx[0];
    reg.read = aes_read;
    reg.write = aes_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = SAES_SEC_BASE;
    reg.opaque = &saes_ctx[1];
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
    rng_ctx[1][2] = &rcc_s;
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

    /* DBGMCU */
    reg.base = DBGMCU_BASE;
    reg.size = DBGMCU_SIZE;
    reg.opaque = &dbgmcu;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
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

    /* GPDMA1/GPDMA2 (non-secure and secure aliases) - using shared implementation */
    reg.size = 0x1000u;
    reg.read = stm32_gpdma_read;
    reg.write = stm32_gpdma_write;
    reg.base = 0x40020000u; /* GPDMA1 */
    reg.opaque = &gpdma1;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = 0x50020000u; /* SEC_GPDMA1 */
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = 0x40021000u; /* GPDMA2 */
    reg.opaque = &gpdma2;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = 0x50021000u; /* SEC_GPDMA2 */
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GPIO A..K: NS alias 0x4202xxxx, Secure alias 0x5202xxxx - using shared implementation */
    {
        mm_u32 base_ns = 0x42020000u;
        mm_u32 base_s  = 0x52020000u;
        int i;
        for (i = 0; i < V->gpio_count; ++i) {
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

    if (V->usb_register_mmio != 0 && !V->usb_register_mmio(bus)) return MM_FALSE;
    if (!stm32h5_eth_register_mmio(bus, stm32h5_rcc_regs())) return MM_FALSE;
    if (!stm32h5_i2c_init(bus, 0, V->i2c_count)) return MM_FALSE;
    return MM_TRUE;
}

static void stm32h5_flash_bind_impl(struct mm_memmap *map,
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
    mm_memmap_set_flash_sector_secure(map, flash_sector_secure_cb, &flash_ctl);
}

static void stm32h5_otp_init_impl(const char *target_name)
{
    mm_otp_init(&otp_state, target_name, FLASH_OTP_SIZE);
}

void stm32h5_rng_set_nvic(struct mm_nvic *nvic)
{
    g_rng_nvic = nvic;
}

void stm32h5_gpdma_set_nvic(struct mm_nvic *nvic)
{
    stm32_gpdma_set_nvic(&gpdma1, nvic);
    stm32_gpdma_set_nvic(&gpdma2, nvic);
}

mm_u8 stm32h5_gpio_get_af(int bank, int pin)
{
    if (bank < 0 || bank >= V->gpio_count || pin < 0 || pin >= 16) {
        return 0;
    }
    return stm32_gpio_get_pin_af(&gpio[bank], pin);
}

mm_u8 stm32h5_gpio_get_mode(int bank, int pin)
{
    if (bank < 0 || bank >= V->gpio_count || pin < 0 || pin >= 16) {
        return 0;
    }
    return stm32_gpio_get_pin_mode(&gpio[bank], pin);
}


/* Public entry points. Each of the four a target can reach first latches
 * the variant, so main.c may call them in any order. */

mm_bool stm32h5_register_mmio(const struct stm32h5_mmio_variant *v, struct mmio_bus *bus)
{
    if (v == 0) return MM_FALSE;
    V = v;
    return stm32h5_register_mmio_impl(bus);
}

void stm32h5_mmio_reset(const struct stm32h5_mmio_variant *v)
{
    if (v == 0) return;
    V = v;
    stm32h5_mmio_reset_impl();
}

void stm32h5_flash_bind(const struct stm32h5_mmio_variant *v,
                        struct mm_memmap *map,
                        mm_u8 *flash,
                        mm_u32 flash_size,
                        const struct mm_flash_persist *persist,
                        mm_u32 flags)
{
    if (v == 0) return;
    V = v;
    stm32h5_flash_bind_impl(map, flash, flash_size, persist, flags);
}

void stm32h5_otp_init(const struct stm32h5_mmio_variant *v, const char *target_name)
{
    if (v == 0) return;
    V = v;
    stm32h5_otp_init_impl(target_name);
}
