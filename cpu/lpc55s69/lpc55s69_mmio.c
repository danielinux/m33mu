/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include "lpc55s69/lpc55s69_mmio.h"
#include "lpc55s69/lpc55s69_romapi.h"
#include "lpc55s69/lpc55s69_hashcrypt.h"
#include "lpc55s69/lpc55s69_casper.h"
#include "lpc55s69/lpc55s69_puf.h"
#include "lpc55s69/lpc55s69_rng.h"
#include "lpc55s69/cpu_config.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

/* -------------------------------------------------------------------------
 * SYSCON  (0x40000000 NS, 0x50000000 S)
 * ------------------------------------------------------------------------- */
#define SYSCON_BASE      0x40000000u
#define SYSCON_SEC_BASE  0x50000000u
#define SYSCON_SIZE      0x1000u

/* SYSCON key register offsets */
#define SYSCON_PRESETCTRL0    0x100u
#define SYSCON_PRESETCTRL1    0x104u
#define SYSCON_PRESETCTRL2    0x108u
#define SYSCON_AHBCLKCTRL0    0x200u
#define SYSCON_AHBCLKCTRL1    0x204u
#define SYSCON_AHBCLKCTRL2    0x208u
#define SYSCON_MAINCLKSELA      0x280u
#define SYSCON_MAINCLKSELB      0x284u
#define SYSCON_SYSTICKCLKSEL0   0x260u
#define SYSCON_SYSTICKCLKSEL1   0x264u
#define SYSCON_CLOCK_CTRL       0xA18u

/* SET/CLR register arrays:
 *   PRESETCTRLSET[0..2] @ 0x120/0x124/0x128  → write ORs into PRESETCTRL[n]
 *   PRESETCTRLCLR[0..2] @ 0x140/0x144/0x148  → write AND-NOTs into PRESETCTRL[n]
 *   AHBCLKCTRLSET[0..2] @ 0x220/0x224/0x228  → write ORs into AHBCLKCTRL[n]
 *   AHBCLKCTRLCLR[0..2] @ 0x240/0x244/0x248  → write AND-NOTs into AHBCLKCTRL[n]
 */
#define SYSCON_PRESETCTRLSET_BASE 0x120u
#define SYSCON_PRESETCTRLCLR_BASE 0x140u
#define SYSCON_AHBCLKCTRLSET_BASE 0x220u
#define SYSCON_AHBCLKCTRLCLR_BASE 0x240u

struct syscon_state {
    mm_u32 regs[SYSCON_SIZE / 4u];
};

static struct syscon_state syscon;

