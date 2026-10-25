/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include <stdio.h>
#include "pic32ck/pic32ck_mmio.h"
#include "pic32ck/cpu_config.h"
#include "m33mu/code_cache.h"
#include "m33mu/flash_persist.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/gpio_pads.h"

#define PIC32CK_FLASH_PAGE_SIZE     4096u
#define PIC32CK_FLASH_ROW_SIZE      1024u
#define PIC32CK_FLASH_QDW_SIZE      32u
#define PIC32CK_FLASH_SDW_SIZE      8u
#define PIC32CK_FLASH_GRANULARITY   4096u
#define PIC32CK_RAM_GRANULARITY     4096u
#define PIC32CK_ANSC_GRANULARITY    4096u

#define PIC32CK_IDAU_REGION_ANSC    0x09u
#define PIC32CK_IDAU_REGION_ANS     0x0Au
#define PIC32CK_IDAU_REGION_RS      0x0Du
#define PIC32CK_IDAU_REGION_RNS     0x0Eu
#define PIC32CK_IDAU_REGION_CFMS    0x07u

#define PIC32CK_FUSE_FUCFG14_OFF    0x78u
#define PIC32CK_FUSE_FUCFG15_OFF    0x7Cu
#define PIC32CK_FUSE_IDAU_ANS_MASK  0x000001FFu
#define PIC32CK_FUSE_IDAU_ANSC_MASK 0x00010000u
#define PIC32CK_FUSE_IDAU_RNS_MASK  0x0000007Fu

struct pic32ck_idau_state {
    mm_bool enabled;
    mm_bool write_locked;
    mm_u32 statusb;
    mm_u32 ans_size;
    mm_u32 ansc_size;
    mm_u32 rns_size;
};

static struct mm_memmap *g_map;
static const struct mm_flash_persist *g_persist;
static mm_u8 cfm_buf[PIC32CK_CFM_SIZE];
static struct pic32ck_idau_state g_idau;
static mm_u8 *g_flash_ptr;
static mm_u32 g_flash_size;
static mm_bool cfm_initialized;
static mm_bool fcw_key_valid;

/* periph_stub -- lightweight register-file stub */
struct periph_stub {
    mm_u32  size;
    mm_u32 *regs;
};

static mm_bool stub_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    const struct periph_stub *ps = (const struct periph_stub *)opaque;
    if (ps == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > ps->size) return MM_FALSE;
    memcpy(value_out, (const mm_u8 *)ps->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool stub_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct periph_stub *ps = (struct periph_stub *)opaque;
    if (ps == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > ps->size) return MM_FALSE;
    memcpy((mm_u8 *)ps->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

#define DECL_STUB(N, S) \
    static mm_u32 N##_regs[(S) / 4u]; \
    static struct periph_stub N##_stub = { (S), N##_regs }

static mm_bool stub_reg_one(struct mmio_bus *bus, struct periph_stub *ps,
                            mm_u32 base)
{
    struct mmio_region reg;
    reg.base   = base;
    reg.size   = ps->size;
    reg.opaque = ps;
    reg.read   = stub_read;
    reg.write  = stub_write;
    return mmio_bus_register_region(bus, &reg);
}

/* MCLK (0x44012000, size 0x60) */
/* CLKMSK[0..8] at offsets 0x3C..0x5C */
#define MCLK_BASE    0x44012000u
#define MCLK_SIZE    0x60u
#define MCLK_CLKMSK0 0x3Cu
static mm_u32 mclk_regs[MCLK_SIZE / 4u];

static mm_bool mclk_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MCLK_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)mclk_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool mclk_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > MCLK_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)mclk_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

mm_bool mm_pic32ck_mclk_periph_active(mm_u32 clkmsk_reg, mm_u32 clkmsk_bit)
{
    mm_u32 off, val;
    if (clkmsk_reg > 8u) return MM_FALSE;
    off = MCLK_CLKMSK0 + clkmsk_reg * 4u;
    memcpy(&val, (mm_u8 *)mclk_regs + off, 4u);
    return ((val >> clkmsk_bit) & 1u) != 0u ? MM_TRUE : MM_FALSE;
}

/* OSCCTRL (0x4400C000, size 0xC0) */
/* STATUS at offset 0x10: bit24=PLL0LOCK, bit16=DFLLRDY, bit8=XOSCRDY, bit0=XOSCRDY0 */
/* SYNCBUSY at 0x78: always 0 (no sync delays in emulator) */
#define OSCCTRL_BASE     0x4400C000u
#define OSCCTRL_SIZE     0xC0u
#define OSCCTRL_STATUS   0x10u
#define OSCCTRL_SYNCBUSY 0x78u
static mm_u32 oscctrl_regs[OSCCTRL_SIZE / 4u];

static mm_bool oscctrl_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                            mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > OSCCTRL_SIZE) return MM_FALSE;
    if ((offset & ~3u) == (OSCCTRL_STATUS & ~3u)) {
        /* PLL0LOCK(24)|DFLLRDY(16)|XOSCRDY(8)|XOSCRDY0(0) all set */
        mm_u32 v = 0x01010101u;
        memcpy(value_out, (mm_u8 *)&v + (offset & 3u), size_bytes);
        return MM_TRUE;
    }
    if ((offset & ~3u) == (OSCCTRL_SYNCBUSY & ~3u)) {
        mm_u32 v = 0u;
        memcpy(value_out, (mm_u8 *)&v + (offset & 3u), size_bytes);
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)oscctrl_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool oscctrl_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                             mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > OSCCTRL_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)oscctrl_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* SUPC (0x44008000, size 0x30) */
/* STATUS at offset 0x0C:
 *   bit8=ADDVREGRDY0 (additional regulator ready -- needed by PLL0 init)
 *   bit4=BORVDDIOB, bit1=LVDRDY, bit0=LVDET
 * VREGCTRL at 0x1C (accept writes, return written value)
 * SYNCBUSY at 0x10: always 0
 */
#define SUPC_BASE    0x44008000u
#define SUPC_SIZE    0x30u
#define SUPC_STATUS  0x0Cu
static mm_u32 supc_regs[SUPC_SIZE / 4u];

