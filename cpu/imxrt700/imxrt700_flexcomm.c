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
#include <string.h>
#include "imxrt700/imxrt700_flexcomm.h"
#include "imxrt700/imxrt700_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/target_hal.h"
#include "m33mu/spi_bus.h"

/* LP_FLEXCOMM shares one 4 KB slot between LPUART and LPSPI (LPI2C sits at
 * +0x800 and is a plain register file).  PSELID selects the function. */
#define FC_PSELID 0xFF8u
#define PSELID_PERSEL_MASK 0x7u
#define PERSEL_UART 1u
#define PERSEL_SPI 2u
#define PERSEL_UART_I2C 7u

#define LPUART_STAT 0x14u
#define LPUART_CTRL 0x18u
#define LPUART_DATA 0x1Cu
#define LPUART_FIFO 0x28u

#define CTRL_RE (1u << 18)
#define CTRL_TE (1u << 19)
#define CTRL_RIE (1u << 21)
#define CTRL_TCIE (1u << 22)
#define CTRL_TIE (1u << 23)

#define STAT_IDLE (1u << 20)
#define STAT_RDRF (1u << 21)
#define STAT_TC (1u << 22)
#define STAT_TDRE (1u << 23)

#define FIFO_RXEMPT (1u << 22)
#define FIFO_TXEMPT (1u << 23)

#define LPSPI_CR 0x10u
#define LPSPI_SR 0x14u
#define LPSPI_TCR 0x60u
#define LPSPI_TDR 0x64u
#define LPSPI_RDR 0x74u
#define CR_MEN (1u << 0)
#define CR_RRF (1u << 9)
#define SR_TDF (1u << 0)
#define SR_RDF (1u << 1)
#define SR_TCF (1u << 10)
#define TCR_TXMSK (1u << 18)
#define TCR_RXMSK (1u << 19)
#define TCR_CONT (1u << 21)

struct fc_inst {
    const char *name;
    int index;
    int core;       /* CPU whose NVIC gets the interrupt */
    int irq;
    struct imxrt700_dev *dev;
    struct mm_uart_io uart_io;
    char uart_label[20];
    mm_u8 spi_last_rx;
    mm_bool spi_rx_valid;
};

static const struct {
    const char *name;
    int core;
    int irq;
} fc_table[] = {
    { "LP_FLEXCOMM0", IMXRT700_CPU0, 7 },   { "LP_FLEXCOMM1", IMXRT700_CPU0, 8 },
    { "LP_FLEXCOMM2", IMXRT700_CPU0, 9 },   { "LP_FLEXCOMM3", IMXRT700_CPU0, 10 },
    { "LP_FLEXCOMM4", IMXRT700_CPU0, 11 },  { "LP_FLEXCOMM5", IMXRT700_CPU0, 12 },
    { "LP_FLEXCOMM6", IMXRT700_CPU0, 35 },  { "LP_FLEXCOMM7", IMXRT700_CPU0, 36 },
    { "LP_FLEXCOMM8", IMXRT700_CPU0, 47 },  { "LP_FLEXCOMM9", IMXRT700_CPU0, 48 },
    { "LP_FLEXCOMM10", IMXRT700_CPU0, 49 }, { "LP_FLEXCOMM11", IMXRT700_CPU0, 50 },
    { "LP_FLEXCOMM12", IMXRT700_CPU0, 51 }, { "LP_FLEXCOMM13", IMXRT700_CPU0, 52 },
    { "LP_FLEXCOMM17", IMXRT700_CPU1, 11 }, { "LP_FLEXCOMM18", IMXRT700_CPU1, 12 },
    { "LP_FLEXCOMM19", IMXRT700_CPU1, 13 }, { "LP_FLEXCOMM20", IMXRT700_CPU1, 14 },
};

#define FC_COUNT (sizeof(fc_table) / sizeof(fc_table[0]))

static struct fc_inst fcs[FC_COUNT];

static mm_u32 fc_mode(const struct fc_inst *fc)
{
    mm_u32 persel = mm_imxrt700_reg(fc->dev, FC_PSELID) & PSELID_PERSEL_MASK;
    if (persel == PERSEL_UART_I2C) {
        return PERSEL_UART;
    }
    return persel;
}

static void uart_ensure_open(struct fc_inst *fc)
{
    if (fc->uart_io.fd >= 0) {
        return;
    }
    if (mm_uart_io_open(&fc->uart_io, fc->dev->base)) {
        if (mm_tui_is_active()) {
            mm_tui_attach_uart(fc->uart_label, fc->uart_io.name);
        }
    }
}

