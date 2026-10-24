/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "mcxn947/mcxn947_flexcomm.h"
#include "mcxn947/mcxn947_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/target_hal.h"
#include "m33mu/spi_bus.h"

/* FLEXCOMM base addresses: LP_FLEXCOMM0 through LP_FLEXCOMM9 */
#define FLEXCOMM_COUNT 10
#define FLEXCOMM_BASE_START 0x40092000u
#define FLEXCOMM_BASE_STRIDE 0x1000u
#define FLEXCOMM_SIZE 0x1000u
#define FLEXCOMM_LPSPI_BASE_START 0x400B4000u
#define FLEXCOMM_LPSPI_BASE_STRIDE 0x1000u

/* FLEXCOMM mode selection register */
#define FLEXCOMM_PSELID 0xFF8u
#define PSELID_PERSEL_MASK 0x7u
#define PERSEL_NONE 0
#define PERSEL_UART 1
#define PERSEL_SPI  2
#define PERSEL_I2C  3

/* LPUART register offsets (when in UART mode) */
#define LPUART_STAT 0x14u
#define LPUART_CTRL 0x18u
#define LPUART_DATA 0x1Cu

#define CTRL_RE (1u << 18)
#define CTRL_TE (1u << 19)

#define STAT_IDLE (1u << 20)
#define STAT_RDRF (1u << 21)
#define STAT_TC   (1u << 22)
#define STAT_TDRE (1u << 23)

/* LPSPI register offsets (when in SPI mode) */
#define LPSPI_CR   0x10u
#define LPSPI_SR   0x14u
#define LPSPI_TCR  0x60u
#define LPSPI_TDR  0x64u
#define LPSPI_RDR  0x74u

#define CR_MEN   (1u << 0)
#define CR_RRF   (1u << 9)

#define SR_TDF   (1u << 0)
#define SR_RDF   (1u << 1)
#define SR_TCF   (1u << 10)

#define TCR_FRAMESZ_MASK 0x1fu
#define TCR_TXMSK (1u << 18)
#define TCR_RXMSK (1u << 19)
#define TCR_CONT  (1u << 21)

/* SYSCON AHBCLKCTRL0 / PRESETCTRL0 bit positions for FLEXCOMM0-9 */
#define SYSCON_FC0_BIT 11

struct flexcomm_inst {
    mm_u32 base;
    mm_u32 regs[FLEXCOMM_SIZE / 4];
    int index;

    /* UART state */
    struct mm_uart_io uart_io;
    char uart_label[16];

    /* SPI state */
    int spi_bus_index;
    mm_u8 spi_last_rx;
    mm_bool spi_rx_valid;

    mm_bool initialized;
};

static struct flexcomm_inst flexcomms[FLEXCOMM_COUNT];
static size_t flexcomm_count = 0;
static mm_bool global_init_done = MM_FALSE;

static mm_u32 flexcomm_get_mode(const struct flexcomm_inst *fc)
{
    if (fc == 0) return PERSEL_NONE;
    return fc->regs[FLEXCOMM_PSELID / 4] & PSELID_PERSEL_MASK;
}

static mm_bool flexcomm_clocked(const struct flexcomm_inst *fc)
{
    if (fc == 0) return MM_FALSE;

    /* Check AHBCLKCTRL0 bit for FLEXCOMMx (bits 11-20 for FC0-FC9) */
    if (!mm_mcxn947_syscon_clock_on(0x200u)) return MM_FALSE;

    /* Check PRESETCTRL0 bit for FLEXCOMMx reset release */
    if (!mm_mcxn947_syscon_reset_released(0x100u)) return MM_FALSE;

    return MM_TRUE;
}

static void flexcomm_uart_ensure_open(struct flexcomm_inst *fc)
{
    if (fc == 0) return;
    if (fc->uart_io.fd >= 0) return;
    if (mm_uart_io_open(&fc->uart_io, fc->base)) {
        if (mm_tui_is_active()) {
            mm_tui_attach_uart(fc->uart_label, fc->uart_io.name);
        }
    }
}

/* UART-specific functions */
static void uart_update_status(struct flexcomm_inst *fc)
{
    if (fc == 0) return;
    if (mm_uart_io_tx_empty(&fc->uart_io)) {
        fc->regs[LPUART_STAT / 4] |= STAT_TDRE | STAT_TC;
    }
    if (mm_uart_io_has_rx(&fc->uart_io)) {
        fc->regs[LPUART_STAT / 4] |= STAT_RDRF;
    }
}