static mm_bool supc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SUPC_SIZE) return MM_FALSE;
    if ((offset & ~3u) == (SUPC_STATUS & ~3u)) {
        /* ADDVREGRDY0(8)|BORVDDIOB(4)|LVDRDY(1)|LVDET(0) all set */
        mm_u32 v = 0x00000113u;
        memcpy(value_out, (mm_u8 *)&v + (offset & 3u), size_bytes);
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)supc_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool supc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SUPC_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)supc_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_u32 pic32ck_read_le32(const mm_u8 *buf, mm_u32 off)
{
    return ((mm_u32)buf[off]) |
           ((mm_u32)buf[off + 1u] << 8) |
           ((mm_u32)buf[off + 2u] << 16) |
           ((mm_u32)buf[off + 3u] << 24);
}

static void pic32ck_write_le32(mm_u8 *buf, mm_u32 off, mm_u32 value)
{
    buf[off] = (mm_u8)(value & 0xFFu);
    buf[off + 1u] = (mm_u8)((value >> 8) & 0xFFu);
    buf[off + 2u] = (mm_u8)((value >> 16) & 0xFFu);
    buf[off + 3u] = (mm_u8)((value >> 24) & 0xFFu);
}

static mm_u32 pic32ck_clamp_partition(mm_u32 value, mm_u32 gran, mm_u32 limit)
{
    if (gran == 0u) {
        return 0u;
    }
    value = (value / gran) * gran;
    if (value > limit) {
        value = (limit / gran) * gran;
    }
    return value;
}

static void pic32ck_apply_default_fuses(void)
{
    mm_u32 fucfg14 = pic32ck_read_le32(cfm_buf, PIC32CK_FUSE_FUCFG14_OFF);
    mm_u32 fucfg15 = pic32ck_read_le32(cfm_buf, PIC32CK_FUSE_FUCFG15_OFF);

    if (fucfg14 == 0xFFFFFFFFu) {
        mm_u32 ans_units = (PIC32CK_FLASH_SIZE / 2u) / PIC32CK_FLASH_GRANULARITY;
        pic32ck_write_le32(cfm_buf, PIC32CK_FUSE_FUCFG14_OFF,
                           ans_units & PIC32CK_FUSE_IDAU_ANS_MASK);
    }
    if (fucfg15 == 0xFFFFFFFFu) {
        mm_u32 rns_units = (PIC32CK_RAM_SIZE / 2u) / PIC32CK_RAM_GRANULARITY;
        pic32ck_write_le32(cfm_buf, PIC32CK_FUSE_FUCFG15_OFF,
                           rns_units & PIC32CK_FUSE_IDAU_RNS_MASK);
    }
}

static void pic32ck_idau_reload_from_fuses(void)
{
    mm_u32 fucfg14;
    mm_u32 fucfg15;
    mm_u32 ans_size;
    mm_u32 ansc_size;
    mm_u32 rns_size;

    pic32ck_apply_default_fuses();
    fucfg14 = pic32ck_read_le32(cfm_buf, PIC32CK_FUSE_FUCFG14_OFF);
    fucfg15 = pic32ck_read_le32(cfm_buf, PIC32CK_FUSE_FUCFG15_OFF);

    ans_size = (fucfg14 & PIC32CK_FUSE_IDAU_ANS_MASK) * PIC32CK_FLASH_GRANULARITY;
    ansc_size = ((fucfg14 & PIC32CK_FUSE_IDAU_ANSC_MASK) != 0u)
        ? PIC32CK_ANSC_GRANULARITY : 0u;
    if (ans_size == 0u || ans_size > PIC32CK_FLASH_SIZE) {
        ans_size = PIC32CK_FLASH_SIZE / 2u;
    }
    if (ansc_size > ans_size) {
        ansc_size = ans_size;
    }
    rns_size = (fucfg15 & PIC32CK_FUSE_IDAU_RNS_MASK) * PIC32CK_RAM_GRANULARITY;
    if (rns_size == 0u || rns_size > PIC32CK_RAM_SIZE) {
        rns_size = PIC32CK_RAM_SIZE / 2u;
    }

    g_idau.enabled = MM_TRUE;
    g_idau.ans_size = ans_size;
    g_idau.ansc_size = ansc_size;
    g_idau.rns_size = rns_size;
}

static void pic32ck_idau_store_fuses(void)
{
    mm_u32 fucfg14 = 0u;
    mm_u32 fucfg15 = 0u;

    fucfg14 |= (g_idau.ans_size / PIC32CK_FLASH_GRANULARITY) & PIC32CK_FUSE_IDAU_ANS_MASK;
    if (g_idau.ansc_size != 0u) {
        fucfg14 |= PIC32CK_FUSE_IDAU_ANSC_MASK;
    }
    fucfg15 |= (g_idau.rns_size / PIC32CK_RAM_GRANULARITY) & PIC32CK_FUSE_IDAU_RNS_MASK;
    pic32ck_write_le32(cfm_buf, PIC32CK_FUSE_FUCFG14_OFF, fucfg14);
    pic32ck_write_le32(cfm_buf, PIC32CK_FUSE_FUCFG15_OFF, fucfg15);
}

static mm_bool pic32ck_main_flash_bounds(mm_u32 *secure_len_out,
                                         mm_u32 *nsc_len_out,
                                         mm_u32 *ns_len_out)
{
    mm_u32 secure_len;
    if (g_idau.ans_size > PIC32CK_FLASH_SIZE ||
        g_idau.ansc_size > g_idau.ans_size) {
        return MM_FALSE;
    }
    secure_len = PIC32CK_FLASH_SIZE - g_idau.ans_size;
    if (g_idau.ansc_size > secure_len + g_idau.ans_size) {
        return MM_FALSE;
    }
    secure_len -= g_idau.ansc_size;
    if (secure_len_out != 0) *secure_len_out = secure_len;
    if (nsc_len_out != 0) *nsc_len_out = g_idau.ansc_size;
    if (ns_len_out != 0) *ns_len_out = g_idau.ans_size;
    return MM_TRUE;
}