static void uart_irq_update(struct fc_inst *fc)
{
    mm_u32 ctrl = mm_imxrt700_reg(fc->dev, LPUART_CTRL);
    mm_u32 stat = mm_imxrt700_reg(fc->dev, LPUART_STAT);
    if (((ctrl & CTRL_RIE) && (stat & STAT_RDRF)) ||
        ((ctrl & CTRL_TIE) && (stat & STAT_TDRE)) ||
        ((ctrl & CTRL_TCIE) && (stat & STAT_TC))) {
        mm_imxrt700_irq_set(fc->core, fc->irq, MM_TRUE);
    }
}

static void uart_update_status(struct fc_inst *fc)
{
    mm_u32 stat = mm_imxrt700_reg(fc->dev, LPUART_STAT);
    mm_u32 fifo = mm_imxrt700_reg(fc->dev, LPUART_FIFO);
    stat |= STAT_TDRE | STAT_TC;
    fifo |= FIFO_TXEMPT;
    if (mm_uart_io_has_rx(&fc->uart_io)) {
        stat |= STAT_RDRF;
        fifo &= ~FIFO_RXEMPT;
    } else {
        stat &= ~STAT_RDRF;
        stat |= STAT_IDLE;
        fifo |= FIFO_RXEMPT;
    }
    mm_imxrt700_reg_put(fc->dev, LPUART_STAT, stat);
    mm_imxrt700_reg_put(fc->dev, LPUART_FIFO, fifo);
}

static mm_bool uart_read(struct fc_inst *fc, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    if (off == LPUART_DATA) {
        mm_u32 v = 0;
        if (mm_uart_io_has_rx(&fc->uart_io)) {
            v = mmio_peek_mode() ? (mm_u32)mm_uart_io_peek(&fc->uart_io)
                                 : (mm_u32)mm_uart_io_read(&fc->uart_io);
        }
        if (!mmio_peek_mode()) {
            uart_update_status(fc);
        }
        *value_out = (size == 1u) ? (v & 0xFFu) : v;
        return MM_TRUE;
    }
    if (off == LPUART_STAT || off == LPUART_FIFO) {
        uart_update_status(fc);
    }
    return MM_FALSE;
}