static mm_bool syscon_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                           mm_u32 *value_out)
{
    struct syscon_state *s = (struct syscon_state *)opaque;
    if (s == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > SYSCON_SIZE)
        return MM_FALSE;
    memcpy(value_out, (mm_u8 *)s->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool syscon_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                            mm_u32 value)
{
    struct syscon_state *s = (struct syscon_state *)opaque;
    mm_u32 idx;
    if (s == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > SYSCON_SIZE)
        return MM_FALSE;

    /*
     * PRESETCTRLSET[0..2]: writes OR into PRESETCTRLn.
     * PRESETCTRLCLR[0..2]: writes AND-NOT into PRESETCTRLn.
     * AHBCLKCTRLSET[0..2]: writes OR into AHBCLKCTRLn.
     * AHBCLKCTRLCLR[0..2]: writes AND-NOT into AHBCLKCTRLn.
     * Only 32-bit accesses are meaningful; sub-word fall through to plain store.
     */
    if (size_bytes == 4u) {
        if (offset >= SYSCON_PRESETCTRLSET_BASE &&
            offset <  SYSCON_PRESETCTRLSET_BASE + 3u * 4u) {
            idx = (offset - SYSCON_PRESETCTRLSET_BASE) / 4u;
            s->regs[(SYSCON_PRESETCTRL0 / 4u) + idx] |= value;
            return MM_TRUE;
        }
        if (offset >= SYSCON_PRESETCTRLCLR_BASE &&
            offset <  SYSCON_PRESETCTRLCLR_BASE + 3u * 4u) {
            idx = (offset - SYSCON_PRESETCTRLCLR_BASE) / 4u;
            s->regs[(SYSCON_PRESETCTRL0 / 4u) + idx] &= ~value;
            return MM_TRUE;
        }
        if (offset >= SYSCON_AHBCLKCTRLSET_BASE &&
            offset <  SYSCON_AHBCLKCTRLSET_BASE + 3u * 4u) {
            idx = (offset - SYSCON_AHBCLKCTRLSET_BASE) / 4u;
            s->regs[(SYSCON_AHBCLKCTRL0 / 4u) + idx] |= value;
            return MM_TRUE;
        }
        if (offset >= SYSCON_AHBCLKCTRLCLR_BASE &&
            offset <  SYSCON_AHBCLKCTRLCLR_BASE + 3u * 4u) {
            idx = (offset - SYSCON_AHBCLKCTRLCLR_BASE) / 4u;
            s->regs[(SYSCON_AHBCLKCTRL0 / 4u) + idx] &= ~value;
            return MM_TRUE;
        }
    }

    memcpy((mm_u8 *)s->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * IOCON  (0x40001000 NS, 0x50001000 S)
 * ------------------------------------------------------------------------- */
#define IOCON_BASE     0x40001000u
#define IOCON_SEC_BASE 0x50001000u
#define IOCON_SIZE     0x100u

static mm_u32 iocon_regs[IOCON_SIZE / 4u];

static mm_bool iocon_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > IOCON_SIZE)
        return MM_FALSE;
    memcpy(value_out, (mm_u8 *)iocon_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool iocon_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                           mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > IOCON_SIZE)
        return MM_FALSE;
    memcpy((mm_u8 *)iocon_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * GPIO  (0x4008C000 NS, 0x5008C000 S)
 * SECGPIO alias (0x400A8000 NS, 0x500A8000 S)
 *
 * LPC55S69 has 2 ports (64 pins total).
 * Register layout (offsets within the GPIO block):
 *   BYTE[0..63]  0x000..0x03F  byte-wide per-pin read/write
 *   WORD[0..63]  0x080..0x17F  word-wide per-pin read/write (bit 0 only)
 *   DIR[0..1]    0x2000, 0x2004
 *   MASK[0..1]   0x2080, 0x2084
 *   PIN[0..1]    0x2100, 0x2104
 *   MPIN[0..1]   0x2180, 0x2184
 *   SET[0..1]    0x2200, 0x2204  (write-only: OR into PIN)
 *   CLR[0..1]    0x2280, 0x2284  (write-only: AND-NOT into PIN)
 *   NOT[0..1]    0x2300, 0x2304  (write-only: XOR into PIN)
 *   DIRSET[0..1] 0x2380, 0x2384
 *   DIRCLR[0..1] 0x2400, 0x2404
 *   DIRNOT[0..1] 0x2480, 0x2484
 * ------------------------------------------------------------------------- */
#define GPIO_BASE      0x4008C000u
#define GPIO_SEC_BASE  0x5008C000u
#define SECGPIO_BASE     0x400A8000u
#define SECGPIO_SEC_BASE 0x500A8000u
#define GPIO_SIZE      0x2490u

#define GPIO_PORT_COUNT 2u
#define GPIO_PINS_PER_PORT 32u

#define GPIO_OFF_BYTE   0x000u
#define GPIO_OFF_WORD   0x080u
#define GPIO_OFF_DIR    0x2000u
#define GPIO_OFF_MASK   0x2080u
#define GPIO_OFF_PIN    0x2100u
#define GPIO_OFF_MPIN   0x2180u
#define GPIO_OFF_SET    0x2200u
#define GPIO_OFF_CLR    0x2280u
#define GPIO_OFF_NOT    0x2300u
#define GPIO_OFF_DIRSET 0x2380u
#define GPIO_OFF_DIRCLR 0x2400u
#define GPIO_OFF_DIRNOT 0x2480u

struct gpio_state {
    mm_u32 dir[GPIO_PORT_COUNT];
    mm_u32 pin[GPIO_PORT_COUNT];
    mm_u32 mask[GPIO_PORT_COUNT];
};

static struct gpio_state gpio;

static mm_bool gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    struct gpio_state *g = (struct gpio_state *)opaque;
    mm_u32 port;
    if (g == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > GPIO_SIZE)
        return MM_FALSE;

    *value_out = 0;

    /* BYTE registers: byte-wide per-pin access */
    if (offset < GPIO_OFF_WORD) {
        mm_u32 pin_idx = offset;
        mm_u32 bit;
        if (pin_idx < (GPIO_PORT_COUNT * GPIO_PINS_PER_PORT)) {
            port = pin_idx / GPIO_PINS_PER_PORT;
            bit  = pin_idx % GPIO_PINS_PER_PORT;
            *value_out = (g->pin[port] >> bit) & 0x1u;
        }
        return MM_TRUE;
    }

    /* WORD registers: word-wide per-pin (bit 0) */
    if (offset >= GPIO_OFF_WORD && offset < GPIO_OFF_DIR) {
        mm_u32 pin_idx = (offset - GPIO_OFF_WORD) / 4u;
        mm_u32 bit;
        if (pin_idx < (GPIO_PORT_COUNT * GPIO_PINS_PER_PORT)) {
            port = pin_idx / GPIO_PINS_PER_PORT;
            bit  = pin_idx % GPIO_PINS_PER_PORT;
            *value_out = (g->pin[port] >> bit) & 0x1u;
        }
        return MM_TRUE;
    }

    /* Port-indexed registers */
    if (size_bytes == 4u) {
        if (offset >= GPIO_OFF_DIR && offset < GPIO_OFF_DIR + GPIO_PORT_COUNT *
            4u) {
            port = (offset - GPIO_OFF_DIR) / 4u;
            *value_out = g->dir[port];
            return MM_TRUE;
        }
        if (offset >= GPIO_OFF_MASK && offset < GPIO_OFF_MASK + GPIO_PORT_COUNT
            * 4u) {
            port = (offset - GPIO_OFF_MASK) / 4u;
            *value_out = g->mask[port];
            return MM_TRUE;
        }
        if (offset >= GPIO_OFF_PIN && offset < GPIO_OFF_PIN + GPIO_PORT_COUNT *
            4u) {
            port = (offset - GPIO_OFF_PIN) / 4u;
            *value_out = g->pin[port];
            return MM_TRUE;
        }
        if (offset >= GPIO_OFF_MPIN && offset < GPIO_OFF_MPIN + GPIO_PORT_COUNT
            * 4u) {
            port = (offset - GPIO_OFF_MPIN) / 4u;
            *value_out = g->pin[port] & ~g->mask[port];
            return MM_TRUE;
        }
        /* SET/CLR/NOT/DIRSET/DIRCLR/DIRNOT are write-only; reads return 0 */
    }

    return MM_TRUE;
}

static mm_bool gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct gpio_state *g = (struct gpio_state *)opaque;
    mm_u32 port;
    if (g == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > GPIO_SIZE)
        return MM_FALSE;

    /* BYTE registers */
    if (offset < GPIO_OFF_WORD) {
        mm_u32 pin_idx = offset;
        mm_u32 bit;
        if (pin_idx < (GPIO_PORT_COUNT * GPIO_PINS_PER_PORT)) {
            port = pin_idx / GPIO_PINS_PER_PORT;
            bit  = pin_idx % GPIO_PINS_PER_PORT;
            if (value & 0x1u)
                g->pin[port] |= (1u << bit);
            else
                g->pin[port] &= ~(1u << bit);
        }
        return MM_TRUE;
    }

    /* WORD registers */
    if (offset >= GPIO_OFF_WORD && offset < GPIO_OFF_DIR) {
        mm_u32 pin_idx = (offset - GPIO_OFF_WORD) / 4u;
        mm_u32 bit;
        if (pin_idx < (GPIO_PORT_COUNT * GPIO_PINS_PER_PORT)) {
            port = pin_idx / GPIO_PINS_PER_PORT;
            bit  = pin_idx % GPIO_PINS_PER_PORT;
            if (value & 0x1u)
                g->pin[port] |= (1u << bit);
            else
                g->pin[port] &= ~(1u << bit);
        }
        return MM_TRUE;
    }

    if (size_bytes != 4u)
        return MM_TRUE;

    if (offset >= GPIO_OFF_DIR && offset < GPIO_OFF_DIR + GPIO_PORT_COUNT * 4u)
    {
        port = (offset - GPIO_OFF_DIR) / 4u;
        g->dir[port] = value;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_MASK && offset < GPIO_OFF_MASK + GPIO_PORT_COUNT * 4u
        ) {
        port = (offset - GPIO_OFF_MASK) / 4u;
        g->mask[port] = value;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_PIN && offset < GPIO_OFF_PIN + GPIO_PORT_COUNT * 4u)
    {
        port = (offset - GPIO_OFF_PIN) / 4u;
        g->pin[port] = value;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_MPIN && offset < GPIO_OFF_MPIN + GPIO_PORT_COUNT * 4u
        ) {
        port = (offset - GPIO_OFF_MPIN) / 4u;
        g->pin[port] = (g->pin[port] & g->mask[port]) | (value & ~g->mask[port])
        ;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_SET && offset < GPIO_OFF_SET + GPIO_PORT_COUNT * 4u)
    {
        port = (offset - GPIO_OFF_SET) / 4u;
        g->pin[port] |= value;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_CLR && offset < GPIO_OFF_CLR + GPIO_PORT_COUNT * 4u)
    {
        port = (offset - GPIO_OFF_CLR) / 4u;
        g->pin[port] &= ~value;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_NOT && offset < GPIO_OFF_NOT + GPIO_PORT_COUNT * 4u)
    {
        port = (offset - GPIO_OFF_NOT) / 4u;
        g->pin[port] ^= value;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_DIRSET && offset < GPIO_OFF_DIRSET + GPIO_PORT_COUNT
        * 4u) {
        port = (offset - GPIO_OFF_DIRSET) / 4u;
        g->dir[port] |= value;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_DIRCLR && offset < GPIO_OFF_DIRCLR + GPIO_PORT_COUNT
        * 4u) {
        port = (offset - GPIO_OFF_DIRCLR) / 4u;
        g->dir[port] &= ~value;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_DIRNOT && offset < GPIO_OFF_DIRNOT + GPIO_PORT_COUNT
        * 4u) {
        port = (offset - GPIO_OFF_DIRNOT) / 4u;
        g->dir[port] ^= value;
        return MM_TRUE;
    }

    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * AHB_SECURE_CTRL  (0x400AC000 NS, 0x500AC000 S)
 *
 * Controls per-block security attributes for SRAM banks 0-4 (and flash/ROM).
 * MEM_RULE registers use 4-bit stride, 2-bit field per block:
 *   bits [1:0]  = RULE0, bits [5:4] = RULE1, ... bits [29:28] = RULE7
 *   Security bit = bit 1 of each field: 0 = non-secure, 1 = secure
 *
 * Reset value = 0x0 per SVD → all blocks non-secure at reset.
 * Firmware configures these registers during TrustZone initialisation.
 *
 * RAM bank → MEM_RULE register offsets (8 blocks per register):
 *   RAM0  (0x20000000, 64KB, 16 blocks): 0x60, 0x64
 *   RAM1  (0x20010000, 64KB, 16 blocks): 0x80, 0x84
 *   RAM2  (0x20020000, 64KB, 16 blocks): 0xA0, 0xA4
 *   RAM3  (0x20030000, 64KB, 16 blocks): 0xC0, 0xC4
 *   RAM4  (0x20040000, 16KB,  4 blocks): 0xE0
 *
 * Contiguous region (mpcbb_index=0) maps block_index linearly:
 *   blocks   0-15 → RAM0,  16-31 → RAM1,  32-47 → RAM2,
 *   blocks  48-63 → RAM3,  64-67 → RAM4
 * ------------------------------------------------------------------------- */
#define AHBSC_BASE      0x400AC000u
#define AHBSC_SEC_BASE  0x500AC000u
#define AHBSC_SIZE      0x1000u

/* Byte offsets of the first MEM_RULE register for each RAM bank */
static const mm_u32 ahbsc_ram_rule_base[5] = {
    0x60u,   /* RAM0 */
    0x80u,   /* RAM1 */
    0xA0u,   /* RAM2 */
    0xC0u,   /* RAM3 */
    0xE0u,   /* RAM4 */
};

static mm_u32 ahbsc_regs[AHBSC_SIZE / 4u];

static mm_bool ahbsc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > AHBSC_SIZE)
        return MM_FALSE;
    memcpy(value_out, (mm_u8 *)ahbsc_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool ahbsc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                           mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > AHBSC_SIZE)
        return MM_FALSE;
    memcpy((mm_u8 *)ahbsc_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * FLASH controller  (0x40034000 NS, 0x50034000 S)
 *
 * Register layout (subset implemented):
 *   CMD        @ 0x000  write to launch command
 *   EVENT      @ 0x004  w1c event flags
 *   STARTA     @ 0x010  start address in 16-byte (ECC word) units
 *   STOPA      @ 0x014  stop address in 16-byte (ECC word) units
 *   DATAW[0-3] @ 0x080-0x08C  read/write data words
 *   INT_CLR_ENABLE @ 0xFD8
 *   INT_SET_ENABLE @ 0xFDC
 *   INT_STATUS     @ 0xFE0  bit0=FAIL bit1=ERR bit2=DONE bit3=ECC_ERR
 *   INT_ENABLE     @ 0xFE4
 *   INT_CLR_STATUS @ 0xFE8  w1c clears INT_STATUS
 *   INT_SET_STATUS @ 0xFEC  w1s sets INT_STATUS
 *   MODULE_ID      @ 0xFFC  read-only 0xC40F0800
 *
 * Flash commands (CMD field):
 *   4  Erase page    (512-byte aligned, STARTA = byte_addr / 16)
 *   5  Blank check   (STARTA..STOPA range, sets INT_STATUS)
 *   8  Write phrase  (16 bytes from DATAW[0-3] to STARTA * 16)
 * ------------------------------------------------------------------------- */
#define FLASH_CTRL_BASE     0x40034000u
#define FLASH_CTRL_SEC_BASE 0x50034000u
#define FLASH_CTRL_SIZE     0x1000u

#define FC_OFF_CMD          0x000u
#define FC_OFF_EVENT        0x004u
#define FC_OFF_STARTA       0x010u
#define FC_OFF_STOPA        0x014u
#define FC_OFF_DATAW0       0x080u
#define FC_OFF_DATAW3       0x08Cu
#define FC_OFF_INT_CLR_EN   0xFD8u
#define FC_OFF_INT_SET_EN   0xFDCu
#define FC_OFF_INT_STATUS   0xFE0u
#define FC_OFF_INT_ENABLE   0xFE4u
#define FC_OFF_INT_CLR_ST   0xFE8u
#define FC_OFF_INT_SET_ST   0xFECu
#define FC_OFF_MODULE_ID    0xFFCu

#define FC_INT_FAIL         (1u << 0)
#define FC_INT_ERR          (1u << 1)
#define FC_INT_DONE         (1u << 2)
#define FC_INT_ECC_ERR      (1u << 3)

#define FC_CMD_ERASE_PAGE   4u
#define FC_CMD_BLANK_CHECK  5u
#define FC_CMD_WRITE_PHRASE 8u

/* ECC word = 16 bytes = 4 DATAW registers */
#define FC_ECC_WORD_BYTES   16u
/* LPC55S69: 640 KB = 655360 bytes → 40960 ECC words → 5120 bitmap bytes */
#define FLASH_ECC_BITMAP_BYTES (LPC55S69_FLASH_SIZE / FC_ECC_WORD_BYTES / 8u)

static mm_u32 flash_ctrl_regs[FLASH_CTRL_SIZE / 4u];

/* One bit per ECC word; 1 = blank/erased (reads cause bus fault) */
static mm_u8 flash_blank_bits[FLASH_ECC_BITMAP_BYTES];
/* Pointer to the flash buffer (set by flash_bind) */
static mm_u8 *flash_buf = 0;

/* ---- Blank bitmap helpers ---- */

static void flash_blank_set(mm_u32 word_idx)
{
    flash_blank_bits[word_idx / 8u] |= (mm_u8)(1u << (word_idx % 8u));
}

static void flash_blank_clear(mm_u32 word_idx)
{
    flash_blank_bits[word_idx / 8u] &= (mm_u8) ~(1u << (word_idx % 8u));
}

static mm_bool flash_blank_get(mm_u32 word_idx)
{
    return ((flash_blank_bits[word_idx / 8u] >> (word_idx % 8u)) & 1u) != 0u
           ? MM_TRUE : MM_FALSE;
}

/* Return the ECC word index for a byte offset within the flash buffer. */
static mm_u32 flash_word_idx(mm_u32 byte_offset)
{
    return byte_offset / FC_ECC_WORD_BYTES;
}

/* ---- Exported: mark flash range blank (erased) or programmed ---- */

void mm_lpc55s69_flash_mark_blank(mm_u32 offset, mm_u32 len)
{
    mm_u32 first = flash_word_idx(offset);
    mm_u32 last  = flash_word_idx((offset + len - 1u > offset) ? offset + len -
                                  1u : offset);
    mm_u32 i;
    for (i = first; i <= last; ++i) {
        if (i < (LPC55S69_FLASH_SIZE / FC_ECC_WORD_BYTES)) {
            flash_blank_set(i);
        }
    }
}

void mm_lpc55s69_flash_mark_programmed(mm_u32 offset, mm_u32 len)
{
    mm_u32 first = flash_word_idx(offset);
    mm_u32 last  = flash_word_idx((offset + len - 1u > offset) ? offset + len -
                                  1u : offset);
    mm_u32 i;
    for (i = first; i <= last; ++i) {
        if (i < (LPC55S69_FLASH_SIZE / FC_ECC_WORD_BYTES)) {
            flash_blank_clear(i);
        }
    }
}

/* ---- Flash controller CMD execution ---- */

static void flash_ctrl_exec_cmd(mm_u32 cmd)
{
    mm_u32 starta = flash_ctrl_regs[FC_OFF_STARTA / 4u] & 0x3FFFFu;
    mm_u32 stopa  = flash_ctrl_regs[FC_OFF_STOPA  / 4u] & 0x3FFFFu;
    mm_u32 flash_size_words = LPC55S69_FLASH_SIZE / FC_ECC_WORD_BYTES;
    mm_u32 status = FC_INT_DONE;
    mm_u32 i;

    if (starta >= flash_size_words) {
        flash_ctrl_regs[FC_OFF_INT_STATUS / 4u] |= FC_INT_DONE | FC_INT_FAIL;
        return;
    }
    if (stopa < starta)
        stopa = starta;
    if (stopa >= flash_size_words)
        stopa = flash_size_words - 1u;

    switch (cmd) {
    case FC_CMD_BLANK_CHECK:
        for (i = starta; i <= stopa; ++i) {
            if (!flash_blank_get(i)) {
                /* At least one word is not blank → FAIL */
                status |= FC_INT_FAIL;
                break;
            }
        }
        break;

    case FC_CMD_ERASE_PAGE: {
        /* Page = 512 bytes = 32 ECC words; starta is the start word index.
         * Erase the page that contains starta (round down to 32-word boundary). */
        mm_u32 page_start = (starta / 32u) * 32u;
        mm_u32 page_end   = page_start + 31u;
        if (flash_buf != 0) {
            memset(flash_buf + page_start * FC_ECC_WORD_BYTES, 0xFFu,
                   32u * FC_ECC_WORD_BYTES);
        }
        for (i = page_start; i <= page_end && i < flash_size_words; ++i) {
            flash_blank_set(i);
        }
        break;
    }

    case FC_CMD_WRITE_PHRASE:
        /* Write 16 bytes from DATAW[0-3] to STARTA * 16 in flash buffer. */
        if (flash_buf != 0 && starta < flash_size_words) {
            mm_u32 byte_off = starta * FC_ECC_WORD_BYTES;
            mm_u32 w;
            for (w = 0; w < 4u; ++w) {
                mm_u32 val = flash_ctrl_regs[(FC_OFF_DATAW0 / 4u) + w];
                flash_buf[byte_off + w * 4u + 0u] = (mm_u8)(val & 0xffu);
                flash_buf[byte_off + w * 4u + 1u] = (mm_u8)((val >> 8) & 0xffu);
                flash_buf[byte_off + w * 4u + 2u] = (mm_u8)((val >> 16) & 0xffu)
                ;
                flash_buf[byte_off + w * 4u + 3u] = (mm_u8)((val >> 24) & 0xffu)
                ;
            }
            flash_blank_clear(starta);
        }
        break;

    default:
        /* Unknown command — complete without error */
        break;
    }

    flash_ctrl_regs[FC_OFF_INT_STATUS / 4u] |= status;
}

static mm_bool flash_ctrl_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                               mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > FLASH_CTRL_SIZE)
        return MM_FALSE;
    /* MODULE_ID is read-only */
    if (offset == FC_OFF_MODULE_ID && size_bytes == 4u) {
        *value_out = 0xC40F0800u;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)flash_ctrl_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool flash_ctrl_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                                mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > FLASH_CTRL_SIZE)
        return MM_FALSE;
    /* MODULE_ID is read-only */
    if (offset == FC_OFF_MODULE_ID)
        return MM_TRUE;
    /* INT_CLR_STATUS: write 1 to clear INT_STATUS bits */
    if (offset == FC_OFF_INT_CLR_ST && size_bytes == 4u) {
        flash_ctrl_regs[FC_OFF_INT_STATUS / 4u] &= ~value;
        return MM_TRUE;
    }
    /* INT_SET_STATUS: write 1 to set INT_STATUS bits */
    if (offset == FC_OFF_INT_SET_ST && size_bytes == 4u) {
        flash_ctrl_regs[FC_OFF_INT_STATUS / 4u] |= value;
        return MM_TRUE;
    }
    /* Write register, then execute if CMD */
    memcpy((mm_u8 *)flash_ctrl_regs + offset, &value, size_bytes);
    if (offset == FC_OFF_CMD && size_bytes == 4u) {
        flash_ctrl_exec_cmd(value);
    }
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * PMC  (0x40020000 NS, 0x50020000 S)
 * Stub: tracks writes, returns stored values on read.
 * ------------------------------------------------------------------------- */