mm_bool mm_pic32ck_tz_attr_for_addr(mm_u32 addr,
                                    enum mm_sau_attr *attr_out,
                                    mm_u32 *region_out)
{
    mm_u32 secure_len;
    mm_u32 nsc_len;
    mm_u32 ns_len;

    if (attr_out == 0 || region_out == 0 || !g_idau.enabled) {
        return MM_FALSE;
    }
    if (addr >= PIC32CK_FLASH_BASE_S && (addr - PIC32CK_FLASH_BASE_S) < PIC32CK_FLASH_SIZE &&
        pic32ck_main_flash_bounds(&secure_len, &nsc_len, &ns_len)) {
        mm_u32 off = addr - PIC32CK_FLASH_BASE_S;
        if (off < secure_len) {
            *attr_out = MM_SAU_SECURE;
            *region_out = 0x08u;
        } else if (off < secure_len + nsc_len) {
            *attr_out = MM_SAU_NSC;
            *region_out = PIC32CK_IDAU_REGION_ANSC;
        } else {
            *attr_out = MM_SAU_NONSECURE;
            *region_out = PIC32CK_IDAU_REGION_ANS;
        }
        return MM_TRUE;
    }
    if (addr >= PIC32CK_CFM_BASE && (addr - PIC32CK_CFM_BASE) < PIC32CK_CFM_SIZE) {
        *attr_out = MM_SAU_SECURE;
        *region_out = PIC32CK_IDAU_REGION_CFMS;
        return MM_TRUE;
    }
    if (addr >= PIC32CK_RAM_BASE_S && (addr - PIC32CK_RAM_BASE_S) < PIC32CK_RAM_SIZE) {
        mm_u32 secure_ram = PIC32CK_RAM_SIZE - g_idau.rns_size;
        mm_u32 off = addr - PIC32CK_RAM_BASE_S;
        if (off < secure_ram) {
            *attr_out = MM_SAU_SECURE;
            *region_out = PIC32CK_IDAU_REGION_RS;
        } else {
            *attr_out = MM_SAU_NONSECURE;
            *region_out = PIC32CK_IDAU_REGION_RNS;
        }
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool pic32ck_target_ptr(mm_u32 addr, mm_u32 size, mm_u8 **buf_out, mm_u32 *off_out)
{
    if (buf_out == 0 || off_out == 0) {
        return MM_FALSE;
    }
    if (addr >= PIC32CK_FLASH_BASE_S && (addr - PIC32CK_FLASH_BASE_S) + size <= g_flash_size) {
        *buf_out = g_flash_ptr;
        *off_out = addr - PIC32CK_FLASH_BASE_S;
        return MM_TRUE;
    }
    if (addr >= PIC32CK_CFM_BASE && (addr - PIC32CK_CFM_BASE) + size <= PIC32CK_CFM_SIZE) {
        *buf_out = cfm_buf;
        *off_out = addr - PIC32CK_CFM_BASE;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static void pic32ck_flush_target(mm_u32 addr, mm_u32 size)
{
    if (g_map != 0 && g_map->code_cache != 0) {
        mm_code_cache_note_write(g_map->code_cache, addr, size);
    }
    if (g_persist != 0 && addr >= PIC32CK_FLASH_BASE_S &&
        (addr - PIC32CK_FLASH_BASE_S) < g_flash_size) {
        mm_u32 off = addr - PIC32CK_FLASH_BASE_S;
        mm_u32 span = size;
        if (off + span > g_flash_size) {
            span = g_flash_size - off;
        }
        mm_flash_persist_flush((struct mm_flash_persist *)g_persist, off, span);
    }
}

/* FCR -- Flash Controller Read (0x44002000, size 0x20) */
/* STATUS at offset 0x08 always 0x1 (READY) */
#define FCR_BASE    0x44002000u
#define FCR_SIZE    0x20u
#define FCR_STATUS  0x08u
static mm_u32 fcr_regs[FCR_SIZE / 4u];

static mm_bool fcr_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > FCR_SIZE) return MM_FALSE;
    if (offset == FCR_STATUS && size_bytes == 4u) {
        *value_out = 0x1u;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)fcr_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool fcr_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > FCR_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)fcr_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* FCW -- Flash Write Controller (0x44004000, size 0x200) */
/* INTFLAG at 0x14: DONE=bit0, write-1-to-clear; MUTEX at 0x08 */
#define FCW_BASE    0x44004000u
#define FCW_SIZE    0x200u
#define FCW_CTRLA   0x00u
#define FCW_CTRLB   0x04u
#define FCW_MUTEX   0x08u
#define FCW_STATUS  0x18u
#define FCW_INTFLAG 0x14u
#define FCW_INTENCLR 0x0Cu
#define FCW_INTENSET 0x10u
#define FCW_KEY     0x1Cu
#define FCW_ADDR    0x20u
#define FCW_SRCADDR 0x24u
#define FCW_DATA0   0x28u
#define FCW_SWAP    0x48u

#define FCW_STATUS_HTDRDY (1u << 8)
#define FCW_INTFLAG_DONE  (1u << 0)
#define FCW_INTFLAG_KEYERR (1u << 1)
#define FCW_INTFLAG_CFGERR (1u << 2)
#define FCW_INTFLAG_BUSERR (1u << 4)
#define FCW_INTFLAG_WPERR (1u << 5)
#define FCW_INTFLAG_OPERR (1u << 6)
#define FCW_INTFLAG_SECERR (1u << 7)
#define FCW_INTFLAG_WRERR  (1u << 13)
#define FCW_UNLOCK_WRKEY   0x91C32C01u

enum pic32ck_fcw_op {
    PIC32CK_FCW_OP_NOOP = 0u,
    PIC32CK_FCW_OP_SINGLE_DWORD = 1u,
    PIC32CK_FCW_OP_QUAD_DWORD = 2u,
    PIC32CK_FCW_OP_ROW_PROGRAM = 3u,
    PIC32CK_FCW_OP_PAGE_ERASE = 4u,
    PIC32CK_FCW_OP_PROGRAM_ERASE = 7u
};

static mm_u32 fcw_regs[FCW_SIZE / 4u];

static void fcw_flag_set(mm_u32 flag)
{
    fcw_regs[FCW_INTFLAG / 4u] |= flag;
}

static void fcw_set_error(mm_u32 flag)
{
    fcw_flag_set(flag);
}

static mm_bool pic32ck_copy_from_ram(mm_u32 addr, mm_u8 *dst, mm_u32 len)
{
    mm_u32 i;
    struct mm_memmap *map = mm_memmap_current();
    if (map == 0 || dst == 0) {
        return MM_FALSE;
    }
    for (i = 0; i < len; ++i) {
        if (!mm_memmap_read8(map, mmio_active_sec(), addr + i, &dst[i])) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static mm_bool pic32ck_flash_program(mm_u32 addr, const mm_u8 *src, mm_u32 len)
{
    mm_u8 *buf;
    mm_u32 off;
    mm_u32 i;
    enum mm_sau_attr attr;
    mm_u32 region = 0u;

    if (src == 0 || !pic32ck_target_ptr(addr, len, &buf, &off)) {
        fcw_set_error(FCW_INTFLAG_WRERR);
        return MM_FALSE;
    }
    if (mmio_active_sec() == MM_NONSECURE &&
        mm_pic32ck_tz_attr_for_addr(addr, &attr, &region) &&
        attr != MM_SAU_NONSECURE) {
        fcw_set_error(FCW_INTFLAG_SECERR);
        return MM_FALSE;
    }
    for (i = 0; i < len; ++i) {
        buf[off + i] &= src[i];
    }
    if (buf == cfm_buf &&
        off <= PIC32CK_FUSE_FUCFG15_OFF + 3u &&
        (off + len) >= PIC32CK_FUSE_FUCFG14_OFF) {
        pic32ck_idau_reload_from_fuses();
    }
    pic32ck_flush_target(addr, len);
    return MM_TRUE;
}

static mm_bool pic32ck_flash_erase_page(mm_u32 addr)
{
    mm_u8 *buf;
    mm_u32 off;
    mm_u32 base = addr & ~(PIC32CK_FLASH_PAGE_SIZE - 1u);
    enum mm_sau_attr attr;
    mm_u32 region = 0u;

    if (!pic32ck_target_ptr(base, PIC32CK_FLASH_PAGE_SIZE, &buf, &off)) {
        fcw_set_error(FCW_INTFLAG_WRERR);
        return MM_FALSE;
    }
    if (mmio_active_sec() == MM_NONSECURE &&
        mm_pic32ck_tz_attr_for_addr(base, &attr, &region) &&
        attr != MM_SAU_NONSECURE) {
        fcw_set_error(FCW_INTFLAG_SECERR);
        return MM_FALSE;
    }
    memset(buf + off, 0xFF, PIC32CK_FLASH_PAGE_SIZE);
    if (buf == cfm_buf &&
        off <= PIC32CK_FUSE_FUCFG15_OFF + 3u &&
        (off + PIC32CK_FLASH_PAGE_SIZE) >= PIC32CK_FUSE_FUCFG14_OFF) {
        pic32ck_apply_default_fuses();
        pic32ck_idau_reload_from_fuses();
    }
    pic32ck_flush_target(base, PIC32CK_FLASH_PAGE_SIZE);
    return MM_TRUE;
}

static void fcw_execute(mm_u32 ctrla)
{
    mm_u32 op = ctrla & 0xFu;
    mm_u32 addr = fcw_regs[FCW_ADDR / 4u] & ~0x3u;
    mm_u8 tmp[PIC32CK_FLASH_ROW_SIZE];
    mm_bool ok = MM_FALSE;

    fcw_regs[FCW_STATUS / 4u] = FCW_STATUS_HTDRDY;
    if (!fcw_key_valid) {
        fcw_set_error(FCW_INTFLAG_KEYERR);
        return;
    }
    switch (op) {
    case PIC32CK_FCW_OP_NOOP:
        ok = MM_TRUE;
        break;
    case PIC32CK_FCW_OP_SINGLE_DWORD:
        memcpy(tmp, (mm_u8 *)fcw_regs + FCW_DATA0, PIC32CK_FLASH_SDW_SIZE);
        ok = pic32ck_flash_program(addr, tmp, PIC32CK_FLASH_SDW_SIZE);
        break;
    case PIC32CK_FCW_OP_QUAD_DWORD:
        memcpy(tmp, (mm_u8 *)fcw_regs + FCW_DATA0, PIC32CK_FLASH_QDW_SIZE);
        ok = pic32ck_flash_program(addr, tmp, PIC32CK_FLASH_QDW_SIZE);
        break;
    case PIC32CK_FCW_OP_ROW_PROGRAM:
        if (pic32ck_copy_from_ram(fcw_regs[FCW_SRCADDR / 4u] & ~0x3u, tmp,
                                  PIC32CK_FLASH_ROW_SIZE)) {
            ok = pic32ck_flash_program(addr, tmp, PIC32CK_FLASH_ROW_SIZE);
        } else {
            fcw_set_error(FCW_INTFLAG_BUSERR);
        }
        break;
    case PIC32CK_FCW_OP_PAGE_ERASE:
        ok = pic32ck_flash_erase_page(addr);
        break;
    case PIC32CK_FCW_OP_PROGRAM_ERASE:
        if (pic32ck_flash_erase_page(addr) &&
            pic32ck_copy_from_ram(fcw_regs[FCW_SRCADDR / 4u] & ~0x3u, tmp,
                                  PIC32CK_FLASH_ROW_SIZE)) {
            ok = pic32ck_flash_program(addr, tmp, PIC32CK_FLASH_ROW_SIZE);
        } else if ((fcw_regs[FCW_INTFLAG / 4u] & (FCW_INTFLAG_SECERR | FCW_INTFLAG_KEYERR)) == 0u) {
            fcw_set_error(FCW_INTFLAG_BUSERR);
        }
        break;
    default:
        fcw_set_error(FCW_INTFLAG_OPERR);
        break;
    }
    if (ok) {
        fcw_flag_set(FCW_INTFLAG_DONE);
    }
}

static mm_bool fcw_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > FCW_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)fcw_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool fcw_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > FCW_SIZE) return MM_FALSE;
    if (offset == FCW_INTFLAG) {
        fcw_regs[FCW_INTFLAG / 4u] &= ~value;
        return MM_TRUE;
    }
    if (offset == FCW_INTENCLR) {
        fcw_regs[FCW_INTENSET / 4u] &= ~value;
        return MM_TRUE;
    }
    if (offset == FCW_INTENSET) {
        fcw_regs[FCW_INTENSET / 4u] |= value;
        return MM_TRUE;
    }
    memcpy((mm_u8 *)fcw_regs + offset, &value, size_bytes);
    if (offset == FCW_KEY && size_bytes == 4u) {
        fcw_key_valid = (value == FCW_UNLOCK_WRKEY) ? MM_TRUE : MM_FALSE;
        if (!fcw_key_valid) {
            fcw_set_error(FCW_INTFLAG_KEYERR);
        }
        return MM_TRUE;
    }
    if (offset == FCW_CTRLA && size_bytes == 4u) {
        fcw_execute(value);
        fcw_key_valid = MM_FALSE;
        return MM_TRUE;
    }
    return MM_TRUE;
}

/* GCLK (0x44010000, size 0x200)
 * SYNCBUSY at 0x04: always 0 (no sync delays in emulator)
 * GENCTRL[n] at 0x20+n*4: accept r/w
 * PCHCTRL[n] at 0x80+n*4: read back with CHEN (bit6) always set so
 *   firmware does not spin waiting for clock enable confirmation.
 */
#define GCLK_BASE     0x44010000u
#define GCLK_SIZE     0x200u
#define GCLK_SYNCBUSY 0x04u
#define GCLK_PCHCTRL0 0x80u
static mm_u32 gclk_regs[GCLK_SIZE / 4u];

static mm_bool gclk_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > GCLK_SIZE) return MM_FALSE;
    /* SYNCBUSY always 0 */
    if ((offset & ~3u) == (GCLK_SYNCBUSY & ~3u)) {
        mm_u32 v = 0u;
        memcpy(value_out, (mm_u8 *)&v + (offset & 3u), size_bytes);
        return MM_TRUE;
    }
    /* PCHCTRL[n]: return stored value with CHEN (bit6) always set */
    if (offset >= GCLK_PCHCTRL0 && offset < GCLK_SIZE) {
        mm_u32 v;
        memcpy(&v, (mm_u8 *)gclk_regs + (offset & ~3u), 4u);
        v |= (1u << 6);  /* CHEN always set */
        memcpy(value_out, (mm_u8 *)&v + (offset & 3u), size_bytes);
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)gclk_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool gclk_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > GCLK_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)gclk_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* PORT (0x44800000, size 0x200 -- 4 port groups A-D, stride 0x80)
 * Each group has set/clear/toggle aliases for DIR and OUT.
 * Layout per group (offset within group):
 *   0x00 DIR    0x04 DIRCLR  0x08 DIRSET  0x0C DIRTGL
 *   0x10 OUT    0x14 OUTCLR  0x18 OUTSET  0x1C OUTTGL
 *   0x20 IN     0x24 CTRL    0x28 WRCONFIG
 *   0x30 EVCTRL
 *   0x40 PINCFG[0..31]  (1 byte each)
 *   0x60 PMUX[0..15]    (1 byte each, 2 pins per byte)
 */
