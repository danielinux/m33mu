/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include <stdio.h>
#include "lpc55s69/lpc55s69_flexcomm.h"
#include "lpc55s69/lpc55s69_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/target_hal.h"
#include "m33mu/spi_bus.h"

/* AHBCLKCTRL1/PRESETCTRL1 bit positions for FC0-FC7 */
#define FLEXCOMM_COUNT   9
#define FC_AHBCLKCTRL1_OFFSET 0x204u
#define FC_AHBCLKCTRL2_OFFSET 0x208u
#define FC_BIT_IN_CTRL1(n)    (11u + (mm_u32)(n))  /* FC0=bit11 .. FC7=bit18 */
#define FC8_BIT_IN_CTRL2      28u                   /* HS_LSPI */

static mm_bool fc_periph_active(int idx)
{
    if (idx < 0 || idx >= FLEXCOMM_COUNT) return MM_FALSE;
    if (idx < 8)
        return mm_lpc55s69_syscon_periph_active(FC_AHBCLKCTRL1_OFFSET,
                                                FC_BIT_IN_CTRL1(idx));
    return mm_lpc55s69_syscon_periph_active(FC_AHBCLKCTRL2_OFFSET,
                                            FC8_BIT_IN_CTRL2);
}

/*
 * LPC55S69 FLEXCOMM base addresses.
 * FC0-FC4 are contiguous; FC5-FC7 are at a different base; FC8 is separate.
 */
#define FLEXCOMM_SIZE    0x1000u

static const mm_u32 flexcomm_bases[FLEXCOMM_COUNT] = {
    0x40086000u,  /* FC0 */
    0x40087000u,  /* FC1 */
    0x40088000u,  /* FC2 */
    0x40089000u,  /* FC3 */
    0x4008A000u,  /* FC4 */
    0x40096000u,  /* FC5 */
    0x40097000u,  /* FC6 */
    0x40098000u,  /* FC7 */
    0x4009F000u,  /* FC8 */
};

/*
 * FLEXCOMM peripheral selection register (common to all modes).
 * Offset 0xFF8 within each FLEXCOMM block.
 */
#define FC_PSELID        0xFF8u
#define PSELID_PERSEL_MASK 0x7u
#define PERSEL_NONE      0u
#define PERSEL_USART     1u
#define PERSEL_SPI       2u
#define PERSEL_I2C       3u
#define PERSEL_I2S       4u

/*
 * USART register offsets (within each FLEXCOMM block in USART mode).
 */
#define USART_CFG        0x000u  /* Configuration */
#define USART_CTL        0x004u  /* Control */
#define USART_STAT       0x008u  /* Status */
#define USART_INTENSET   0x00Cu  /* Interrupt enable set */
#define USART_INTENCLR   0x010u  /* Interrupt enable clear */
#define USART_BRG        0x020u  /* Baud rate generator */
#define USART_INTSTAT    0x024u  /* Interrupt status */
#define USART_OSR        0x028u  /* Oversampling selection */
#define USART_ADDR       0x02Cu  /* Address register */

/* USART CFG bit fields */
#define CFG_ENABLE       (1u << 0)

/* USART STAT bit fields */
#define USART_STAT_RXIDLE  (1u << 1)
#define USART_STAT_TXIDLE  (1u << 3)

/*
 * SPI register offsets (within each FLEXCOMM block in SPI mode).
 */
#define SPI_CFG          0x400u  /* SPI configuration */
#define SPI_STAT         0x408u  /* SPI status */

/* SPI CFG bit fields */
#define SPI_CFG_ENABLE   (1u << 0)
#define SPI_CFG_MASTER   (1u << 2)

/* SPI STAT bit fields (SPI STAT has no TX/RX ready bits; those are in FIFOSTAT) */
#define SPI_STAT_MSTIDLE (1u << 8)

/*
 * FIFO registers (shared between USART and SPI modes).
 */
#define FIFO_CFG         0xE00u  /* FIFO configuration and enable */
#define FIFO_STAT        0xE04u  /* FIFO status */
#define FIFO_TRIG        0xE08u  /* FIFO trigger settings */
#define FIFO_INTENSET    0xE10u  /* FIFO interrupt enable set */
#define FIFO_INTENCLR    0xE14u  /* FIFO interrupt enable clear */
#define FIFO_INTSTAT     0xE18u  /* FIFO interrupt status */
#define FIFO_WR          0xE20u  /* FIFO write data (TX) */
#define FIFO_RD          0xE30u  /* FIFO read data (RX) */
#define FIFO_RDNOPOP     0xE40u  /* FIFO read with no pop */