#define PMC_BASE      0x40020000u
#define PMC_SEC_BASE  0x50020000u
#define PMC_SIZE      0x100u

static mm_u32 pmc_regs[PMC_SIZE / 4u];

static mm_bool pmc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > PMC_SIZE)
        return MM_FALSE;
    memcpy(value_out, (mm_u8 *)pmc_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool pmc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > PMC_SIZE)
        return MM_FALSE;
    /* PDRUNCFGSET0 @ 0xBC / PDRUNCFGCLR0 @ 0xC0 act on PDRUNCFG0 @ 0xB8
     * (the fsl_rng driver clears PDEN_RNG via the CLR register). */
    if (size_bytes == 4u && offset == 0xBCu) {
        pmc_regs[0xB8u / 4u] |= value;
        return MM_TRUE;
    }
    if (size_bytes == 4u && offset == 0xC0u) {
        pmc_regs[0xB8u / 4u] &= ~value;
        return MM_TRUE;
    }
    memcpy((mm_u8 *)pmc_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * Generic pass-through stub for peripherals that need no active emulation.
 * Uses the opaque pointer to carry (size << 16 | ptr-identity); actual data
 * lives in a static array whose address is passed as opaque.
 * We embed the byte-size as the first word of the array, then the registers
 * follow — but that complicates array sizing.  Instead we use a lightweight
 * two-word wrapper.
 * ------------------------------------------------------------------------- */
struct periph_stub {
    mm_u32 size;       /* byte-size of the register space   */
    mm_u32 *regs;      /* pointer to the backing word array */
};

static mm_bool stub_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    const struct periph_stub *ps = (const struct periph_stub *)opaque;
    if (ps == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > ps->size)
        return MM_FALSE;
    memcpy(value_out, (const mm_u8 *)ps->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool stub_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct periph_stub *ps = (struct periph_stub *)opaque;
    if (ps == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > ps->size)
        return MM_FALSE;
    memcpy((mm_u8 *)ps->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/*
 * Declare a stub peripheral: static reg array + periph_stub descriptor.
 * NAME  – C identifier prefix
 * BYTES – byte-size of the register window (must be a multiple of 4)
 */
#define DECL_STUB(NAME, BYTES)                                  \
    static mm_u32 NAME##_regs[(BYTES) / 4u];                   \
    static struct periph_stub NAME##_stub = {                   \
        (BYTES), NAME##_regs                                    \
    }

/* Helper to register a stub at NS_BASE and S_BASE (S = NS + 0x10000000) */
static mm_bool stub_reg_pair(struct mmio_bus *bus,
                             struct periph_stub *ps,
                             mm_u32 ns_base)
{
    struct mmio_region reg;
    reg.size   = ps->size;
    reg.opaque = ps;
    reg.read   = stub_read;
    reg.write  = stub_write;
    reg.base   = ns_base;
    if (!mmio_bus_register_region(bus, &reg))
        return MM_FALSE;
    reg.base   = ns_base + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg))
        return MM_FALSE;
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * GINT0  (0x40002000) — Group GPIO Interrupt
 * ------------------------------------------------------------------------- */
DECL_STUB(gint0, 0x48u);

/* -------------------------------------------------------------------------
 * PINT  (0x40004000) — Pin Interrupt / Pattern Match
 * ------------------------------------------------------------------------- */
DECL_STUB(pint, 0x34u);

/* -------------------------------------------------------------------------
 * INPUTMUX  (0x40006000) — Input Multiplexer
 * ------------------------------------------------------------------------- */
DECL_STUB(inputmux, 0x7C0u);   /* round 0x7B4 up to 4-byte boundary */

/* -------------------------------------------------------------------------
 * CTIMER0-4  — Counter/Timer (each 0x88 bytes)
 * 0x40008000 / 0x40009000 / 0x40028000 / 0x40029000 / 0x4002A000
 * ------------------------------------------------------------------------- */
DECL_STUB(ctimer0, 0x88u);
DECL_STUB(ctimer1, 0x88u);
DECL_STUB(ctimer2, 0x88u);
DECL_STUB(ctimer3, 0x88u);
DECL_STUB(ctimer4, 0x88u);

/* -------------------------------------------------------------------------
 * WWDT  (0x4000C000) — Windowed Watchdog Timer  (size 0x1C)
 * Reset values per SVD: TC=0xFF, TV=0xFF, WINDOW=0xFFFFFF
 * ------------------------------------------------------------------------- */
DECL_STUB(wwdt, 0x1Cu);
#define WWDT_OFF_TC     0x04u
#define WWDT_OFF_TV     0x0Cu
#define WWDT_OFF_WINDOW 0x18u

/* -------------------------------------------------------------------------
 * UTICK0  (0x4000E000) — Micro-tick Timer  (size 0x20)
 * ------------------------------------------------------------------------- */
DECL_STUB(utick0, 0x20u);

/* -------------------------------------------------------------------------
 * ANACTRL  (0x40013000) — Analog Control  (size 0x104)
 * Several read-only status registers reflect "hardware ready" at reset.
 * ------------------------------------------------------------------------- */
DECL_STUB(anactrl, 0x104u);
#define ANACTRL_ANALOG_CTRL_STATUS 0x04u  /* reset 0x50000000 — VDD/VBAT OK  */
#define ANACTRL_FRO192M_CTRL       0x10u  /* reset 0x80D01A                   */
#define ANACTRL_FRO192M_STATUS     0x14u  /* reset 0x3 — FRO on and stable    */
#define ANACTRL_XO32M_CTRL         0x20u  /* reset 0x21428A                   */
#define ANACTRL_BOD_DCDC_INT_STATUS 0x34u /* reset 0x104                      */
#define ANACTRL_RINGO0_CTRL        0x40u  /* reset 0x40                       */
#define ANACTRL_RINGO1_CTRL        0x44u  /* reset 0x40                       */
#define ANACTRL_RINGO2_CTRL        0x48u  /* reset 0x40                       */
#define ANACTRL_USBHS_PHY_CTRL     0x100u /* reset 0x8                        */

/* -------------------------------------------------------------------------
 * SYSCTL  (0x40023000) — System Controller  (size 0x104)
 * ------------------------------------------------------------------------- */
DECL_STUB(sysctl, 0x104u);

/* -------------------------------------------------------------------------
 * RTC  (0x4002C000) — Real-Time Clock  (size 0x60)
 * ------------------------------------------------------------------------- */
DECL_STUB(rtc, 0x60u);

/* -------------------------------------------------------------------------
 * OSTIMER  (0x4002D000) — OS Event Timer  (size 0x20)
 * ------------------------------------------------------------------------- */
DECL_STUB(ostimer, 0x20u);

/* -------------------------------------------------------------------------
 * PRINCE  (0x40035000) — PRINCE on-the-fly decryption  (size 0x40)
 * ------------------------------------------------------------------------- */
DECL_STUB(prince, 0x40u);

/* -------------------------------------------------------------------------
 * USBPHY  (0x40038000) — USB Physical Layer  (size 0x110)
 * ------------------------------------------------------------------------- */
DECL_STUB(usbphy, 0x110u);

/* -------------------------------------------------------------------------
 * RNG  (0x4003A000) — True Random Number Generator  (size 0x1000)
 * Functional model in lpc55s69_rng.c: RANDOM_NUMBER draws from the
 * replayable host RNG stream; chi-squared online-test registers follow
 * the fsl_rng driver sequence.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * PUF  (0x4003B000) — Physical Unclonable Function  (size 0x260)
 * Functional model in lpc55s69_puf.c: Enroll/Start/GenerateKey/SetKey/
 * GetKey/Zeroize per the fsl_puf driver contract.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * PLU  (0x4003D000) — Programmable Logic Unit  (size 0xC20)
 * ------------------------------------------------------------------------- */
DECL_STUB(plu, 0xC20u);

/* -------------------------------------------------------------------------
 * DMA0  (0x40082000) — DMA Controller 0  (size 0x56C)
 * DMA1  (0x400A7000) — DMA Controller 1  (size 0x49C)
 * ------------------------------------------------------------------------- */
DECL_STUB(dma0, 0x56Cu);
DECL_STUB(dma1, 0x49Cu);

/* -------------------------------------------------------------------------
 * USB0  (0x40084000) — USB Full-Speed Device controller  (size 0x38)
 * USBHSD (0x40094000) — USB High-Speed Device controller (size 0x38)
 * USBFSH (0x400A2000) — USB Full-Speed Host              (size 0x60)
 * USBHSH (0x400A3000) — USB High-Speed Host              (size 0x54)
 * ------------------------------------------------------------------------- */
DECL_STUB(usb0, 0x38u);
DECL_STUB(usbhsd, 0x38u);
DECL_STUB(usbfsh, 0x60u);
DECL_STUB(usbhsh, 0x54u);

/* -------------------------------------------------------------------------
 * SCT0  (0x40085000) — State Configurable Timer/PWM  (size 0x550)
 * ------------------------------------------------------------------------- */
DECL_STUB(sct0, 0x550u);

/* -------------------------------------------------------------------------
 * MAILBOX  (0x4008B000) — Cortex-M33/M4 Mailbox  (size 0xFC)
 * ------------------------------------------------------------------------- */
DECL_STUB(mailbox, 0xFCu);

/* -------------------------------------------------------------------------
 * CRC_ENGINE  (0x40095000) — CRC Engine  (size 0xC)
 *
 * Registers (all at base + offset):
 *   [0x0] MODE  — polynomial selector + bit-reverse/complement flags
 *   [0x4] SEED  — writing resets the accumulator to this value
 *   [0x8] SUM   — reading returns current (post-processed) CRC
 *   [0x8] WR_DATA — writing feeds a byte/word through the CRC engine
 *
 * MODE bits:
 *   [1:0] CRC_POLY: 0=CRC-CCITT(0x1021,16b), 1=CRC-16(0x8005,16b), 2/3=CRC-32(0x04C11DB7,32b)
 *   [2]   BIT_RVS_WR: bit-reverse each input byte
 *   [3]   CMPL_WR: 1's-complement each input byte
 *   [4]   BIT_RVS_SUM: bit-reverse result
 *   [5]   CMPL_SUM: 1's-complement result
 * ------------------------------------------------------------------------- */
#define CRC_OFF_MODE   0x0u
#define CRC_OFF_SEED   0x4u
#define CRC_OFF_SUM    0x8u   /* read */
#define CRC_OFF_WRDATA 0x8u   /* write */

#define CRC_MODE_POLY_MASK    0x3u
#define CRC_MODE_BIT_RVS_WR  (1u << 2)
#define CRC_MODE_CMPL_WR     (1u << 3)
#define CRC_MODE_BIT_RVS_SUM (1u << 4)
#define CRC_MODE_CMPL_SUM    (1u << 5)

static mm_u32 crc_mode;
static mm_u32 crc_accum;  /* running accumulator */

static mm_u8 crc_bitrev8(mm_u8 b)
{
    b = (mm_u8)(((b >> 1u) & 0x55u) | ((b & 0x55u) << 1u));
    b = (mm_u8)(((b >> 2u) & 0x33u) | ((b & 0x33u) << 2u));
    b = (mm_u8)((b >> 4u) | (b << 4u));
    return b;
}

static mm_u32 crc_bitrev16(mm_u32 v)
{
    mm_u32 r = 0u, i;
    for (i = 0u; i < 16u; ++i) {
        r = (r << 1u) | (v & 1u); v >>= 1u;
    }
    return r;
}

static mm_u32 crc_bitrev32(mm_u32 v)
{
    mm_u32 r = 0u, i;
    for (i = 0u; i < 32u; ++i) {
        r = (r << 1u) | (v & 1u); v >>= 1u;
    }
    return r;
}

static void crc_feed_byte(mm_u8 b)
{
    mm_u32 poly, i;
    if (crc_mode & CRC_MODE_BIT_RVS_WR)
        b = crc_bitrev8(b);
    if (crc_mode & CRC_MODE_CMPL_WR)
        b = (mm_u8)(~b);
    switch (crc_mode & CRC_MODE_POLY_MASK) {
    case 0u: /* CRC-CCITT: 0x1021, 16-bit */
        poly = 0x1021u;
        crc_accum ^= ((mm_u32)b << 8u);
        for (i = 0u; i < 8u; ++i) {
            if (crc_accum & 0x8000u)
                crc_accum = ((crc_accum << 1u) ^ poly) & 0xFFFFu;
            else
                crc_accum = (crc_accum << 1u) & 0xFFFFu;
        }
        break;
    case 1u: /* CRC-16: 0x8005, 16-bit */
        poly = 0x8005u;
        crc_accum ^= ((mm_u32)b << 8u);
        for (i = 0u; i < 8u; ++i) {
            if (crc_accum & 0x8000u)
                crc_accum = ((crc_accum << 1u) ^ poly) & 0xFFFFu;
            else
                crc_accum = (crc_accum << 1u) & 0xFFFFu;
        }
        break;
    default: /* CRC-32: 0x04C11DB7, 32-bit */
        poly = 0x04C11DB7u;
        crc_accum ^= ((mm_u32)b << 24u);
        for (i = 0u; i < 8u; ++i) {
            if (crc_accum & 0x80000000u)
                crc_accum = (crc_accum << 1u) ^ poly;
            else
                crc_accum = (crc_accum << 1u);
        }
        break;
    }
}

static mm_u32 crc_read_sum(void)
{
    mm_u32 v = crc_accum;
    int is32 = ((crc_mode & CRC_MODE_POLY_MASK) >= 2u);
    if (crc_mode & CRC_MODE_BIT_RVS_SUM)
        v = is32 ? crc_bitrev32(v) : crc_bitrev16(v);
    if (crc_mode & CRC_MODE_CMPL_SUM)
        v = is32 ? (~v) : ((~v) & 0xFFFFu);
    return v;
}

static mm_bool crc_engine_read(void *opaque, mm_u32 offset, mm_u32 size,
                               mm_u32 *value_out)
{
    (void)opaque;
    if (size == 0u || size > 4u || value_out == 0)
        return MM_FALSE;
    switch (offset) {
    case CRC_OFF_MODE: *value_out = crc_mode;     return MM_TRUE;
    case CRC_OFF_SEED: *value_out = crc_accum;    return MM_TRUE;
    case CRC_OFF_SUM:  *value_out = crc_read_sum(); return MM_TRUE;
    default: *value_out = 0u; return MM_TRUE;
    }
}

static mm_bool crc_engine_write(void *opaque, mm_u32 offset, mm_u32 size,
                                mm_u32 value)
{
    (void)opaque;
    switch (offset) {
    case CRC_OFF_MODE:
        crc_mode = value & 0x3Fu;
        return MM_TRUE;
    case CRC_OFF_SEED:
        crc_accum = value;
        return MM_TRUE;
    case CRC_OFF_WRDATA: {
        mm_u32 i, nbytes = (size < 4u) ? size : 4u;
        for (i = 0u; i < nbytes; ++i) {
            crc_feed_byte((mm_u8)((value >> (8u * i)) & 0xFFu));
        }
        return MM_TRUE;
    }
    default: return MM_TRUE;
    }
}

static struct mmio_region crc_engine_ns_region;
static struct mmio_region crc_engine_s_region;

/* -------------------------------------------------------------------------
 * SDIF  (0x4009B000) — SD/MMC Interface  (size 0x300)
 * ------------------------------------------------------------------------- */
DECL_STUB(sdif, 0x300u);

/* -------------------------------------------------------------------------
 * DBGMAILBOX  (0x4009C000) — Debug Mailbox  (size 0x100)
 * ------------------------------------------------------------------------- */
DECL_STUB(dbgmailbox, 0x100u);

/* -------------------------------------------------------------------------
 * ADC0  (0x400A0000) — 16-bit Sigma-Delta ADC  (size 0x1000)
 * ------------------------------------------------------------------------- */
DECL_STUB(adc0, 0x1000u);

/* -------------------------------------------------------------------------
 * HASHCRYPT  (0x400A4000 NS, 0x500A4000 S) — Hash/AES-Crypt accelerator
 * Backed by mm_lpc55_hashcrypt; NVIC pointer wired in mm_lpc55s69_register_mmio.
 * ------------------------------------------------------------------------- */
static struct mm_lpc55_hashcrypt hashcrypt_state;
static struct mmio_region hashcrypt_ns_region;
static struct mmio_region hashcrypt_s_region;

/* -------------------------------------------------------------------------
 * CASPER  (0x400A5000 NS, 0x500A5000 S) — RSA/ECC accelerator (size 0x84)
 * Backed by mm_lpc55_casper; NVIC and memmap pointers wired at register time.
 * ------------------------------------------------------------------------- */
static struct mm_lpc55_casper casper_state;
static struct mmio_region casper_ns_region;
static struct mmio_region casper_s_region;

/* -------------------------------------------------------------------------
 * POWERQUAD  (0x400A6000) — DSP/Math accelerator  (size 0x260)
 * ------------------------------------------------------------------------- */
DECL_STUB(powerquad, 0x260u);

/* -------------------------------------------------------------------------
 * Public interface
 * ------------------------------------------------------------------------- */

/* PRESETCTRL offset = AHBCLKCTRL offset - 0x100 */
#define SYSCON_PRESETCTRL_BASE 0x100u
#define SYSCON_AHBCLKCTRL_BASE 0x200u

mm_bool mm_lpc55s69_syscon_periph_active(mm_u32 ahbclk_offset, mm_u32 bit)
{
    mm_u32 preset_offset;
    mm_u32 clk_reg;
    mm_u32 rst_reg;
    mm_u32 mask;

    if (ahbclk_offset < SYSCON_AHBCLKCTRL_BASE)
        return MM_FALSE;
    preset_offset = ahbclk_offset - 0x100u;  /* PRESETCTRLn at offset - 0x100 */
    if ((ahbclk_offset + 4u) > SYSCON_SIZE)
        return MM_FALSE;
    if ((preset_offset + 4u) > SYSCON_SIZE)
        return MM_FALSE;

    clk_reg = syscon.regs[ahbclk_offset / 4u];
    rst_reg = syscon.regs[preset_offset / 4u];
    mask = (1u << bit);
    /* Clock bit must be set; reset bit must be CLEAR (0 = released from reset). */
    return ((clk_reg & mask) != 0u && (rst_reg & mask) == 0u) ? MM_TRUE :
           MM_FALSE;
}

void mm_lpc55s69_mmio_reset(void)
{
    memset(&syscon, 0, sizeof(syscon));
    memset(&gpio, 0, sizeof(gpio));
    memset(iocon_regs, 0, sizeof(iocon_regs));
    memset(flash_ctrl_regs, 0, sizeof(flash_ctrl_regs));
    flash_ctrl_regs[FC_OFF_MODULE_ID / 4u] = 0xC40F0800u;
    memset(pmc_regs, 0, sizeof(pmc_regs));
    /* AHB_SECURE_CTRL: reset value 0 per SVD (all blocks non-secure) */
    memset(ahbsc_regs, 0, sizeof(ahbsc_regs));

    /* New peripheral stubs — clear all first, then set non-zero resets */
    memset(gint0_regs,       0, sizeof(gint0_regs));
    memset(pint_regs,        0, sizeof(pint_regs));
    memset(inputmux_regs,    0, sizeof(inputmux_regs));
    memset(ctimer0_regs,     0, sizeof(ctimer0_regs));
    memset(ctimer1_regs,     0, sizeof(ctimer1_regs));
    memset(ctimer2_regs,     0, sizeof(ctimer2_regs));
    memset(ctimer3_regs,     0, sizeof(ctimer3_regs));
    memset(ctimer4_regs,     0, sizeof(ctimer4_regs));
    memset(wwdt_regs,        0, sizeof(wwdt_regs));
    memset(utick0_regs,      0, sizeof(utick0_regs));
    memset(anactrl_regs,     0, sizeof(anactrl_regs));
    memset(sysctl_regs,      0, sizeof(sysctl_regs));
    memset(rtc_regs,         0, sizeof(rtc_regs));
    memset(ostimer_regs,     0, sizeof(ostimer_regs));
    memset(prince_regs,      0, sizeof(prince_regs));
    memset(usbphy_regs,      0, sizeof(usbphy_regs));
    memset(plu_regs,         0, sizeof(plu_regs));
    memset(dma0_regs,        0, sizeof(dma0_regs));
    memset(dma1_regs,        0, sizeof(dma1_regs));
    memset(usb0_regs,        0, sizeof(usb0_regs));
    memset(usbhsd_regs,      0, sizeof(usbhsd_regs));
    memset(usbfsh_regs,      0, sizeof(usbfsh_regs));
    memset(usbhsh_regs,      0, sizeof(usbhsh_regs));
    memset(sct0_regs,        0, sizeof(sct0_regs));
    memset(mailbox_regs,     0, sizeof(mailbox_regs));
    crc_mode  = 0u;
    crc_accum = 0u;
    memset(sdif_regs,        0, sizeof(sdif_regs));
    memset(dbgmailbox_regs,  0, sizeof(dbgmailbox_regs));
    memset(adc0_regs,        0, sizeof(adc0_regs));
    /* hashcrypt reset handled by mm_lpc55_hashcrypt_reset() below */
    /* casper reset handled by mm_lpc55_casper_reset() below */
    memset(powerquad_regs,   0, sizeof(powerquad_regs));

    /* WWDT non-zero resets */
    wwdt_regs[WWDT_OFF_TC     / 4u] = 0xFFu;
    wwdt_regs[WWDT_OFF_TV     / 4u] = 0xFFu;
    wwdt_regs[WWDT_OFF_WINDOW / 4u] = 0xFFFFFFu;

    /* ANACTRL: FRO and power-management status bits show hardware ready */
    anactrl_regs[ANACTRL_ANALOG_CTRL_STATUS  / 4u] = 0x50000000u;
    anactrl_regs[ANACTRL_FRO192M_CTRL        / 4u] = 0x0080D01Au;
    anactrl_regs[ANACTRL_FRO192M_STATUS      / 4u] = 0x3u;
    anactrl_regs[ANACTRL_XO32M_CTRL          / 4u] = 0x0021428Au;
    anactrl_regs[ANACTRL_BOD_DCDC_INT_STATUS / 4u] = 0x104u;
    anactrl_regs[ANACTRL_RINGO0_CTRL         / 4u] = 0x40u;
    anactrl_regs[ANACTRL_RINGO1_CTRL         / 4u] = 0x40u;
    anactrl_regs[ANACTRL_RINGO2_CTRL         / 4u] = 0x40u;
    anactrl_regs[ANACTRL_USBHS_PHY_CTRL      / 4u] = 0x8u;

    /* RNG + PUF functional models: reset handled by their own functions */
    mm_lpc55s69_rng_reset();
    mm_lpc55s69_puf_reset();

    /* PMC non-zero resets per SVD */
    pmc_regs[0x30u / 4u] = 0x47u;        /* BODVBAT */
    pmc_regs[0x50u / 4u] = 0xAu;         /* COMP    */
    pmc_regs[0x74u / 4u] = 0x6u;         /* STATUSCLK */
    pmc_regs[0x98u / 4u] = 0x3FF0008u;   /* RTCOSC32K */
    pmc_regs[0x9Cu / 4u] = 0x8u;         /* OSTIMER (PMC) */
    pmc_regs[0xB8u / 4u] = 0xDEFFC4u;    /* PDRUNCFG0 */

    /* SYSCON reset-state defaults per LPC55S69 SVD */
    syscon.regs[SYSCON_AHBCLKCTRL0 / 4u] = 0x00000180u;  /* FLASH + FMC */
    syscon.regs[SYSCON_AHBCLKCTRL1 / 4u] = 0x00000000u;
    syscon.regs[SYSCON_AHBCLKCTRL2 / 4u] = 0x00000000u;
    /* All resets asserted (0 = in reset) */
    syscon.regs[SYSCON_PRESETCTRL0 / 4u] = 0x00000000u;
    syscon.regs[SYSCON_PRESETCTRL1 / 4u] = 0x00000000u;
    syscon.regs[SYSCON_PRESETCTRL2 / 4u] = 0x00000000u;
    /* FRO12M as default main clock source */
    syscon.regs[SYSCON_MAINCLKSELA / 4u] = 0x0u;  /* FRO12M */
    syscon.regs[SYSCON_MAINCLKSELB / 4u] = 0x0u;  /* use MAINCLKSELA */
    /* SysTick clock selector reset = 7 (none / disabled) per SVD */
    syscon.regs[SYSCON_SYSTICKCLKSEL0 / 4u] = 0x7u;
    syscon.regs[SYSCON_SYSTICKCLKSEL1 / 4u] = 0x7u;
    /* CLOCK_CTRL: FRO1MHz enabled by default */
    syscon.regs[SYSCON_CLOCK_CTRL / 4u] = 0x1u;

    mm_lpc55s69_romapi_reset();
    mm_lpc55_hashcrypt_reset(&hashcrypt_state);
    mm_lpc55_casper_reset(&casper_state);
}

/*
 * Wire the NVIC into the HashCrypt peripheral model.
 * Called from mm_lpc55s69_timers_init (which already receives the NVIC).
 */
void mm_lpc55s69_set_nvic(struct mm_nvic *nvic)
{
    hashcrypt_state.nvic = nvic;
    casper_state.nvic    = nvic;
}

mm_bool mm_lpc55s69_casper_cp_active(void)
{
    /* Returns MM_TRUE whenever the CASPER instance has been initialised
     * (i.e. mm_lpc55s69_register_mmio has been called).  This is always
     * true for the LPC55S69 target and false for all other targets since
     * this function is never called for them. */
    return (mm_lpc55_casper_get_global() != NULL) ? MM_TRUE : MM_FALSE;
}

static mm_bool reg_pair(struct mmio_bus *bus, struct mmio_region *r,
                        mm_u32 ns_base, mm_u32 s_base)
{
    r->base = ns_base;
    if (!mmio_bus_register_region(bus, r))
        return MM_FALSE;
    r->base = s_base;
    if (!mmio_bus_register_region(bus, r))
        return MM_FALSE;
    return MM_TRUE;
}

mm_bool mm_lpc55s69_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0)
        return MM_FALSE;

    /* SYSCON */
    reg.size   = SYSCON_SIZE;
    reg.opaque = &syscon;
    reg.read   = syscon_read;
    reg.write  = syscon_write;
    if (!reg_pair(bus, &reg, SYSCON_BASE, SYSCON_SEC_BASE))
        return MM_FALSE;

    /* IOCON */
    reg.size   = IOCON_SIZE;
    reg.opaque = iocon_regs;
    reg.read   = iocon_read;
    reg.write  = iocon_write;
    if (!reg_pair(bus, &reg, IOCON_BASE, IOCON_SEC_BASE))
        return MM_FALSE;

    /* GPIO */
    reg.size   = GPIO_SIZE;
    reg.opaque = &gpio;
    reg.read   = gpio_read;
    reg.write  = gpio_write;
    if (!reg_pair(bus, &reg, GPIO_BASE, GPIO_SEC_BASE))
        return MM_FALSE;

    /* SECGPIO (alias of GPIO for secure access) */
    reg.size   = GPIO_SIZE;
    reg.opaque = &gpio;
    reg.read   = gpio_read;
    reg.write  = gpio_write;
    if (!reg_pair(bus, &reg, SECGPIO_BASE, SECGPIO_SEC_BASE))
        return MM_FALSE;

    /* AHB_SECURE_CTRL */
    reg.size   = AHBSC_SIZE;
    reg.opaque = ahbsc_regs;
    reg.read   = ahbsc_read;
    reg.write  = ahbsc_write;
    if (!reg_pair(bus, &reg, AHBSC_BASE, AHBSC_SEC_BASE))
        return MM_FALSE;

    /* FLASH controller */
    reg.size   = FLASH_CTRL_SIZE;
    reg.opaque = flash_ctrl_regs;
    reg.read   = flash_ctrl_read;
    reg.write  = flash_ctrl_write;
    if (!reg_pair(bus, &reg, FLASH_CTRL_BASE, FLASH_CTRL_SEC_BASE))
        return MM_FALSE;

    /* PMC */
    reg.size   = PMC_SIZE;
    reg.opaque = pmc_regs;
    reg.read   = pmc_read;
    reg.write  = pmc_write;
    if (!reg_pair(bus, &reg, PMC_BASE, PMC_SEC_BASE))
        return MM_FALSE;

    /* --- Generic stubs (address order) --- */
    if (!stub_reg_pair(bus, &gint0_stub,     0x40002000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &pint_stub,      0x40004000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &inputmux_stub,  0x40006000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &ctimer0_stub,   0x40008000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &ctimer1_stub,   0x40009000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &wwdt_stub,      0x4000C000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &utick0_stub,    0x4000E000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &anactrl_stub,   0x40013000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &sysctl_stub,    0x40023000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &ctimer2_stub,   0x40028000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &ctimer3_stub,   0x40029000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &ctimer4_stub,   0x4002A000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &rtc_stub,       0x4002C000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &ostimer_stub,   0x4002D000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &prince_stub,    0x40035000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &usbphy_stub,    0x40038000u))
        return MM_FALSE;
    if (!mm_lpc55s69_rng_register(bus))
        return MM_FALSE;
    if (!mm_lpc55s69_puf_register(bus))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &plu_stub,       0x4003D000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &dma0_stub,      0x40082000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &usb0_stub,      0x40084000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &sct0_stub,      0x40085000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &mailbox_stub,   0x4008B000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &usbhsd_stub,    0x40094000u))
        return MM_FALSE;
    /* CRC_ENGINE — custom read/write handlers */
    crc_engine_ns_region.base   = 0x40095000u;
    crc_engine_ns_region.size   = 0xCu;
    crc_engine_ns_region.opaque = 0;
    crc_engine_ns_region.read   = crc_engine_read;
    crc_engine_ns_region.write  = crc_engine_write;
    if (!mmio_bus_register_region(bus, &crc_engine_ns_region))
        return MM_FALSE;
    crc_engine_s_region = crc_engine_ns_region;
    crc_engine_s_region.base    = 0x50095000u;
    if (!mmio_bus_register_region(bus, &crc_engine_s_region))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &sdif_stub,      0x4009B000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &dbgmailbox_stub, 0x4009C000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &adc0_stub,      0x400A0000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &usbfsh_stub,    0x400A2000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &usbhsh_stub,    0x400A3000u))
        return MM_FALSE;
    /* HASHCRYPT — proper peripheral model */
    mm_lpc55_hashcrypt_init(&hashcrypt_state, NULL); /* NVIC wired later by timers_init */
    hashcrypt_ns_region.base   = 0x400A4000u;
    hashcrypt_ns_region.size   = 0xE0u;
    hashcrypt_ns_region.opaque = &hashcrypt_state;
    hashcrypt_ns_region.read   = mm_lpc55_hashcrypt_read;
    hashcrypt_ns_region.write  = mm_lpc55_hashcrypt_write;
    hashcrypt_ns_region.name   = "hashcrypt";
    if (!mmio_bus_register_region(bus, &hashcrypt_ns_region))
        return MM_FALSE;
    hashcrypt_s_region = hashcrypt_ns_region;
    hashcrypt_s_region.base = 0x500A4000u;
    if (!mmio_bus_register_region(bus, &hashcrypt_s_region))
        return MM_FALSE;
    /* CASPER — proper peripheral model */
    mm_lpc55_casper_init(&casper_state, NULL, NULL); /* NVIC/memmap wired later */
    mm_lpc55_casper_set_global(&casper_state);
    casper_ns_region.base   = 0x400A5000u;
    casper_ns_region.size   = CASPER_SIZE;
    casper_ns_region.opaque = &casper_state;
    casper_ns_region.read   = mm_lpc55_casper_read;
    casper_ns_region.write  = mm_lpc55_casper_write;
    casper_ns_region.name   = "casper";
    if (!mmio_bus_register_region(bus, &casper_ns_region))
        return MM_FALSE;
    casper_s_region = casper_ns_region;
    casper_s_region.base = 0x500A5000u;
    if (!mmio_bus_register_region(bus, &casper_s_region))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &powerquad_stub, 0x400A6000u))
        return MM_FALSE;
    if (!stub_reg_pair(bus, &dma1_stub,      0x400A7000u))
        return MM_FALSE;

    /* ROM API — bootloader tree and stub execution region */
    if (!mm_lpc55s69_romapi_register_mmio(bus))
        return MM_FALSE;

    return MM_TRUE;
}