static mm_bool uart_read(struct flexcomm_inst *fc, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    if (offset == LPUART_DATA && size_bytes == 4) {
        mm_u32 v = 0;
        if (mm_uart_io_has_rx(&fc->uart_io)) {
            if (mmio_peek_mode()) {
                v = (mm_u32)mm_uart_io_peek(&fc->uart_io);
            } else {
                v = (mm_u32)mm_uart_io_read(&fc->uart_io);
            }
        }
        if (!mmio_peek_mode()) {
            fc->regs[LPUART_STAT / 4] &= ~STAT_RDRF;
        }
        *value_out = v;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)fc->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool uart_write(struct flexcomm_inst *fc, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    if (offset == LPUART_DATA && size_bytes == 4) {
        mm_u32 ctrl = fc->regs[LPUART_CTRL / 4];
        if ((ctrl & CTRL_TE) != 0u) {
            flexcomm_uart_ensure_open(fc);
            mm_uart_io_queue_tx(&fc->uart_io, (mm_u8)(value & 0xffu));
            (void)mm_uart_io_flush(&fc->uart_io);
            fc->regs[LPUART_STAT / 4] |= STAT_TDRE | STAT_TC;
        }
        return MM_TRUE;
    }
    memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* SPI-specific functions */
static void spi_update_sr(struct flexcomm_inst *fc)
{
    mm_u32 sr = fc->regs[LPSPI_SR / 4];
    sr |= SR_TDF;
    if (fc->spi_rx_valid) sr |= SR_RDF;
    fc->regs[LPSPI_SR / 4] = sr;
}

static mm_bool spi_read(struct flexcomm_inst *fc, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    if (offset == LPSPI_RDR && size_bytes == 4) {
        mm_u32 v = fc->spi_rx_valid ? (mm_u32)fc->spi_last_rx : 0u;
        if (!mmio_peek_mode()) {
            fc->spi_rx_valid = MM_FALSE;
            fc->regs[LPSPI_SR / 4] &= ~SR_RDF;
        }
        *value_out = v;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)fc->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool spi_write(struct flexcomm_inst *fc, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 tcr;

    if (offset == LPSPI_CR && size_bytes == 4) {
        fc->regs[LPSPI_CR / 4] = value;
        if ((value & CR_RRF) != 0u) {
            fc->spi_rx_valid = MM_FALSE;
            fc->regs[LPSPI_SR / 4] &= ~SR_RDF;
        }
        if ((value & CR_MEN) == 0u) {
            mm_spi_bus_end(fc->spi_bus_index);
        }
        spi_update_sr(fc);
        return MM_TRUE;
    }

    if (offset == LPSPI_TDR && size_bytes == 4) {
        mm_u8 out = (mm_u8)(value & 0xffu);
        mm_u8 in = 0xffu;
        if ((fc->regs[LPSPI_CR / 4] & CR_MEN) != 0u) {
            tcr = fc->regs[LPSPI_TCR / 4];
            if ((tcr & TCR_TXMSK) == 0u) {
                in = mm_spi_bus_xfer(fc->spi_bus_index, out);
            }
            if ((tcr & TCR_RXMSK) == 0u) {
                fc->spi_last_rx = in;
                fc->spi_rx_valid = MM_TRUE;
                fc->regs[LPSPI_SR / 4] |= SR_RDF;
            }
            fc->regs[LPSPI_SR / 4] |= SR_TCF;
            if ((tcr & TCR_CONT) == 0u) {
                mm_spi_bus_end(fc->spi_bus_index);
            }
        }
        spi_update_sr(fc);
        return MM_TRUE;
    }

    memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
    spi_update_sr(fc);
    return MM_TRUE;
}

/* Unified FLEXCOMM read/write handlers */
static mm_bool flexcomm_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct flexcomm_inst *fc = (struct flexcomm_inst *)opaque;
    mm_u32 mode;

    if (fc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!flexcomm_clocked(fc)) return MM_FALSE;
    if ((offset + size_bytes) > FLEXCOMM_SIZE) return MM_FALSE;

    mode = flexcomm_get_mode(fc);

    /* PSELID register is always accessible */
    if (offset == FLEXCOMM_PSELID && size_bytes == 4) {
        memcpy(value_out, (mm_u8 *)fc->regs + offset, size_bytes);
        return MM_TRUE;
    }

    /* Dispatch based on mode */
    switch (mode) {
    case PERSEL_UART:
        return uart_read(fc, offset, size_bytes, value_out);
    case PERSEL_SPI:
        return spi_read(fc, offset, size_bytes, value_out);
    case PERSEL_I2C:
        /* I2C not implemented yet - return zeros */
        *value_out = 0;
        return MM_TRUE;
    case PERSEL_NONE:
    default:
        /* Return register value or zero */
        memcpy(value_out, (mm_u8 *)fc->regs + offset, size_bytes);
        return MM_TRUE;
    }
}

static mm_bool flexcomm_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct flexcomm_inst *fc = (struct flexcomm_inst *)opaque;
    mm_u32 mode;

    if (fc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!flexcomm_clocked(fc)) return MM_FALSE;
    if ((offset + size_bytes) > FLEXCOMM_SIZE) return MM_FALSE;

    /* PSELID register write changes mode */
    if (offset == FLEXCOMM_PSELID && size_bytes == 4) {
        mm_u32 old_mode = flexcomm_get_mode(fc);
        mm_u32 new_mode = value & PSELID_PERSEL_MASK;
        fc->regs[FLEXCOMM_PSELID / 4] = value;

        /* Initialize mode-specific state when mode changes */
        if (old_mode != new_mode) {
            if (new_mode == PERSEL_UART && !fc->initialized) {
                fc->regs[LPUART_STAT / 4] = STAT_TDRE | STAT_TC | STAT_IDLE;
                fc->initialized = MM_TRUE;
            } else if (new_mode == PERSEL_SPI && !fc->initialized) {
                fc->regs[LPSPI_SR / 4] = SR_TDF;
                fc->initialized = MM_TRUE;
            }
        }
        return MM_TRUE;
    }

    mode = flexcomm_get_mode(fc);

    /* Dispatch based on mode */
    switch (mode) {
    case PERSEL_UART:
        if (offset == LPUART_CTRL && size_bytes == 4) {
            mm_u32 prev = fc->regs[LPUART_CTRL / 4];
            memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
            if (((prev | value) & (CTRL_RE | CTRL_TE)) != 0u) {
                flexcomm_uart_ensure_open(fc);
            }
            return MM_TRUE;
        }
        return uart_write(fc, offset, size_bytes, value);
    case PERSEL_SPI:
        return spi_write(fc, offset, size_bytes, value);
    case PERSEL_I2C:
        /* I2C not implemented yet - just store value */
        memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
        return MM_TRUE;
    case PERSEL_NONE:
    default:
        /* Store value in register array */
        memcpy((mm_u8 *)fc->regs + offset, &value, size_bytes);
        return MM_TRUE;
    }
}

void mm_mcxn947_flexcomm_poll(void)
{
    size_t i;
    for (i = 0; i < flexcomm_count; ++i) {
        struct flexcomm_inst *fc = &flexcomms[i];
        mm_u32 mode = flexcomm_get_mode(fc);

        if (mode == PERSEL_UART) {
            if (mm_uart_io_poll(&fc->uart_io)) {
                uart_update_status(fc);
            }
        } else if (mode == PERSEL_SPI) {
            spi_update_sr(fc);
        }
    }
}

void mm_mcxn947_flexcomm_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    size_t i;

    /* Prevent double initialization */
    if (global_init_done && mmio_bus_maps(bus, FLEXCOMM_BASE_START)) return;
    global_init_done = MM_TRUE;

    (void)nvic;
    flexcomm_count = FLEXCOMM_COUNT;

    for (i = 0; i < flexcomm_count; ++i) {
        struct flexcomm_inst *fc = &flexcomms[i];
        struct mmio_region reg;

        memset(fc, 0, sizeof(*fc));
        fc->base = FLEXCOMM_BASE_START + (i * FLEXCOMM_BASE_STRIDE);
        fc->index = (int)i;
        fc->spi_bus_index = (int)i;

        /* Initialize UART I/O */
        mm_uart_io_init(&fc->uart_io);
        sprintf(fc->uart_label, "LPUART%u", (unsigned)i);
        mm_uart_instance_register(fc->base, fc->uart_label);

        /* Set default PSELID value (from SVD reset value) */
        fc->regs[FLEXCOMM_PSELID / 4] = 0x00103070u;

        /* Register MMIO region (non-secure) */
        reg.base = fc->base;
        reg.size = FLEXCOMM_SIZE;
        reg.opaque = fc;
        reg.read = flexcomm_read;
        reg.write = flexcomm_write;
        mmio_bus_register_region(bus, &reg);

        /* Register secure alias */
        reg.base = fc->base + 0x10000000u;
        mmio_bus_register_region(bus, &reg);

        if (i >= 4) {
            mm_u32 lpspi_base = FLEXCOMM_LPSPI_BASE_START + (mm_u32)(i - 4) * FLEXCOMM_LPSPI_BASE_STRIDE;
            reg.base = lpspi_base;
            mmio_bus_register_region(bus, &reg);
            reg.base = lpspi_base + 0x10000000u;
            mmio_bus_register_region(bus, &reg);
        }
    }
}

void mm_mcxn947_flexcomm_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(flexcomms) / sizeof(flexcomms[0]); ++i) {
        mm_uart_io_close(&flexcomms[i].uart_io);
        memset(&flexcomms[i], 0, sizeof(flexcomms[i]));
    }
    flexcomm_count = 0;
    global_init_done = MM_FALSE;
}
