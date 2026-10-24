/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include <stdio.h>
#include "pic32ck/pic32ck_sercom.h"
#include "pic32ck/pic32ck_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/target_hal.h"
#include "m33mu/spi_bus.h"

/* 8 SERCOM instances */
/* SERCOM0-3: 0x44810000+0x2000*n (APB-A) */
/* SERCOM4-7: 0x45000000+0x2000*(n-4) (APB-B) */
#define SERCOM_COUNT  8
#define SERCOM_SIZE   0x2000u

/* SERCOM register offsets */
#define SERCOM_CTRLA      0x00u
#define SERCOM_CTRLB      0x04u
#define SERCOM_CTRLC      0x08u
#define SERCOM_BAUD       0x0Cu
#define SERCOM_RXPL       0x0Eu
#define SERCOM_INTENCLR   0x14u
#define SERCOM_INTENSET   0x16u
#define SERCOM_INTFLAG    0x18u
#define SERCOM_STATUS     0x1Au
#define SERCOM_SYNCBUSY   0x1Cu
#define SERCOM_DATA       0x28u

/* CTRLA MODE field bits [4:2] */
#define CTRLA_MODE_SHIFT  2u
#define CTRLA_MODE_MASK   (0x7u << CTRLA_MODE_SHIFT)
#define CTRLA_MODE_USART  1u   /* USART with internal clock */
#define CTRLA_MODE_SPI    3u   /* SPI master */
#define CTRLA_ENABLE      (1u << 1)

/* INTFLAG / INTENSET bits */
#define INTFLAG_DRE   (1u << 0)   /* Data Register Empty (TX ready) */
#define INTFLAG_TXC   (1u << 1)   /* TX Complete */
#define INTFLAG_RXC   (1u << 2)   /* RX Complete (data available) */

static const mm_u32 sercom_bases[SERCOM_COUNT] = {
    0x44810000u,  /* SERCOM0 */
    0x44812000u,  /* SERCOM1 */
    0x44814000u,  /* SERCOM2 */
    0x44816000u,  /* SERCOM3 */
    0x45000000u,  /* SERCOM4 */
    0x45002000u,  /* SERCOM5 */
    0x45004000u,  /* SERCOM6 */
    0x45006000u,  /* SERCOM7 */
};

struct sercom_inst {
    mm_u32  base;
    int     index;
    mm_u32  regs[SERCOM_SIZE / 4u];
    struct mm_uart_io uart_io;
    char    uart_label[16];
    int     spi_bus_index;
    mm_u8   spi_last_rx;
    mm_bool spi_rx_valid;
};

static struct sercom_inst sercomms[SERCOM_COUNT];
static mm_bool global_init_done;

static mm_u32 sercom_get_mode(const struct sercom_inst *sc)
{
    return (sc->regs[SERCOM_CTRLA / 4u] >> CTRLA_MODE_SHIFT) & 0x7u;
}

/* --- USART helpers --- */

static void usart_ensure_open(struct sercom_inst *sc)
{
    if (mm_uart_io_is_open(&sc->uart_io)) return;
    if (mm_uart_io_open(&sc->uart_io, sc->base)) {
        if (mm_tui_is_active()) {
            mm_tui_attach_uart(sc->uart_label, sc->uart_io.name);
        }
    }
}

static void usart_update_flags(struct sercom_inst *sc)
{
    mm_u32 flags = INTFLAG_DRE | INTFLAG_TXC;
    if (mm_uart_io_has_rx(&sc->uart_io)) flags |= INTFLAG_RXC;
    else flags &= ~INTFLAG_RXC;
    memcpy((mm_u8 *)sc->regs + SERCOM_INTFLAG, &flags,
           sizeof(mm_u16));
}