/*
 * TrustZone memory block security query — called by the core to determine
 * whether a RAM block is secure.
 *
 * bank:        mpcbb_index from mm_ram_region (0 for our single region)
 * block_index: (addr - region_base_s_or_ns) / LPC55S69_MPCBB_BLOCK_SIZE
 *
 * Our single contiguous region covers SRAM0-4 linearly:
 *   blocks   0-15 → SRAM0  (AHB_SECURE_CTRL offsets 0x60, 0x64)
 *   blocks  16-31 → SRAM1  (0x80, 0x84)
 *   blocks  32-47 → SRAM2  (0xA0, 0xA4)
 *   blocks  48-63 → SRAM3  (0xC0, 0xC4)
 *   blocks  64-67 → SRAM4  (0xE0)
 *
 * Each MEM_RULE register holds 8 blocks at 4-bit stride:
 *   security bit = bit 1 of field = (reg >> (block_in_reg * 4 + 1)) & 1
 */
mm_bool mm_lpc55s69_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    mm_u32 ram_bank;      /* which SRAM (0-4) */
    mm_u32 block_in_bank; /* block within that SRAM */
    mm_u32 rule_offset;   /* byte offset in ahbsc_regs of the rule register */
    mm_u32 block_in_reg;  /* which of the 8 rules within that register */
    mm_u32 reg_val;
    mm_u32 sec_bit;

    if (bank != 0)
        return MM_FALSE;

    /* 16 blocks per SRAM bank (except SRAM4 which has 4) */
    ram_bank      = block_index / 16u;
    block_in_bank = block_index % 16u;

    if (ram_bank > 4u)
        return MM_FALSE;
    /* SRAM4 has only 4 blocks (16 KB / 4 KB) */
    if (ram_bank == 4u && block_in_bank >= 4u)
        return MM_FALSE;

    rule_offset  = ahbsc_ram_rule_base[ram_bank] + (block_in_bank / 8u) * 4u;
    block_in_reg = block_in_bank % 8u;

    reg_val = ahbsc_regs[rule_offset / 4u];
    sec_bit = (reg_val >> (block_in_reg * 4u + 1u)) & 1u;
    return (sec_bit != 0u) ? MM_TRUE : MM_FALSE;
}