#define FC_ID            0xFFCu  /* Peripheral ID register */

/* FIFO CFG bit fields */
#define FIFOCFG_ENABLETX (1u << 0)
#define FIFOCFG_ENABLERX (1u << 1)
#define FIFOCFG_EMPTYTX  (1u << 16)
#define FIFOCFG_EMPTYRX  (1u << 17)

/* FIFO STAT bit fields (from SVD: TXEMPTY=4, TXNOTFULL=5, RXNOTEMPTY=6, RXFULL=7) */
#define FIFOSTAT_TXEMPTY    (1u << 4)
#define FIFOSTAT_TXNOTFULL  (1u << 5)
#define FIFOSTAT_RXNOTEMPTY (1u << 6)
#define FIFOSTAT_RXFULL     (1u << 7)

/* FIFO WR (SPI TX) control bits (RXIGNORE=bit22 per SVD) */
#define FIFOWR_RXIGNORE  (1u << 22)
#define FIFOWR_LEN_SHIFT 24u
#define FIFOWR_LEN_MASK  (0xFu << FIFOWR_LEN_SHIFT)

/* FIFO RD control bits */
#define FIFORD_SOT       (1u << 20)

struct flexcomm_inst {
    mm_u32  base;
    int     index;
    mm_u32  regs[FLEXCOMM_SIZE / 4u];

    /* UART state */
    struct mm_uart_io uart_io;
    char uart_label[16];

    /* SPI state */
    int     spi_bus_index;
    mm_u8   spi_last_rx;
    mm_bool spi_rx_valid;

    mm_bool initialized;
};

static struct flexcomm_inst flexcomms[FLEXCOMM_COUNT];
static mm_bool global_init_done = MM_FALSE;

static mm_u32 fc_get_mode(const struct flexcomm_inst *fc)
{
    if (fc == 0) return PERSEL_NONE;
    return fc->regs[FC_PSELID / 4u] & PSELID_PERSEL_MASK;
}

/* ---------- USART helpers ------------------------------------------------ */

static void usart_ensure_open(struct flexcomm_inst *fc)
{
    if (fc == 0 || mm_uart_io_is_open(&fc->uart_io)) return;
    if (mm_uart_io_open(&fc->uart_io, fc->base)) {
        if (mm_tui_is_active()) {
            mm_tui_attach_uart(fc->uart_label, fc->uart_io.name);
        }
    }
}

static void usart_update_fifostat(struct flexcomm_inst *fc)
{
    mm_u32 fs = fc->regs[FIFO_STAT / 4u];
    fs |= FIFOSTAT_TXNOTFULL | FIFOSTAT_TXEMPTY;
    if (mm_uart_io_has_rx(&fc->uart_io)) {
        fs |= FIFOSTAT_RXNOTEMPTY;
    } else {
        fs &= ~(FIFOSTAT_RXNOTEMPTY | FIFOSTAT_RXFULL);
    }
    fc->regs[FIFO_STAT / 4u] = fs;
    fc->regs[USART_STAT / 4u] |= USART_STAT_TXIDLE | USART_STAT_RXIDLE;
}

