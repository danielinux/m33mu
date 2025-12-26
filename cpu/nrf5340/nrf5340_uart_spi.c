/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "nrf5340/nrf5340_uart_spi.h"
#include "nrf5340/nrf5340_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/spi_bus.h"
#include "m33mu/target_hal.h"

#define SERIAL_SIZE 0x1000u

#define UARTE_TASKS_STARTRX 0x000u
#define UARTE_TASKS_STOPRX  0x004u
#define UARTE_TASKS_STARTTX 0x008u
#define UARTE_TASKS_STOPTX  0x00Cu
#define UARTE_TASKS_FLUSHRX 0x02Cu

#define SPIM_TASKS_START 0x010u
#define SPIM_TASKS_STOP  0x014u
#define SPIM_TASKS_SUSPEND 0x01Cu
#define SPIM_TASKS_RESUME  0x020u

#define EVENTS_CTS     0x100u
#define EVENTS_NCTS    0x104u
#define EVENTS_RXDRDY  0x108u
#define EVENTS_ENDRX   0x110u
#define EVENTS_TXDRDY  0x11Cu
#define EVENTS_ENDTX   0x120u
#define EVENTS_ERROR   0x124u
#define EVENTS_RXTO    0x144u

#define SPIM_EVENTS_STOPPED 0x104u
#define SPIM_EVENTS_ENDRX   0x110u
#define SPIM_EVENTS_END     0x118u
#define SPIM_EVENTS_ENDTX   0x120u
#define SPIM_EVENTS_STARTED 0x14Cu

#define INTENSET 0x304u
#define INTENCLR 0x308u

#define ENABLE 0x500u
#define PSEL_SCK 0x508u
#define PSEL_MOSI 0x50Cu
#define PSEL_MISO 0x510u
#define PSEL_RTS 0x508u
#define PSEL_TXD 0x50Cu
#define PSEL_CTS 0x510u
#define PSEL_RXD 0x514u
#define BAUDRATE 0x524u

#define RXD_PTR 0x534u
#define RXD_MAXCNT 0x538u
#define RXD_AMOUNT 0x53Cu
#define TXD_PTR 0x544u
#define TXD_MAXCNT 0x548u
#define TXD_AMOUNT 0x54Cu

#define CONFIG 0x554u
#define ORC 0x5C0u

#define ENABLE_SPIM  7u
#define ENABLE_UARTE 8u

#define INT_RXDRDY (1u << 2)
#define INT_ENDRX  (1u << 4)
#define INT_TXDRDY (1u << 7)
#define INT_ENDTX  (1u << 8)
#define INT_ERROR  (1u << 9)

struct serial_inst {
    mm_u32 base;
    mm_u32 regs[SERIAL_SIZE / 4];
    mm_u32 bus_index;
    int irq;
    mm_bool has_uarte;
    mm_bool rx_running;
    struct mm_uart_io io;
    char label[16];
};

static struct serial_inst serials[5];
static mm_bool serials_init_done = MM_FALSE;
static struct mm_nvic *g_nvic = 0;

static struct mm_memmap *serial_map(void)
{
    return mm_memmap_current();
}

static mm_bool dma_read8(mm_u32 addr, mm_u8 *value_out)
{
    struct mm_memmap *map = serial_map();
    if (map == 0 || value_out == 0) return MM_FALSE;
    return mm_memmap_read8(map, mmio_active_sec(), addr, value_out);
}

static mm_bool dma_write8(mm_u32 addr, mm_u8 value)
{
    struct mm_memmap *map = serial_map();
    if (map == 0) return MM_FALSE;
    return mm_memmap_write8(map, mmio_active_sec(), addr, value);
}

static void serial_raise_irq(struct serial_inst *s, mm_u32 mask)
{
    if (s == 0 || g_nvic == 0) return;
    if ((s->regs[INTENSET / 4] & mask) == 0u) return;
    mm_nvic_set_pending(g_nvic, (mm_u32)s->irq, MM_TRUE);
}

static void serial_event_set(struct serial_inst *s, mm_u32 offset, mm_u32 mask)
{
    s->regs[offset / 4] = 1u;
    serial_raise_irq(s, mask);
}