static mm_bool usart_read(struct sercom_inst *sc, mm_u32 offset,
                          mm_u32 size_bytes, mm_u32 *value_out)
{
    if (offset == SERCOM_DATA && size_bytes <= 4u) {
        mm_u32 v = 0;
        if (mm_uart_io_has_rx(&sc->uart_io)) {
            if (mmio_peek_mode())
                v = (mm_u32)mm_uart_io_peek(&sc->uart_io);
            else
                v = (mm_u32)mm_uart_io_read(&sc->uart_io);
        }
        memcpy(value_out, &v, size_bytes);
        return MM_TRUE;
    }
    if (offset == SERCOM_INTFLAG && size_bytes <= 4u) {
        usart_update_flags(sc);
    }
    memcpy(value_out, (mm_u8 *)sc->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool usart_write(struct sercom_inst *sc, mm_u32 offset,
                           mm_u32 size_bytes, mm_u32 value)
{
    if (offset == SERCOM_INTFLAG && size_bytes <= 4u) {
        mm_u32 cur = 0u;
        memcpy(&cur, (mm_u8 *)sc->regs + SERCOM_INTFLAG, size_bytes);
        cur &= ~value;
        memcpy((mm_u8 *)sc->regs + SERCOM_INTFLAG, &cur, size_bytes);
        return MM_TRUE;
    }
    if (offset == SERCOM_DATA && size_bytes <= 4u) {
        mm_u32 ctrla;
        memcpy(&ctrla, (mm_u8 *)sc->regs + SERCOM_CTRLA, 4u);
        if ((ctrla & CTRLA_ENABLE) != 0u) {
            usart_ensure_open(sc);
            mm_uart_io_queue_tx(&sc->uart_io, (mm_u8)(value & 0xFFu));
            mm_uart_io_flush(&sc->uart_io);
        }
        return MM_TRUE;
    }
    if (offset == SERCOM_CTRLA && size_bytes == 4u) {
        memcpy((mm_u8 *)sc->regs + offset, &value, size_bytes);
        if ((value & CTRLA_ENABLE) != 0u)
            usart_ensure_open(sc);
        return MM_TRUE;
    }
    memcpy((mm_u8 *)sc->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* --- SPI helpers --- */

static mm_bool spi_read(struct sercom_inst *sc, mm_u32 offset,
                        mm_u32 size_bytes, mm_u32 *value_out)
{
    if (offset == SERCOM_DATA && size_bytes <= 4u) {
        mm_u32 v = sc->spi_rx_valid ? (mm_u32)sc->spi_last_rx : 0u;
        if (!mmio_peek_mode()) sc->spi_rx_valid = MM_FALSE;
        memcpy(value_out, &v, size_bytes);
        return MM_TRUE;
    }
    if (offset == SERCOM_INTFLAG && size_bytes <= 4u) {
        mm_u32 flags = INTFLAG_DRE | INTFLAG_TXC;
        if (sc->spi_rx_valid) flags |= INTFLAG_RXC;
        memcpy(value_out, &flags, size_bytes);
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)sc->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool spi_write(struct sercom_inst *sc, mm_u32 offset,
                         mm_u32 size_bytes, mm_u32 value)
{
    if (offset == SERCOM_INTFLAG && size_bytes <= 4u) {
        mm_u32 cur = 0u;
        memcpy(&cur, (mm_u8 *)sc->regs + SERCOM_INTFLAG, size_bytes);
        cur &= ~value;
        memcpy((mm_u8 *)sc->regs + SERCOM_INTFLAG, &cur, size_bytes);
        return MM_TRUE;
    }
    if (offset == SERCOM_DATA && size_bytes <= 4u) {
        mm_u32 ctrla;
        memcpy(&ctrla, (mm_u8 *)sc->regs + SERCOM_CTRLA, 4u);
        if ((ctrla & CTRLA_ENABLE) != 0u) {
            mm_u8 rx = mm_spi_bus_xfer(sc->spi_bus_index,
                                       (mm_u8)(value & 0xFFu));
            sc->spi_last_rx  = rx;
            sc->spi_rx_valid = MM_TRUE;
        }
        return MM_TRUE;
    }
    memcpy((mm_u8 *)sc->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* --- Unified read/write --- */

static mm_bool sc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                       mm_u32 *value_out)
{
    struct sercom_inst *sc = (struct sercom_inst *)opaque;
    if (sc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > SERCOM_SIZE) return MM_FALSE;
    /* SYNCBUSY always 0 (no sync delays) */
    if (offset == SERCOM_SYNCBUSY && size_bytes == 4u) {
        *value_out = 0;
        return MM_TRUE;
    }
    switch (sercom_get_mode(sc)) {
    case CTRLA_MODE_USART: return usart_read(sc, offset, size_bytes, value_out);
    case CTRLA_MODE_SPI:   return spi_read(sc, offset, size_bytes, value_out);
    default:
        memcpy(value_out, (mm_u8 *)sc->regs + offset, size_bytes);
        return MM_TRUE;
    }
}

static mm_bool sc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 value)
{
    struct sercom_inst *sc = (struct sercom_inst *)opaque;
    if (sc == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SERCOM_SIZE) return MM_FALSE;
    switch (sercom_get_mode(sc)) {
    case CTRLA_MODE_USART: return usart_write(sc, offset, size_bytes, value);
    case CTRLA_MODE_SPI:   return spi_write(sc, offset, size_bytes, value);
    default:
        memcpy((mm_u8 *)sc->regs + offset, &value, size_bytes);
        return MM_TRUE;
    }
}

/* --- Public interface --- */

void mm_pic32ck_sercom_poll(void)
{
    int i;
    for (i = 0; i < SERCOM_COUNT; ++i) {
        struct sercom_inst *sc = &sercomms[i];
        mm_u32 mode = sercom_get_mode(sc);
        if (mode == CTRLA_MODE_USART)
            mm_uart_io_poll(&sc->uart_io);
    }
}

void mm_pic32ck_sercom_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    int i;
    if (global_init_done && mmio_bus_maps(bus, sercom_bases[0])) return;
    global_init_done = MM_TRUE;
    (void)nvic;

    for (i = 0; i < SERCOM_COUNT; ++i) {
        struct sercom_inst *sc = &sercomms[i];
        struct mmio_region reg;
        memset(sc, 0, sizeof(*sc));
        sc->base          = sercom_bases[i];
        sc->index         = i;
        sc->spi_bus_index = i;
        mm_uart_io_init(&sc->uart_io);
        sprintf(sc->uart_label, "SERCOM%d", i);
        mm_uart_instance_register(sc->base, sc->uart_label);
        /* INTFLAG: DRE|TXC set at reset (TX ready) */
        {
            mm_u16 fl = INTFLAG_DRE | INTFLAG_TXC;
            memcpy((mm_u8 *)sc->regs + SERCOM_INTFLAG, &fl, sizeof fl);
        }
        reg.base   = sc->base;
        reg.size   = SERCOM_SIZE;
        reg.opaque = sc;
        reg.read   = sc_read;
        reg.write  = sc_write;
        mmio_bus_register_region(bus, &reg);
    }
}

void mm_pic32ck_sercom_reset(void)
{
    int i;
    for (i = 0; i < SERCOM_COUNT; ++i)
        mm_uart_io_close(&sercomms[i].uart_io);
    memset(sercomms, 0, sizeof(sercomms));
    global_init_done = MM_FALSE;
}