static mm_bool usart_read(struct flexcomm_inst *fc, mm_u32 offset,
                          mm_u32 size_bytes, mm_u32 *value_out)
{
    if (offset == FIFO_RD && size_bytes == 4u) {
        mm_u32 v = 0;
        if (mm_uart_io_has_rx(&fc->uart_io)) {
            if (mmio_peek_mode()) {
                v = (mm_u32)mm_uart_io_peek(&fc->uart_io);
            } else {
                v = (mm_u32)mm_uart_io_read(&fc->uart_io);
                fc->regs[FIFO_STAT / 4u] &= ~FIFOSTAT_RXNOTEMPTY;
            }
        }
        *value_out = v;
        return MM_TRUE;
    }
    if (offset == FIFO_RDNOPOP && size_bytes == 4u) {
        mm_u32 v = 0;
        if (mm_uart_io_has_rx(&fc->uart_io))
            v = (mm_u32)mm_uart_io_peek(&fc->uart_io);
        *value_out = v;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)fc->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool usart_write(struct flexcomm_inst *fc, mm_u32 offset,
                           mm_u32 size_bytes, mm_u32 value)
{
    if (offset == FIFO_WR && size_bytes == 4u) {
        mm_u32 cfg = fc->regs[USART_CFG / 4u];
        if ((cfg & CFG_ENABLE) != 0u) {
            usart_ensure_open(fc);
            mm_uart_io_queue_tx(&fc->uart_io, (mm_u8)(value & 0xFFu));
            (void)mm_uart_io_flush(&fc->uart_io);
        }
        return MM_TRUE;
    }
    if (offset == USART_CFG && size_bytes == 4u) {
        mm_u32 prev = fc->regs[USART_CFG / 4u];
        memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
        if (((prev | value) & CFG_ENABLE) != 0u)
            usart_ensure_open(fc);
        return MM_TRUE;
    }
    if (offset == FIFO_CFG && size_bytes == 4u) {
        fc->regs[FIFO_CFG / 4u] = value & ~(FIFOCFG_EMPTYTX | FIFOCFG_EMPTYRX);
        return MM_TRUE;
    }
    memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* ---------- SPI helpers -------------------------------------------------- */

static void spi_update_fifostat(struct flexcomm_inst *fc)
{
    mm_u32 fs = fc->regs[FIFO_STAT / 4u];
    fs |= FIFOSTAT_TXNOTFULL | FIFOSTAT_TXEMPTY;
    if (fc->spi_rx_valid) {
        fs |= FIFOSTAT_RXNOTEMPTY;
    } else {
        fs &= ~(FIFOSTAT_RXNOTEMPTY | FIFOSTAT_RXFULL);
    }
    fc->regs[FIFO_STAT / 4u] = fs;
    /* SPI_STAT: MSTIDLE when no transfer pending; TX/RX ready is tracked via FIFOSTAT only */
    fc->regs[SPI_STAT / 4u] |= SPI_STAT_MSTIDLE;
}

static mm_bool spi_read(struct flexcomm_inst *fc, mm_u32 offset,
                        mm_u32 size_bytes, mm_u32 *value_out)
{
    if (offset == FIFO_RD && size_bytes == 4u) {
        mm_u32 v = fc->spi_rx_valid ? (mm_u32)fc->spi_last_rx : 0u;
        if (!mmio_peek_mode()) {
            fc->spi_rx_valid = MM_FALSE;
            fc->regs[FIFO_STAT / 4u] &= ~FIFOSTAT_RXNOTEMPTY;
        }
        *value_out = v;
        return MM_TRUE;
    }
    if (offset == FIFO_RDNOPOP && size_bytes == 4u) {
        *value_out = fc->spi_rx_valid ? (mm_u32)fc->spi_last_rx : 0u;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)fc->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool spi_write(struct flexcomm_inst *fc, mm_u32 offset,
                         mm_u32 size_bytes, mm_u32 value)
{
    if (offset == FIFO_WR && size_bytes == 4u) {
        mm_u8 out = (mm_u8)(value & 0xFFu);
        mm_u8 in  = 0xFFu;
        if ((fc->regs[SPI_CFG / 4u] & SPI_CFG_ENABLE) != 0u) {
            if ((value & FIFOWR_RXIGNORE) == 0u) {
                in = mm_spi_bus_xfer(fc->spi_bus_index, out);
                fc->spi_last_rx = in;
                fc->spi_rx_valid = MM_TRUE;
            } else {
                (void)mm_spi_bus_xfer(fc->spi_bus_index, out);
            }
        }
        spi_update_fifostat(fc);
        return MM_TRUE;
    }
    if (offset == FIFO_CFG && size_bytes == 4u) {
        fc->regs[FIFO_CFG / 4u] = value & ~(FIFOCFG_EMPTYTX | FIFOCFG_EMPTYRX);
        return MM_TRUE;
    }
    memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
    spi_update_fifostat(fc);
    return MM_TRUE;
}

/* ---------- Unified read/write handlers ---------------------------------- */

static mm_bool fc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                       mm_u32 *value_out)
{
    struct flexcomm_inst *fc = (struct flexcomm_inst *)opaque;
    mm_u32 mode;
    if (fc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > FLEXCOMM_SIZE)
        return MM_FALSE;
    if (!fc_periph_active(fc->index))
        return MM_FALSE;

    /* PSELID and ID are always accessible */
    if (offset == FC_PSELID && size_bytes == 4u) {
        memcpy(value_out, (mm_u8 *)fc->regs + offset, size_bytes);
        return MM_TRUE;
    }
    if (offset == FC_ID && size_bytes == 4u) {
        *value_out = fc->regs[FC_ID / 4u];
        return MM_TRUE;
    }

    mode = fc_get_mode(fc);
    switch (mode) {
    case PERSEL_USART: return usart_read(fc, offset, size_bytes, value_out);
    case PERSEL_SPI:   return spi_read(fc, offset, size_bytes, value_out);
    default:
        memcpy(value_out, (mm_u8 *)fc->regs + offset, size_bytes);
        return MM_TRUE;
    }
}

static mm_bool fc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 value)
{
    struct flexcomm_inst *fc = (struct flexcomm_inst *)opaque;
    mm_u32 mode;
    if (fc == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > FLEXCOMM_SIZE)
        return MM_FALSE;
    if (!fc_periph_active(fc->index))
        return MM_FALSE;

    /* PSELID write selects peripheral mode */
    if (offset == FC_PSELID && size_bytes == 4u) {
        mm_u32 old_mode = fc_get_mode(fc);
        mm_u32 new_mode = value & PSELID_PERSEL_MASK;
        fc->regs[FC_PSELID / 4u] = value;
        if (old_mode != new_mode && !fc->initialized) {
            if (new_mode == PERSEL_USART) {
                fc->regs[USART_STAT / 4u] = USART_STAT_TXIDLE | USART_STAT_RXIDLE;
                fc->regs[FIFO_STAT / 4u]  = FIFOSTAT_TXNOTFULL | FIFOSTAT_TXEMPTY;
                fc->initialized = MM_TRUE;
            } else if (new_mode == PERSEL_SPI) {
                fc->regs[SPI_STAT / 4u]  = SPI_STAT_MSTIDLE;
                fc->regs[FIFO_STAT / 4u] = FIFOSTAT_TXNOTFULL | FIFOSTAT_TXEMPTY;
                fc->initialized = MM_TRUE;
            }
        }
        return MM_TRUE;
    }

    mode = fc_get_mode(fc);
    switch (mode) {
    case PERSEL_USART: return usart_write(fc, offset, size_bytes, value);
    case PERSEL_SPI:   return spi_write(fc, offset, size_bytes, value);
    default:
        memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
        return MM_TRUE;
    }
}

/* ---------- Public interface --------------------------------------------- */

void mm_lpc55s69_flexcomm_poll(void)
{
    int i;
    for (i = 0; i < FLEXCOMM_COUNT; ++i) {
        struct flexcomm_inst *fc = &flexcomms[i];
        mm_u32 mode = fc_get_mode(fc);
        if (mode == PERSEL_USART) {
            if (mm_uart_io_poll(&fc->uart_io))
                usart_update_fifostat(fc);
        } else if (mode == PERSEL_SPI) {
            spi_update_fifostat(fc);
        }
    }
}

void mm_lpc55s69_flexcomm_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    int i;
    if (global_init_done && mmio_bus_maps(bus, flexcomm_bases[0])) return;
    global_init_done = MM_TRUE;
    (void)nvic;

    for (i = 0; i < FLEXCOMM_COUNT; ++i) {
        struct flexcomm_inst *fc = &flexcomms[i];
        struct mmio_region reg;

        memset(fc, 0, sizeof(*fc));
        fc->base          = flexcomm_bases[i];
        fc->index         = i;
        fc->spi_bus_index = i;
        mm_uart_io_init(&fc->uart_io);
        sprintf(fc->uart_label, "USART%d", i);
        mm_uart_instance_register(fc->base, fc->uart_label);

        /*
         * PSELID reset per SVD: ID field (bits[31:12]) = 0x101,
         * PERSEL=0 (no mode selected).
         * Capability bits (read-only hardware configuration):
         *   FC0-FC7: USARTPRESENT(4) | SPIPRESENT(5) | I2CPRESENT(6) | I2SPRESENT(7)
         *   FC8 (HS_LSPI): SPIPRESENT(5) only
         */
        if (i < 8)
            fc->regs[FC_PSELID / 4u] = 0x001010F0u; /* all 4 functions present */
        else
            fc->regs[FC_PSELID / 4u] = 0x00101020u; /* SPI only */
        fc->regs[FC_ID / 4u]     = 0xE0100000u + (mm_u32)i;

        reg.base  = fc->base;
        reg.size  = FLEXCOMM_SIZE;
        reg.opaque = fc;
        reg.read  = fc_read;
        reg.write = fc_write;
        mmio_bus_register_region(bus, &reg);

        /* Secure alias */
        reg.base = fc->base + 0x10000000u;
        mmio_bus_register_region(bus, &reg);
    }
}

void mm_lpc55s69_flexcomm_reset(void)
{
    int i;
    for (i = 0; i < FLEXCOMM_COUNT; ++i) {
        mm_uart_io_close(&flexcomms[i].uart_io);
        memset(&flexcomms[i], 0, sizeof(flexcomms[i]));
    }
    global_init_done = MM_FALSE;
}