#define PORT_BASE       0x44800000u
#define PORT_SIZE       0x200u
#define PORT_GROUP_SIZE 0x80u
#define PORT_NGROUPS    4u

/* Group register offsets (byte offsets within a group) */
#define PG_DIR    0x00u
#define PG_DIRCLR 0x04u
#define PG_DIRSET 0x08u
#define PG_DIRTGL 0x0Cu
#define PG_OUT    0x10u
#define PG_OUTCLR 0x14u
#define PG_OUTSET 0x18u
#define PG_OUTTGL 0x1Cu
#define PG_IN     0x20u

/* Per-group state */
struct port_group_state {
    mm_u32 dir;
    mm_u32 out;
    mm_u32 in;
    mm_u8  pincfg[32];
    mm_u8  pmux[16];
    mm_u32 evctrl;
    mm_u32 ctrl;
    mm_u32 wrconfig;
};
static struct port_group_state port_groups[PORT_NGROUPS];

/* Pads (m33mu/gpio_pads.h): PINCFG PMUXEN (bit 0) hands the pin to a
 * peripheral, INEN (bit 1) enables the input, PULLEN (bit 2) with DIR = 0
 * pulls toward OUT (PIC32CK SG/GC DS PORT chapter). */
#define PINCFG_PMUXEN 0x01u
#define PINCFG_PULLEN 0x04u
static struct mm_gpio_pads port_pads[PORT_NGROUPS];
static int port_pad_ids[PORT_NGROUPS] = { 0, 1, 2, 3 };
static mm_bool port_pads_ready;