static mm_bool uart_write(struct fc_inst *fc, mm_u32 off, mm_u32 size, mm_u32 value)
{
    if (off == LPUART_DATA) {
        (void)size;
        if ((mm_imxrt700_reg(fc->dev, LPUART_CTRL) & CTRL_TE) != 0u) {
            uart_ensure_open(fc);
            mm_uart_io_queue_tx(&fc->uart_io, (mm_u8)(value & 0xFFu));
            (void)mm_uart_io_flush(&fc->uart_io);
        }
        uart_update_status(fc);
        uart_irq_update(fc);
        return MM_TRUE;
    }
    if (off == LPUART_STAT) {
        /* Status flags are write-1-to-clear; the TX side is always ready. */
        mm_imxrt700_reg_put(fc->dev, LPUART_STAT, mm_imxrt700_reg(fc->dev, LPUART_STAT) & ~(value & 0xC01FC000u));
        uart_update_status(fc);
        return MM_TRUE;
    }
    if (off == LPUART_CTRL && size == 4u) {
        mm_imxrt700_reg_put(fc->dev, LPUART_CTRL, value);
        if ((value & (CTRL_RE | CTRL_TE)) != 0u) {
            uart_ensure_open(fc);
        }
        uart_update_status(fc);
        uart_irq_update(fc);
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool spi_read(struct fc_inst *fc, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    if (off == LPSPI_RDR && size == 4u) {
        *value_out = fc->spi_rx_valid ? (mm_u32)fc->spi_last_rx : 0u;
        if (!mmio_peek_mode()) {
            fc->spi_rx_valid = MM_FALSE;
            mm_imxrt700_reg_put(fc->dev, LPSPI_SR, mm_imxrt700_reg(fc->dev, LPSPI_SR) & ~SR_RDF);
        }
        return MM_TRUE;
    }
    if (off == LPSPI_SR && size == 4u) {
        *value_out = mm_imxrt700_reg(fc->dev, LPSPI_SR) | SR_TDF | (fc->spi_rx_valid ? SR_RDF : 0u);
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool spi_write(struct fc_inst *fc, mm_u32 off, mm_u32 size, mm_u32 value)
{
    if (size != 4u) {
        return MM_FALSE;
    }
    if (off == LPSPI_CR) {
        mm_imxrt700_reg_put(fc->dev, LPSPI_CR, value & ~CR_RRF);
        if ((value & CR_RRF) != 0u) {
            fc->spi_rx_valid = MM_FALSE;
        }
        if ((value & CR_MEN) == 0u) {
            mm_spi_bus_end(fc->index);
        }
        return MM_TRUE;
    }
    if (off == LPSPI_TDR) {
        mm_u32 tcr = mm_imxrt700_reg(fc->dev, LPSPI_TCR);
        mm_u8 in = 0xFFu;
        if ((mm_imxrt700_reg(fc->dev, LPSPI_CR) & CR_MEN) != 0u) {
            if ((tcr & TCR_TXMSK) == 0u) {
                in = mm_spi_bus_xfer(fc->index, (mm_u8)(value & 0xFFu));
            }
            if ((tcr & TCR_RXMSK) == 0u) {
                fc->spi_last_rx = in;
                fc->spi_rx_valid = MM_TRUE;
            }
            mm_imxrt700_reg_put(fc->dev, LPSPI_SR, mm_imxrt700_reg(fc->dev, LPSPI_SR) | SR_TCF);
            if ((tcr & TCR_CONT) == 0u) {
                mm_spi_bus_end(fc->index);
            }
        }
        return MM_TRUE;
    }
    if (off == LPSPI_SR) {
        mm_imxrt700_reg_put(fc->dev, LPSPI_SR, mm_imxrt700_reg(fc->dev, LPSPI_SR) & ~value);
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool fc_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    struct fc_inst *fc = (struct fc_inst *)dev->opaque;
    switch (fc_mode(fc)) {
    case PERSEL_UART:
        return uart_read(fc, off, size, value_out);
    case PERSEL_SPI:
        return spi_read(fc, off, size, value_out);
    default:
        return MM_FALSE;
    }
}

static mm_bool fc_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    struct fc_inst *fc = (struct fc_inst *)dev->opaque;
    if (off == FC_PSELID) {
        mm_u32 cur = mm_imxrt700_reg(dev, FC_PSELID);
        if ((cur & 0x8u) != 0u) {
            return MM_TRUE; /* LOCK */
        }
        /* Only PERSEL and LOCK are writable; the PRESENT/ID bits are fixed. */
        mm_imxrt700_reg_put(dev, FC_PSELID, (cur & ~0xFu) | (value & 0xFu));
        return MM_TRUE;
    }
    switch (fc_mode(fc)) {
    case PERSEL_UART:
        return uart_write(fc, off, size, value);
    case PERSEL_SPI:
        return spi_write(fc, off, size, value);
    default:
        return MM_FALSE;
    }
}

void mm_imxrt700_flexcomm_attach(void)
{
    mm_u32 i;
    for (i = 0; i < FC_COUNT; ++i) {
        struct fc_inst *fc = &fcs[i];
        if (fc->name == 0) {
            fc->name = fc_table[i].name;
            fc->core = fc_table[i].core;
            fc->irq = fc_table[i].irq;
            fc->index = (int)i;
            mm_uart_io_init(&fc->uart_io);
            snprintf(fc->uart_label, sizeof(fc->uart_label), "LPUART%s", fc->name + 11);
        }
        fc->dev = mm_imxrt700_dev(fc->name);
        if (fc->dev == 0) {
            continue;
        }
        (void)mm_imxrt700_dev_hook(fc->name, fc_read, fc_write, fc);
    }
}

void mm_imxrt700_flexcomm_reset(void)
{
    mm_u32 i;
    for (i = 0; i < FC_COUNT; ++i) {
        if (fcs[i].name == 0) {
            continue;
        }
        mm_uart_io_close(&fcs[i].uart_io);
        mm_uart_io_init(&fcs[i].uart_io);
        fcs[i].spi_rx_valid = MM_FALSE;
    }
}

void mm_imxrt700_flexcomm_poll(void)
{
    mm_u32 i;
    for (i = 0; i < FC_COUNT; ++i) {
        struct fc_inst *fc = &fcs[i];
        if (fc->dev == 0 || fc_mode(fc) != PERSEL_UART) {
            continue;
        }
        if (mm_uart_io_poll(&fc->uart_io)) {
            uart_update_status(fc);
        }
        uart_irq_update(fc);
    }
}