static void serial_spim_run(struct serial_inst *s)
{
    mm_u32 tx_cnt;
    mm_u32 rx_cnt;
    mm_u32 tx_ptr;
    mm_u32 rx_ptr;
    mm_u32 i;
    mm_u32 total;
    mm_u8 orc;

    if (s == 0) return;
    if (!mm_nrf5340_clock_hf_running()) return;
    if ((s->regs[ENABLE / 4] & 0xFu) != ENABLE_SPIM) return;

    tx_cnt = s->regs[TXD_MAXCNT / 4];
    rx_cnt = s->regs[RXD_MAXCNT / 4];
    tx_ptr = s->regs[TXD_PTR / 4];
    rx_ptr = s->regs[RXD_PTR / 4];
    orc = (mm_u8)(s->regs[ORC / 4] & 0xFFu);

    total = (tx_cnt > rx_cnt) ? tx_cnt : rx_cnt;
    for (i = 0; i < total; ++i) {
        mm_u8 out = orc;
        mm_u8 in;
        if (i < tx_cnt) {
            (void)dma_read8(tx_ptr + i, &out);
        }
        in = mm_spi_bus_xfer((int)s->bus_index, out);
        if (i < rx_cnt) {
            (void)dma_write8(rx_ptr + i, in);
        }
    }

    s->regs[TXD_AMOUNT / 4] = tx_cnt;
    s->regs[RXD_AMOUNT / 4] = rx_cnt;

    serial_event_set(s, SPIM_EVENTS_ENDTX, INT_ENDTX);
    serial_event_set(s, SPIM_EVENTS_ENDRX, INT_ENDRX);
    serial_event_set(s, SPIM_EVENTS_END, INT_ENDTX | INT_ENDRX);

    mm_spi_bus_end((int)s->bus_index);
}

static void serial_uarte_try_rx(struct serial_inst *s)
{
    mm_u32 rx_cnt;
    mm_u32 rx_ptr;
    mm_u32 amount;

    if (s == 0) return;
    if (!s->rx_running) return;
    if (!mm_nrf5340_clock_hf_running()) return;
    if ((s->regs[ENABLE / 4] & 0xFu) != ENABLE_UARTE) return;

    rx_cnt = s->regs[RXD_MAXCNT / 4];
    rx_ptr = s->regs[RXD_PTR / 4];
    amount = s->regs[RXD_AMOUNT / 4];

    while (amount < rx_cnt && mm_uart_io_has_rx(&s->io)) {
        mm_u8 byte = mm_uart_io_read(&s->io);
        (void)dma_write8(rx_ptr + amount, byte);
        amount++;
        serial_event_set(s, EVENTS_RXDRDY, INT_RXDRDY);
    }

    s->regs[RXD_AMOUNT / 4] = amount;
    if (amount >= rx_cnt && rx_cnt != 0u) {
        s->rx_running = MM_FALSE;
        serial_event_set(s, EVENTS_ENDRX, INT_ENDRX);
    }
}

static void serial_uarte_start_tx(struct serial_inst *s)
{
    mm_u32 tx_cnt;
    mm_u32 tx_ptr;
    mm_u32 i;
    if (s == 0) return;
    if (!mm_nrf5340_clock_hf_running()) return;
    if ((s->regs[ENABLE / 4] & 0xFu) != ENABLE_UARTE) return;

    tx_cnt = s->regs[TXD_MAXCNT / 4];
    tx_ptr = s->regs[TXD_PTR / 4];

    for (i = 0; i < tx_cnt; ++i) {
        mm_u8 out = 0;
        (void)dma_read8(tx_ptr + i, &out);
        mm_uart_io_queue_tx(&s->io, out);
    }
    (void)mm_uart_io_flush(&s->io);

    s->regs[TXD_AMOUNT / 4] = tx_cnt;
    serial_event_set(s, EVENTS_ENDTX, INT_ENDTX);
}

static mm_bool serial_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct serial_inst *s = (struct serial_inst *)opaque;
    if (s == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SERIAL_SIZE) return MM_FALSE;

    if (size_bytes == 4) {
        if (offset == RXD_AMOUNT || offset == TXD_AMOUNT) {
            *value_out = s->regs[offset / 4];
            return MM_TRUE;
        }
    }

    *value_out = s->regs[offset / 4];
    return MM_TRUE;
}