static void port_pads_init(void)
{
    mm_u32 i;
    if (port_pads_ready) return;
    for (i = 0; i < PORT_NGROUPS; ++i) mm_gpio_pads_init(&port_pads[i], (int)i, 32);
    port_pads_ready = MM_TRUE;
}

static mm_u32 port_pull_mask(const struct port_group_state *g, mm_bool up)
{
    mm_u32 mask = 0u;
    int pin;
    for (pin = 0; pin < 32; ++pin) {
        mm_u32 bit = 1u << pin;
        if ((g->pincfg[pin] & PINCFG_PULLEN) != 0u && (g->dir & bit) == 0u && (((g->out & bit) != 0u) == up)) {
            mask |= bit;
        }
    }
    return mask;
}

static mm_u32 port_driven(const struct port_group_state *g)
{
    mm_u32 gpio = 0u;
    int pin;
    for (pin = 0; pin < 32; ++pin) {
        if ((g->pincfg[pin] & PINCFG_PMUXEN) == 0u) gpio |= 1u << pin;
    }
    return g->dir & gpio;
}

static void port_pad_config(void *opaque, int pin, struct mm_gpio_pad_config *cfg)
{
    const struct port_group_state *g = &port_groups[*(const int *)opaque];
    mm_u8 c = g->pincfg[pin];
    if ((c & PINCFG_PMUXEN) != 0u) {
        cfg->mode = MM_GPIO_MODE_AF;
        cfg->af = (mm_u8)((g->pmux[pin / 2] >> ((pin & 1) * 4)) & 0xFu);
    }
    if ((c & PINCFG_PULLEN) != 0u && (g->dir & (1u << pin)) == 0u) {
        cfg->pull = (g->out & (1u << pin)) != 0u ? MM_GPIO_PULL_UP : MM_GPIO_PULL_DOWN;
    }
}

