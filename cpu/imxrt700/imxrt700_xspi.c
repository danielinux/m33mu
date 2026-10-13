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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imxrt700/imxrt700_xspi.h"
#include "imxrt700/imxrt700_mmio.h"
#include "imxrt700/cpu_config.h"
#include "m33mu/code_cache.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"

/* Registers */
#define XSPI_MCR 0x000u
#define XSPI_SFAR 0x100u
#define XSPI_RBSR 0x10Cu
#define XSPI_RBCT 0x110u
#define XSPI_DLLSR 0x12Cu
#define XSPI_TBSR 0x150u
#define XSPI_TBDR 0x154u
#define XSPI_SR 0x15Cu
#define XSPI_FR 0x160u
#define XSPI_SPTRCLR 0x16Cu
#define XSPI_RBDR0 0x200u
#define XSPI_RBDR_END 0x300u
#define XSPI_LUT0 0x310u
#define XSPI_FRAD0 0x800u
#define XSPI_FRAD_END 0x900u
#define XSPI_TG0MDAD 0x900u
#define XSPI_TGSFAR 0x904u
#define XSPI_TGSFARS 0x908u
#define XSPI_TGIPCRS 0x90Cu
#define XSPI_TG1MDAD 0x910u
#define XSPI_MGC 0x920u
#define XSPI_FSMSTAT 0x930u
#define XSPI_ERRSTAT 0x938u
#define XSPI_SFP_TG_IPCR 0x958u
#define XSPI_SFP_TG_SFAR 0x95Cu

#define MCR_SWRSTSD (1u << 0)
#define MCR_SWRSTHD (1u << 1)
#define MCR_IPS_TG_RST (1u << 9)
#define MCR_CLR_RXF (1u << 10)
#define MCR_CLR_TXF (1u << 11)
#define DLLSR_LOCKED 0x0000C000u      /* SLVA_LOCK | DLLA_LOCK */
#define SR_RXWE (1u << 16)
#define FR_TBFF (1u << 27)
#define FSMSTAT_READY 0x80000001u     /* VLD, STATE 1: TX buffer lock open */
#define TGSFARS_CLR (1u << 29)
#define TGSFARS_ERR (1u << 30)
#define TGSFARS_VLD (1u << 31)
#define TGIPCRS_CLR (1u << 28)
#define MDAD_LCK (1u << 29)
#define MDAD_VLD (1u << 31)
#define MGC_GCLCK (3u << 10)
#define MGC_GVLDFRAD (1u << 27)
#define MGC_GVLDMDAD (1u << 29)
#define FRAD_W2_MD0ACP 0x7u
#define FRAD_W3_LOCK (3u << 29)
#define FRAD_W3_VLD (1u << 31)
#define ERRSTAT_FRADMTCH (1u << 0)
#define ERRSTAT_FRAD0ACC (1u << 1)
#define ERRSTAT_TG0SFAR (1u << 10)
#define ERRSTAT_ARB_WIN (1u << 28)

/* LUT instruction codes */
#define LUT_STOP 0x00u
#define LUT_CMD_SDR 0x01u
#define LUT_RADDR_SDR 0x02u
#define LUT_READ_SDR 0x07u
#define LUT_WRITE_SDR 0x08u
#define LUT_RADDR_DDR 0x0Au
#define LUT_READ_DDR 0x0Eu
#define LUT_WRITE_DDR 0x0Fu
#define LUT_CMD_DDR 0x11u
#define LUT_WORDS_PER_SEQ 5u

/* NOR status */
#define NOR_WEL 0x02u

#define XSPI_WINDOW_SIZE 0x08000000u
#define XSPI_BUF_WORDS 64u

struct xspi_state {
    struct imxrt700_dev *dev;
    mm_u8 *flash;
    mm_u32 flash_size;
    const struct mm_flash_persist *persist;
    mm_u32 sfar;
    mm_bool sfar_pending;
    mm_u32 rx[XSPI_BUF_WORDS];
    mm_u32 rx_words;
    mm_u32 rx_removed;
    mm_u8 tx[XSPI_BUF_WORDS * 4u];
    mm_u32 tx_bytes;
    mm_bool write_pending;
    mm_u8 write_op;
    mm_u32 write_off;
    mm_u32 write_size;
    mm_bool wel;
    mm_u8 scur;
};

static struct xspi_state xspi0;

static mm_bool trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("M33MU_XSPI_TRACE") != 0 ? 1 : 0;
    }
    return cached != 0;
}