static mm_bool serial_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct serial_inst *s = (struct serial_inst *)opaque;
    if (s == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SERIAL_SIZE) return MM_FALSE;

    if (offset == INTENSET && size_bytes == 4) {
        s->regs[INTENSET / 4] |= value;
        return MM_TRUE;
    }
    if (offset == INTENCLR && size_bytes == 4) {
        s->regs[INTENSET / 4] &= ~value;
        return MM_TRUE;
    }

    if (offset == UARTE_TASKS_STARTRX && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            s->rx_running = MM_TRUE;
            s->regs[RXD_AMOUNT / 4] = 0u;
            serial_uarte_try_rx(s);
        }
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_STOPRX && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            s->rx_running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_STARTTX && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            serial_uarte_start_tx(s);
        }
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_STOPTX && size_bytes == 4) {
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_FLUSHRX && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            serial_uarte_try_rx(s);
        }
        return MM_TRUE;
    }

    if (offset == SPIM_TASKS_START && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            serial_spim_run(s);
        }
        return MM_TRUE;
    }
    if (offset == SPIM_TASKS_STOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            mm_spi_bus_end((int)s->bus_index);
        }
        return MM_TRUE;
    }

    if (offset >= EVENTS_CTS && offset <= EVENTS_RXTO && size_bytes == 4) {
        if (value == 0u) {
            s->regs[offset / 4] = 0u;
        } else {
            s->regs[offset / 4] = value;
        }
        return MM_TRUE;
    }

    s->regs[offset / 4] = value;
    return MM_TRUE;
}

static void serial_register_all(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40008000u,
        0x40009000u,
        0x4000B000u,
        0x4000C000u,
        0x4000A000u
    };
    static const int irqs[] = { 8, 9, 11, 12, 10 };
    static const mm_bool has_uarte[] = { MM_TRUE, MM_TRUE, MM_TRUE, MM_TRUE, MM_FALSE };
    struct mmio_region reg;
    size_t i;

    if (serials_init_done) return;
    if (bus == 0) return;

    g_nvic = nvic;
    memset(&serials, 0, sizeof(serials));

    reg.size = SERIAL_SIZE;
    reg.read = serial_read;
    reg.write = serial_write;

    for (i = 0; i < sizeof(serials) / sizeof(serials[0]); ++i) {
        struct serial_inst *s = &serials[i];
        s->base = bases[i];
        s->bus_index = (mm_u32)i;
        s->irq = irqs[i];
        s->has_uarte = has_uarte[i];
        s->regs[ORC / 4] = 0xFFu;

        if (s->has_uarte) {
            mm_uart_io_init(&s->io);
            sprintf(s->label, "UARTE%u", (unsigned)i);
            if (mm_uart_io_open(&s->io, s->base)) {
                if (mm_tui_is_active()) {
                    mm_tui_attach_uart(s->label, s->io.name);
                }
            }
        }

        reg.base = bases[i];
        reg.opaque = s;
        if (!mmio_bus_register_region(bus, &reg)) return;
        reg.base = bases[i] + 0x10000000u;
        if (!mmio_bus_register_region(bus, &reg)) return;
    }

    serials_init_done = MM_TRUE;
}

static void serial_reset_all(void)
{
    size_t i;
    for (i = 0; i < sizeof(serials) / sizeof(serials[0]); ++i) {
        if (serials[i].has_uarte) {
            mm_uart_io_close(&serials[i].io);
        }
    }
    memset(&serials, 0, sizeof(serials));
    serials_init_done = MM_FALSE;
}

void mm_nrf5340_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    serial_register_all(bus, nvic);
}

void mm_nrf5340_usart_reset(void)
{
    serial_reset_all();
}

void mm_nrf5340_usart_poll(void)
{
    size_t i;
    if (!serials_init_done) return;
    for (i = 0; i < sizeof(serials) / sizeof(serials[0]); ++i) {
        struct serial_inst *s = &serials[i];
        if (!s->has_uarte) continue;
        if (mm_uart_io_poll(&s->io)) {
            serial_uarte_try_rx(s);
        }
    }
}

void mm_nrf5340_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    serial_register_all(bus, nvic);
}

void mm_nrf5340_spi_reset(void)
{
    serial_reset_all();
}

void mm_nrf5340_spi_poll(void)
{
    mm_nrf5340_usart_poll();
}