static void port_publish(mm_u32 grp)
{
    struct port_group_state *g = &port_groups[grp];
    port_pads_init();
    g->in = mm_gpio_pads_input(&port_pads[grp], port_driven(g), g->out, port_pull_mask(g, MM_TRUE),
                               port_pull_mask(g, MM_FALSE), g->out & g->dir);
    mm_gpio_pads_publish(&port_pads[grp], port_driven(g), g->out, port_pad_config, &port_pad_ids[grp]);
}

static mm_bool port_set_external_input_cb(void *opaque, int bank, int pin, mm_bool level,
                                          mm_u64 late_cycles)
{
    (void)opaque;
    (void)late_cycles;
    if (bank < 0 || bank >= (int)PORT_NGROUPS) return MM_FALSE;
    port_pads_init();
    if (!mm_gpio_pads_set_external(&port_pads[bank], pin, level)) return MM_FALSE;
    port_publish((mm_u32)bank);
    return MM_TRUE;
}

static mm_bool port_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)PORT_NGROUPS) return MM_FALSE;
    if (name_out != 0 && name_len > 1u) {
        name_out[0] = (char)('A' + bank);
        name_out[1] = '\0';
    }
    if (pins_out != 0) *pins_out = 32;
    return MM_TRUE;
}

static mm_bool port_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    mm_u32 grp_idx, grp_off, val;
    struct port_group_state *g;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > PORT_SIZE) return MM_FALSE;
    grp_idx = offset / PORT_GROUP_SIZE;
    grp_off = offset % PORT_GROUP_SIZE;
    if (grp_idx >= PORT_NGROUPS) { *value_out = 0; return MM_TRUE; }
    g = &port_groups[grp_idx];
    switch (grp_off & ~3u) {
    case PG_DIR:    val = g->dir; break;
    case PG_DIRCLR: val = g->dir; break;
    case PG_DIRSET: val = g->dir; break;
    case PG_DIRTGL: val = g->dir; break;
    case PG_OUT:    val = g->out; break;
    case PG_OUTCLR: val = g->out; break;
    case PG_OUTSET: val = g->out; break;
    case PG_OUTTGL: val = g->out; break;
    case PG_IN:     port_publish(grp_idx); val = g->in; break;
    default:
        val = 0;
        if (grp_off >= 0x40u && grp_off < 0x60u) memcpy(&val, &g->pincfg[(grp_off - 0x40u) & ~3u], 4);
        else if (grp_off >= 0x60u && grp_off < 0x70u) memcpy(&val, &g->pmux[(grp_off - 0x60u) & ~3u], 4);
        break;
    }
    /* Handle sub-word reads */
    memcpy(value_out, (mm_u8 *)&val + (grp_off & 3u), size_bytes);
    return MM_TRUE;
}

static mm_bool port_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    mm_u32 grp_idx, grp_off;
    struct port_group_state *g;
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > PORT_SIZE) return MM_FALSE;
    grp_idx = offset / PORT_GROUP_SIZE;
    grp_off = offset % PORT_GROUP_SIZE;
    if (grp_idx >= PORT_NGROUPS) return MM_TRUE;
    g = &port_groups[grp_idx];
    /* Expand sub-word writes to 32-bit for set/clr registers */
    if (size_bytes < 4u) {
        mm_u32 tmp = 0u;
        memcpy(&tmp, &value, size_bytes);
        value = tmp;
    }
    switch (grp_off) {
    case PG_DIR:    g->dir  = value;          break;
    case PG_DIRCLR: g->dir &= ~value;         break;
    case PG_DIRSET: g->dir |=  value;         break;
    case PG_DIRTGL: g->dir ^=  value;         break;
    case PG_OUT:    g->out  = value;           break;
    case PG_OUTCLR: g->out &= ~value;          break;
    case PG_OUTSET: g->out |=  value;          break;
    case PG_OUTTGL: g->out ^=  value;          break;
    case PG_IN:     break;   /* read-only */
    default:
        if (grp_off >= 0x40u && grp_off < 0x60u) {
            mm_u32 i;
            for (i = 0; i < size_bytes; ++i) g->pincfg[grp_off - 0x40u + i] = (mm_u8)(value >> (8u * i));
        } else if (grp_off >= 0x60u && grp_off < 0x70u) {
            mm_u32 i;
            for (i = 0; i < size_bytes; ++i) g->pmux[grp_off - 0x60u + i] = (mm_u8)(value >> (8u * i));
        }
        break;
    }
    port_publish(grp_idx);
    return MM_TRUE;
}

/* Generic peripheral stubs */
/* TRNG (0x45024000, size 0x24)
 * CTRLA   [0x00]: ENABLE=bit1
 * INTFLAG [0x0A]: DATARDY=bit0 -- always set (data always ready)
 * DATA    [0x20]: random data -- return pseudo-random value
 */
#define TRNG_BASE    0x45024000u
#define TRNG_SIZE    0x24u
#define TRNG_INTFLAG 0x0Au
#define TRNG_DATA    0x20u
static mm_u32 trng_regs[TRNG_SIZE / 4u];
static mm_u32 trng_counter;

static mm_bool trng_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > TRNG_SIZE) return MM_FALSE;
    if (offset == TRNG_INTFLAG && size_bytes == 1u) {
        *value_out = 0x1u;  /* DATARDY always set */
        return MM_TRUE;
    }
    if (offset == TRNG_DATA && size_bytes == 4u) {
        /* Simple LCG for deterministic pseudo-random output */
        trng_counter = trng_counter * 1664525u + 1013904223u;
        *value_out = trng_counter ^ 0xA5A5A5A5u;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)trng_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool trng_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > TRNG_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)trng_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

#define CFM_SIZE PIC32CK_CFM_SIZE
static mm_bool cfm_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CFM_SIZE) return MM_FALSE;
    memcpy(value_out, cfm_buf + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool cfm_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size_bytes;
    (void)value;
    return MM_FALSE;
}

#define IDAU_BASE    0x4480C000u
#define IDAU_SIZE    0x1180u
#define IDAU_CTRL    0x000u
#define IDAU_STATUSA 0x004u
#define IDAU_STATUSB 0x008u
#define IDAU_REGIONS 0x1000u
#define IDAU_REGION_STRIDE 0x10u