static mm_u32 reg(mm_u32 off)
{
    return mm_imxrt700_reg(xspi0.dev, off);
}

static void put(mm_u32 off, mm_u32 v)
{
    mm_imxrt700_reg_put(xspi0.dev, off, v);
}

static void rx_clear(void)
{
    xspi0.rx_words = 0;
    xspi0.rx_removed = 0;
}

static void tx_clear(void)
{
    xspi0.tx_bytes = 0;
}

static void rx_push_bytes(const mm_u8 *b, mm_u32 len)
{
    /* Each command's data starts on a fresh RX buffer word. */
    mm_u32 base = xspi0.rx_words;
    mm_u32 i;
    for (i = 0; i < len; ++i) {
        mm_u32 w = base + i / 4u;
        mm_u32 sh = (i % 4u) * 8u;
        if (w >= XSPI_BUF_WORDS) {
            break;
        }
        if (sh == 0u) {
            xspi0.rx[w] = 0u;
        }
        xspi0.rx[w] |= (mm_u32)b[i] << sh;
    }
    xspi0.rx_words = base + (len + 3u) / 4u;
    if (xspi0.rx_words > XSPI_BUF_WORDS) {
        xspi0.rx_words = XSPI_BUF_WORDS;
    }
}

/* ------------------------------------------------------------------------ */
/* NOR array                                                                */
/* ------------------------------------------------------------------------ */

static void flash_changed(mm_u32 off, mm_u32 len)
{
    struct mm_memmap *map = mm_imxrt700_memmap();
    if (map != 0 && map->code_cache != 0) {
        mm_code_cache_note_write(map->code_cache, IMXRT700_FLASH_BASE_NS + off, len);
        mm_code_cache_note_write(map->code_cache, IMXRT700_FLASH_BASE_S + off, len);
    }
    if (xspi0.persist != 0) {
        mm_flash_persist_flush((struct mm_flash_persist *)xspi0.persist, off, len);
    }
}

static mm_bool nor_offset(mm_u32 addr, mm_u32 *off_out)
{
    mm_u32 off = addr & (XSPI_WINDOW_SIZE - 1u);
    if (xspi0.flash == 0 || off >= xspi0.flash_size) {
        return MM_FALSE;
    }
    *off_out = off;
    return MM_TRUE;
}

static void nor_erase(mm_u32 addr, mm_u32 size)
{
    mm_u32 off;
    if (!nor_offset(addr, &off)) {
        return;
    }
    off &= ~(size - 1u);
    if (off + size > xspi0.flash_size) {
        size = xspi0.flash_size - off;
    }
    memset(xspi0.flash + off, 0xFF, size);
    flash_changed(off, size);
    if (trace_enabled()) {
        fprintf(stderr, "[XSPI] erase 0x%08lx +0x%lx\n", (unsigned long)off, (unsigned long)size);
    }
}

static void nor_program(mm_u32 off, const mm_u8 *data, mm_u32 len)
{
    mm_u32 page = off & ~0xFFu;
    mm_u32 i;
    /* A page program wraps within its 256-byte page and only clears bits. */
    for (i = 0; i < len; ++i) {
        mm_u32 a = page + ((off - page + i) & 0xFFu);
        if (a < xspi0.flash_size) {
            xspi0.flash[a] &= data[i];
        }
    }
    flash_changed(page, 0x100u);
    if (trace_enabled()) {
        fprintf(stderr, "[XSPI] program 0x%08lx +0x%lx\n", (unsigned long)off, (unsigned long)len);
    }
}

/* ------------------------------------------------------------------------ */
/* Serial flash protection (FRAD / MDAD)                                    */
/* ------------------------------------------------------------------------ */

static mm_u32 frad_word(mm_u32 n, mm_u32 w)
{
    return reg(XSPI_FRAD0 + n * 0x20u + w * 4u);
}