/* ---- ECC check callback installed into mm_memmap ---- */

static mm_bool lpc55s69_flash_ecc_check(void *opaque, mm_u32 byte_offset)
{
    mm_u32 word_idx = byte_offset / FC_ECC_WORD_BYTES;
    (void)opaque;
    if (word_idx >= (LPC55S69_FLASH_SIZE / FC_ECC_WORD_BYTES)) {
        return MM_FALSE;
    }
    /* blank → bus fault */
    return flash_blank_get(word_idx) ? MM_FALSE : MM_TRUE;
}

void mm_lpc55s69_flash_bind(struct mm_memmap *map,
                            mm_u8 *flash,
                            mm_u32 flash_size,
                            const struct mm_flash_persist *persist,
                            mm_u32 flags)
{
    mm_u32 n_words;
    mm_u32 i;
    mm_u32 j;
    mm_bool all_ff;

    (void)persist;
    (void)flags;

    flash_buf = flash;

    /* Wire the memory-map pointer into CASPER so it can read operands from RAM */
    casper_state.map = map;

    /*
     * Do NOT pre-mark factory-blank (0xFF) pages as ECC-faulting.
     * On real hardware, freshly-blank flash (factory state or after a proper
     * erase) returns 0xFF correctly; ECC faults only occur on inconsistent
     * (partially-written) words.  We track which words were explicitly erased
     * by the ROM API erase stub and only fault on those, matching the SDK
     * FLASH_VerifyErase() behaviour without breaking reads of unwritten flash.
     */
    memset(flash_blank_bits, 0, sizeof(flash_blank_bits));
    (void)n_words; (void)i; (void)j; (void)all_ff; (void)flash_size;

    mm_memmap_set_flash_ecc_check(map, lpc55s69_flash_ecc_check, 0);
}

mm_u64 mm_lpc55s69_cpu_hz(void)
{
    return 96000000ull; /* FRO96M default */
}