#define IDAU_CTRL_ENABLE_CMD  0xA501u
#define IDAU_CTRL_DISABLE_CMD 0xA502u
#define IDAU_CTRL_WLCK_CMD    0xA503u
#define IDAU_RCTRL_WRSZ_CMD   0x5Cu

static mm_u32 pic32ck_idau_region_size(mm_u32 region)
{
    switch (region) {
    case 0x08u: return PIC32CK_FLASH_SIZE - g_idau.ans_size - g_idau.ansc_size;
    case PIC32CK_IDAU_REGION_ANSC: return g_idau.ansc_size;
    case PIC32CK_IDAU_REGION_ANS: return g_idau.ans_size;
    case PIC32CK_IDAU_REGION_RS: return PIC32CK_RAM_SIZE - g_idau.rns_size;
    case PIC32CK_IDAU_REGION_RNS: return g_idau.rns_size;
    case PIC32CK_IDAU_REGION_CFMS: return PIC32CK_CFM_SIZE;
    default: return 0u;
    }
}

static mm_u32 pic32ck_idau_region_statusa(mm_u32 region)
{
    mm_u32 type = 0u;
    mm_u32 gran = 0u;
    mm_u32 maxsz = 0u;
    switch (region) {
    case 0x08u:
        type = 0x1u;
        gran = 0x0Cu;
        maxsz = 0x15u;
        break;
    case PIC32CK_IDAU_REGION_ANSC:
        type = 0x6u;
        gran = 0x0Cu;
        maxsz = 0x15u;
        break;
    case PIC32CK_IDAU_REGION_ANS:
        type = 0x3u;
        gran = 0x0Cu;
        maxsz = 0x15u;
        break;
    case PIC32CK_IDAU_REGION_RS:
        type = 0x1u;
        gran = 0x0Cu;
        maxsz = 0x13u;
        break;
    case PIC32CK_IDAU_REGION_RNS:
        type = 0x3u;
        gran = 0x0Cu;
        maxsz = 0x13u;
        break;
    case PIC32CK_IDAU_REGION_CFMS:
        type = 0x1u;
        gran = 0x0Cu;
        maxsz = 0x10u;
        break;
    default:
        break;
    }
    return type | (gran << 8) | (maxsz << 16);
}

static mm_bool idau_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    mm_u32 reg_off;
    mm_u32 region;
    mm_u32 v = 0u;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > IDAU_SIZE) return MM_FALSE;
    if (offset == IDAU_STATUSA) {
        v = (g_idau.enabled ? 1u : 0u) |
            ((g_idau.write_locked ? 1u : 0u) << 1) |
            (24u << 8);
        memcpy(value_out, &v, size_bytes);
        return MM_TRUE;
    }
    if (offset == IDAU_STATUSB) {
        v = g_idau.statusb;
        memcpy(value_out, &v, size_bytes);
        return MM_TRUE;
    }
    if (offset < IDAU_REGIONS) {
        return MM_TRUE;
    }
    region = (offset - IDAU_REGIONS) / IDAU_REGION_STRIDE;
    reg_off = (offset - IDAU_REGIONS) % IDAU_REGION_STRIDE;
    if (region >= 24u) return MM_FALSE;
    switch (reg_off & ~3u) {
    case 0x04u: v = pic32ck_idau_region_statusa(region); break;
    case 0x08u: v = pic32ck_idau_region_size(region); break;
    case 0x0Cu: v = 0u; break;
    default:    v = 0u; break;
    }
    memcpy(value_out, (mm_u8 *)&v + (reg_off & 3u), size_bytes);
    return MM_TRUE;
}

static mm_bool idau_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    mm_u32 region;
    mm_u32 arg;
    mm_u32 cmd;
    (void)opaque;
    if (size_bytes != 4u) return MM_FALSE;
    if ((offset + size_bytes) > IDAU_SIZE) return MM_FALSE;
    if (offset == IDAU_CTRL) {
        cmd = (value >> 16) & 0xFFFFu;
        if (cmd == IDAU_CTRL_ENABLE_CMD) {
            g_idau.enabled = MM_TRUE;
        } else if (cmd == IDAU_CTRL_DISABLE_CMD) {
            g_idau.enabled = MM_FALSE;
        } else if (cmd == IDAU_CTRL_WLCK_CMD) {
            g_idau.write_locked = MM_TRUE;
        }
        return MM_TRUE;
    }
    if (offset == IDAU_STATUSB) {
        g_idau.statusb &= ~value;
        return MM_TRUE;
    }
    if (offset < IDAU_REGIONS || g_idau.write_locked || !g_idau.enabled) {
        return MM_TRUE;
    }
    region = (offset - IDAU_REGIONS) / IDAU_REGION_STRIDE;
    if (((offset - IDAU_REGIONS) % IDAU_REGION_STRIDE) != 0u) {
        return MM_TRUE;
    }
    cmd = (value >> 24) & 0xFFu;
    arg = value & 0x00FFFFFFu;
    if (cmd != IDAU_RCTRL_WRSZ_CMD) {
        g_idau.statusb |= 1u;
        return MM_TRUE;
    }
    switch (region) {
    case PIC32CK_IDAU_REGION_ANS:
        g_idau.ans_size = pic32ck_clamp_partition(arg, PIC32CK_FLASH_GRANULARITY,
                                                  PIC32CK_FLASH_SIZE);
        if (g_idau.ans_size == 0u) g_idau.ans_size = PIC32CK_FLASH_GRANULARITY;
        if (g_idau.ansc_size > g_idau.ans_size) g_idau.ansc_size = g_idau.ans_size;
        pic32ck_idau_store_fuses();
        break;
    case PIC32CK_IDAU_REGION_ANSC:
        g_idau.ansc_size = pic32ck_clamp_partition(arg, PIC32CK_ANSC_GRANULARITY,
                                                   g_idau.ans_size);
        pic32ck_idau_store_fuses();
        break;
    case PIC32CK_IDAU_REGION_RNS:
        g_idau.rns_size = pic32ck_clamp_partition(arg, PIC32CK_RAM_GRANULARITY,
                                                  PIC32CK_RAM_SIZE);
        if (g_idau.rns_size == 0u) g_idau.rns_size = PIC32CK_RAM_GRANULARITY;
        pic32ck_idau_store_fuses();
        break;
    default:
        g_idau.statusb |= 1u;
        break;
    }
    return MM_TRUE;
}