/* MM_TRUE when an IP command that modifies the NOR may target addr. */
static mm_bool frad_allows_write(mm_u32 addr)
{
    mm_u32 n;
    mm_u32 a = addr & 0xFFFF0000u;
    if ((reg(XSPI_MGC) & MGC_GVLDFRAD) == 0u) {
        return MM_TRUE;
    }
    for (n = 0; n < 8u; ++n) {
        mm_u32 w3 = frad_word(n, 3);
        mm_u32 start = frad_word(n, 0) & 0xFFFF0000u;
        mm_u32 end = frad_word(n, 1) & 0xFFFF0000u;
        if ((w3 & FRAD_W3_VLD) == 0u || a < start || a > end) {
            continue;
        }
        if ((frad_word(n, 2) & FRAD_W2_MD0ACP) == 0u) {
            put(XSPI_ERRSTAT, reg(XSPI_ERRSTAT) | (ERRSTAT_FRAD0ACC << n));
            return MM_FALSE;
        }
        return MM_TRUE;
    }
    put(XSPI_ERRSTAT, reg(XSPI_ERRSTAT) | ERRSTAT_FRADMTCH);
    return MM_FALSE;
}

/* ------------------------------------------------------------------------ */
/* IP command engine                                                        */
/* ------------------------------------------------------------------------ */

struct lut_seq {
    mm_u8 opcode;
    mm_bool has_opcode;
    mm_bool has_addr;
    mm_bool has_read;
    mm_bool has_write;
};

static void lut_decode(mm_u32 seq, struct lut_seq *s)
{
    mm_u32 i;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < LUT_WORDS_PER_SEQ * 2u; ++i) {
        mm_u32 word = reg(XSPI_LUT0 + (seq * LUT_WORDS_PER_SEQ + i / 2u) * 4u);
        mm_u32 half = (i & 1u) ? (word >> 16) : (word & 0xFFFFu);
        mm_u32 instr = (half >> 10) & 0x3Fu;
        mm_u8 operand = (mm_u8)(half & 0xFFu);
        if (instr == LUT_STOP) {
            break;
        }
        switch (instr) {
        case LUT_CMD_SDR:
        case LUT_CMD_DDR:
            /* Octal DTR commands are opcode + inverted opcode: keep the first. */
            if (!s->has_opcode) {
                s->opcode = operand;
                s->has_opcode = MM_TRUE;
            }
            break;
        case LUT_RADDR_SDR:
        case LUT_RADDR_DDR:
            s->has_addr = MM_TRUE;
            break;
        case LUT_READ_SDR:
        case LUT_READ_DDR:
            s->has_read = MM_TRUE;
            break;
        case LUT_WRITE_SDR:
        case LUT_WRITE_DDR:
            s->has_write = MM_TRUE;
            break;
        default:
            break;
        }
    }
}

static mm_bool op_is_erase(mm_u8 op, mm_u32 *size_out)
{
    switch (op) {
    case 0x20u: case 0x21u:           /* 4 KB sector */
        *size_out = 0x1000u;
        return MM_TRUE;
    case 0xD8u: case 0xDCu:           /* 64 KB block */
        *size_out = 0x10000u;
        return MM_TRUE;
    case 0x60u: case 0xC7u:           /* chip */
        *size_out = 0u;
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

static mm_bool op_is_program(mm_u8 op)
{
    return (op == 0x02u || op == 0x12u) ? MM_TRUE : MM_FALSE;
}

static void ip_command(mm_u32 ipcr)
{
    mm_u32 seq = (ipcr >> 24) & 0xFu;
    mm_u32 size = ipcr & 0xFFFFu;
    mm_u32 addr = xspi0.sfar;
    struct lut_seq s;
    mm_u32 esize = 0;
    mm_u32 off = 0;
    mm_bool modifies;

    lut_decode(seq, &s);
    put(XSPI_ERRSTAT, reg(XSPI_ERRSTAT) | ERRSTAT_ARB_WIN);
    xspi0.sfar_pending = MM_FALSE;
    xspi0.write_pending = MM_FALSE;
    if (trace_enabled()) {
        fprintf(stderr, "[XSPI] ip seq=%lu op=0x%02x addr=0x%08lx size=%lu%s%s%s\n",
                (unsigned long)seq, s.opcode, (unsigned long)addr, (unsigned long)size,
                s.has_addr ? " addr" : "", s.has_read ? " read" : "", s.has_write ? " write" : "");
    }
    if (!s.has_opcode) {
        return;
    }
    modifies = (op_is_program(s.opcode) || op_is_erase(s.opcode, &esize)) ? MM_TRUE : MM_FALSE;
    if (modifies && !frad_allows_write(addr)) {
        return; /* refused by the SFP: ERRSTAT carries the reason */
    }
    switch (s.opcode) {
    case 0x06u: /* WREN */
        xspi0.wel = MM_TRUE;
        return;
    case 0x04u: /* WRDI */
        xspi0.wel = MM_FALSE;
        return;
    case 0x05u: { /* RDSR: WIP never set, operations complete immediately */
        mm_u8 st[8];
        memset(st, xspi0.wel ? NOR_WEL : 0u, sizeof(st));
        rx_push_bytes(st, size > sizeof(st) ? sizeof(st) : (size ? size : 1u));
        return;
    }
    case 0x2Bu: { /* RDSCUR */
        mm_u8 st[8];
        memset(st, xspi0.scur, sizeof(st));
        rx_push_bytes(st, size > sizeof(st) ? sizeof(st) : (size ? size : 1u));
        return;
    }
    case 0x9Fu: { /* RDID: Macronix MX25UM51345G */
        static const mm_u8 id[8] = { 0xC2u, 0x81u, 0x3Au, 0xC2u, 0x81u, 0x3Au, 0xC2u, 0x81u };
        rx_push_bytes(id, size > sizeof(id) ? sizeof(id) : size);
        return;
    }
    default:
        break;
    }
    if (op_is_erase(s.opcode, &esize)) {
        if (xspi0.wel) {
            nor_erase(addr, esize != 0u ? esize : xspi0.flash_size);
        }
        xspi0.wel = MM_FALSE;
        return;
    }
    if (op_is_program(s.opcode)) {
        if (!nor_offset(addr, &off)) {
            return;
        }
        xspi0.write_pending = MM_TRUE;
        xspi0.write_op = s.opcode;
        xspi0.write_off = off;
        xspi0.write_size = size;
        tx_clear();
        return;
    }
    if (s.has_read && s.has_addr && nor_offset(addr, &off)) {
        mm_u32 n = size;
        if (n > XSPI_BUF_WORDS * 4u) {
            n = XSPI_BUF_WORDS * 4u;
        }
        if (off + n > xspi0.flash_size) {
            n = xspi0.flash_size - off;
        }
        rx_push_bytes(xspi0.flash + off, n);
        return;
    }
    /* Other commands (configuration register writes, ...) have no effect. */
}

static void tx_push(mm_u32 word)
{
    mm_u32 i;
    for (i = 0; i < 4u && xspi0.tx_bytes < sizeof(xspi0.tx); ++i) {
        xspi0.tx[xspi0.tx_bytes++] = (mm_u8)(word >> (8u * i));
    }
    if (xspi0.write_pending && xspi0.tx_bytes >= xspi0.write_size) {
        if (xspi0.wel) {
            nor_program(xspi0.write_off, xspi0.tx, xspi0.write_size);
        }
        xspi0.wel = MM_FALSE;
        xspi0.write_pending = MM_FALSE;
        tx_clear();
    }
}

/* ------------------------------------------------------------------------ */
/* Register interface                                                       */
/* ------------------------------------------------------------------------ */

static mm_bool xspi_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    (void)dev;
    if (size != 4u) {
        return MM_FALSE;
    }
    switch (off) {
    case XSPI_DLLSR:
        *value_out = reg(off) | DLLSR_LOCKED;
        return MM_TRUE;
    case XSPI_SR: {
        mm_u32 wm = (reg(XSPI_RBCT) & 0x7Fu) + 1u;
        *value_out = (xspi0.rx_words >= wm) ? SR_RXWE : 0u;
        return MM_TRUE;
    }
    case XSPI_FSMSTAT:
        *value_out = FSMSTAT_READY;
        return MM_TRUE;
    case XSPI_RBSR:
        *value_out = (xspi0.rx_words & 0xFFu) | ((xspi0.rx_removed & 0xFFFFu) << 16);
        return MM_TRUE;
    case XSPI_TBSR:
        *value_out = ((xspi0.tx_bytes / 4u) & 0x1FFu);
        return MM_TRUE;
    case XSPI_SFAR:
    case XSPI_TGSFAR:
        *value_out = xspi0.sfar;
        return MM_TRUE;
    case XSPI_TGSFARS:
        *value_out = xspi0.sfar_pending ? TGSFARS_VLD : 0u;
        return MM_TRUE;
    case XSPI_TGIPCRS:
        *value_out = 0u;
        return MM_TRUE;
    default:
        break;
    }
    if (off >= XSPI_RBDR0 && off < XSPI_RBDR_END) {
        mm_u32 i = (off - XSPI_RBDR0) / 4u;
        *value_out = (i < xspi0.rx_words) ? xspi0.rx[i] : 0u;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool frad_write(mm_u32 off, mm_u32 value)
{
    mm_u32 n = (off - XSPI_FRAD0) / 0x20u;
    mm_u32 w = ((off - XSPI_FRAD0) % 0x20u) / 4u;
    mm_u32 w3 = frad_word(n, 3);
    if (w > 3u) {
        return MM_TRUE; /* compare status words are read-only */
    }
    if ((w3 & FRAD_W3_LOCK) != 0u) {
        return MM_TRUE; /* descriptor locked until reset */
    }
    put(off, value);
    return MM_TRUE;
}

static mm_bool xspi_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    (void)dev;
    if (size != 4u) {
        return MM_FALSE;
    }
    if (off >= XSPI_FRAD0 && off < XSPI_FRAD_END) {
        return frad_write(off, value);
    }
    switch (off) {
    case XSPI_MCR:
        if (value & MCR_CLR_RXF) {
            rx_clear();
        }
        if (value & MCR_CLR_TXF) {
            tx_clear();
        }
        if (value & MCR_IPS_TG_RST) {
            xspi0.sfar_pending = MM_FALSE;
            xspi0.write_pending = MM_FALSE;
        }
        if (value & (MCR_SWRSTSD | MCR_SWRSTHD)) {
            rx_clear();
            tx_clear();
            xspi0.write_pending = MM_FALSE;
        }
        put(off, value & ~(MCR_CLR_RXF | MCR_CLR_TXF | MCR_IPS_TG_RST));
        return MM_TRUE;
    case XSPI_FR:
    case XSPI_ERRSTAT:
        put(off, reg(off) & ~value); /* write-1-to-clear */
        return MM_TRUE;
    case XSPI_SPTRCLR:
        put(off, value & ~0x10101u); /* BFPTRC / IPPTRC / ABRT_CLR complete at once */
        return MM_TRUE;
    case XSPI_TBDR:
        tx_push(value);
        return MM_TRUE;
    case XSPI_SFP_TG_SFAR:
        put(off, value);
        xspi0.sfar = value;
        if ((reg(XSPI_MGC) & MGC_GVLDMDAD) != 0u && (reg(XSPI_TG0MDAD) & MDAD_VLD) != 0u) {
            mm_u32 o;
            if (!nor_offset(value, &o)) {
                put(XSPI_ERRSTAT, reg(XSPI_ERRSTAT) | ERRSTAT_TG0SFAR);
            }
            xspi0.sfar_pending = MM_TRUE;
        }
        return MM_TRUE;
    case XSPI_SFP_TG_IPCR:
        put(off, value);
        ip_command(value);
        return MM_TRUE;
    case XSPI_TGSFARS:
        if (value & TGSFARS_CLR) {
            xspi0.sfar_pending = MM_FALSE;
        }
        return MM_TRUE;
    case XSPI_TGIPCRS:
        return MM_TRUE;
    case XSPI_TG0MDAD:
    case XSPI_TG1MDAD:
        if ((reg(off) & MDAD_LCK) != 0u) {
            return MM_TRUE;
        }
        put(off, value);
        return MM_TRUE;
    case XSPI_MGC:
        if ((reg(off) & MGC_GCLCK) != 0u) {
            return MM_TRUE; /* global configuration locked */
        }
        put(off, value);
        return MM_TRUE;
    case XSPI_FSMSTAT:
    case XSPI_SR:
    case XSPI_RBSR:
    case XSPI_TBSR:
    case XSPI_DLLSR:
        return MM_TRUE;
    default:
        break;
    }
    if (off >= XSPI_RBDR0 && off < XSPI_RBDR_END) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

void mm_imxrt700_xspi_attach(void)
{
    xspi0.dev = mm_imxrt700_dev("XSPI0");
    (void)mm_imxrt700_dev_hook("XSPI0", xspi_read, xspi_write, &xspi0);
}

void mm_imxrt700_xspi_reset(void)
{
    rx_clear();
    tx_clear();
    xspi0.sfar = 0;
    xspi0.sfar_pending = MM_FALSE;
    xspi0.write_pending = MM_FALSE;
    xspi0.wel = MM_FALSE;
    xspi0.scur = 0;
}

void mm_imxrt700_xspi_bind(mm_u8 *flash, mm_u32 flash_size, const struct mm_flash_persist *persist)
{
    xspi0.flash = flash;
    xspi0.flash_size = flash_size;
    xspi0.persist = persist;
}