DECL_STUB(osc32kctrl, 0x20u);   /* 0x4400E000 */
DECL_STUB(pm,         0x20u);   /* 0x44010000 */
DECL_STUB(wdt,        0x10u);   /* 0x44016000 */
DECL_STUB(rtc,        0x80u);   /* 0x44013000 */
DECL_STUB(eic,        0x40u);   /* 0x44015000 */
DECL_STUB(eic_sec,    0x40u);   /* 0x44015200 */
DECL_STUB(pac,        0x80u);   /* 0x44000000 */
DECL_STUB(dma0,       0x100u);  /* 0x44110000 */
DECL_STUB(dma1,       0x100u);  /* 0x44120000 */
DECL_STUB(freqm,      0x20u);   /* 0x44017000 */

void mm_pic32ck_mmio_reset(void)
{
    memset(mclk_regs,       0, sizeof(mclk_regs));
    memset(oscctrl_regs,    0, sizeof(oscctrl_regs));
    memset(supc_regs,       0, sizeof(supc_regs));
    memset(fcr_regs,        0, sizeof(fcr_regs));
    memset(fcw_regs,        0, sizeof(fcw_regs));
    fcw_regs[FCW_CTRLB / 4u] = 0xFFu;
    fcw_regs[FCW_STATUS / 4u] = FCW_STATUS_HTDRDY;
    memset(gclk_regs,       0, sizeof(gclk_regs));
    memset(trng_regs,       0, sizeof(trng_regs));
    trng_counter = 0x12345678u;
    memset(port_groups,     0, sizeof(port_groups));
    port_pads_ready = MM_FALSE;
    memset(osc32kctrl_regs, 0, sizeof(osc32kctrl_regs));
    memset(pm_regs,         0, sizeof(pm_regs));
    memset(wdt_regs,        0, sizeof(wdt_regs));
    memset(rtc_regs,        0, sizeof(rtc_regs));
    memset(eic_regs,        0, sizeof(eic_regs));
    memset(eic_sec_regs,    0, sizeof(eic_sec_regs));
    memset(pac_regs,        0, sizeof(pac_regs));
    memset(dma0_regs,       0, sizeof(dma0_regs));
    memset(dma1_regs,       0, sizeof(dma1_regs));
    memset(freqm_regs,      0, sizeof(freqm_regs));
    memset(&g_idau,         0, sizeof(g_idau));
    pic32ck_idau_reload_from_fuses();
    g_flash_ptr  = 0;
    g_flash_size = 0u;
    g_map = 0;
    g_persist = 0;
    fcw_key_valid = MM_FALSE;
}

mm_bool mm_pic32ck_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;

    /* MCLK */
    reg.base   = MCLK_BASE;
    reg.size   = MCLK_SIZE;
    reg.opaque = 0;
    reg.read   = mclk_read;
    reg.write  = mclk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* OSCCTRL */
    reg.base   = OSCCTRL_BASE;
    reg.size   = OSCCTRL_SIZE;
    reg.opaque = 0;
    reg.read   = oscctrl_read;
    reg.write  = oscctrl_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* SUPC */
    reg.base   = SUPC_BASE;
    reg.size   = SUPC_SIZE;
    reg.opaque = 0;
    reg.read   = supc_read;
    reg.write  = supc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* FCR */
    reg.base   = FCR_BASE;
    reg.size   = FCR_SIZE;
    reg.opaque = 0;
    reg.read   = fcr_read;
    reg.write  = fcr_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* FCW */
    reg.base   = FCW_BASE;
    reg.size   = FCW_SIZE;
    reg.opaque = 0;
    reg.read   = fcw_read;
    reg.write  = fcw_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* CFM */
    reg.base   = PIC32CK_CFM_BASE;
    reg.size   = PIC32CK_CFM_SIZE;
    reg.opaque = 0;
    reg.read   = cfm_read;
    reg.write  = cfm_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* TRNG */
    reg.base   = TRNG_BASE;
    reg.size   = TRNG_SIZE;
    reg.opaque = 0;
    reg.read   = trng_read;
    reg.write  = trng_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GCLK */
    reg.base   = GCLK_BASE;
    reg.size   = GCLK_SIZE;
    reg.opaque = 0;
    reg.read   = gclk_read;
    reg.write  = gclk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* PORT (NS + Secure alias) */
    reg.base   = PORT_BASE;
    reg.size   = PORT_SIZE;
    reg.opaque = 0;
    reg.read   = port_read;
    reg.write  = port_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    /* Secure alias at 0x44801000 -- share same handler */
    reg.base   = 0x44801000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    mm_gpio_set_external_input_writer(port_set_external_input_cb, 0);
    mm_gpio_set_bank_info_reader(port_bank_info, 0);

    /* Generic stubs */
    if (!stub_reg_one(bus, &osc32kctrl_stub, 0x4400E000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &pm_stub,          0x44006000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &wdt_stub,         0x44016000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &rtc_stub,         0x44018000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &eic_stub,         0x4401A000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &eic_sec_stub,     0x4401B000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &pac_stub,         0x4401C000u)) return MM_FALSE;
    reg.base   = IDAU_BASE;
    reg.size   = IDAU_SIZE;
    reg.opaque = 0;
    reg.read   = idau_read;
    reg.write  = idau_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    if (!stub_reg_one(bus, &dma0_stub,        0x44802000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &dma1_stub,        0x44804000u)) return MM_FALSE;
    if (!stub_reg_one(bus, &freqm_stub,       0x44014000u)) return MM_FALSE;
    /* DSU not mapped — omit */

    return MM_TRUE;
}

void mm_pic32ck_flash_bind(struct mm_memmap *map,
                               mm_u8 *flash, mm_u32 flash_size,
                               const struct mm_flash_persist *persist,
                               mm_u32 flags)
{
    (void)flags;
    g_map = map;
    g_flash_ptr  = flash;
    g_flash_size = flash_size;
    g_persist = persist;
    if (!cfm_initialized) {
        memset(cfm_buf, 0xFF, sizeof(cfm_buf));
        cfm_initialized = MM_TRUE;
    }
    pic32ck_idau_reload_from_fuses();
}

mm_u64 mm_pic32ck_cpu_hz(void)
{
    return 96000000ULL;
}
