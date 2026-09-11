/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>
#include <stdio.h>
#include "m2354/m2354_uart.h"
#include "m2354/m2354_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/target_hal.h"

/*
 * Nuvoton M2354 UART0-5 at 0x40070000 + n * 0x1000.
 *
 * Transmission is immediate, so the FIFO is modelled as permanently drained:
 * FIFOSTS reports TXFULL clear and both TX-empty flags set.  Firmware polls
 * those in a tight loop before every character, so anything else would wedge
 * the console.
 */

#define UART_COUNT      6
#define UART_SIZE       0x1000u
#define UART_BASE0      0x40070000u
#define UART_STRIDE     0x1000u

#define UART_DAT        0x00u
#define UART_INTEN      0x04u
#define UART_FIFO       0x08u
#define UART_LINE       0x0Cu
#define UART_FIFOSTS    0x18u
#define UART_INTSTS     0x1Cu
#define UART_BAUD       0x24u
#define UART_FUNCSEL    0x30u

#define UART_FIFO_RXRST     (1u << 1)
#define UART_FIFO_TXRST     (1u << 2)

#define UART_FIFOSTS_RXEMPTY  (1u << 14)
#define UART_FIFOSTS_RXFULL   (1u << 15)
#define UART_FIFOSTS_TXEMPTY  (1u << 22)
#define UART_FIFOSTS_TXFULL   (1u << 23)
#define UART_FIFOSTS_TXEMPTYF (1u << 28)

#define UART_INTEN_RDAIEN   (1u << 0)
#define UART_INTEN_THREIEN  (1u << 1)

#define UART_INTSTS_RDAIF    (1u << 0)
#define UART_INTSTS_THREIF   (1u << 1)
#define UART_INTSTS_RDAINT   (1u << 8)
#define UART_INTSTS_THREINT  (1u << 9)
#define UART_INTSTS_TXENDIF  (1u << 22)
#define UART_INTSTS_TXENDINT (1u << 30)

/* CLK_APBCLK0 UARTnCKEN starts at bit 16 */
#define UART_CKEN_BIT0  16u

static const mm_u32 uart_irqs[UART_COUNT] = { 36u, 37u, 48u, 49u, 74u, 75u };

struct uart_inst {
    mm_u32 base;
    int    index;
    mm_u32 regs[UART_SIZE / 4u];
    struct mm_uart_io io;
    char   label[16];
};

static struct uart_inst uarts[UART_COUNT];
static struct mm_nvic *g_nvic;
static mm_bool global_init_done;

static mm_bool uart_clock_on(const struct uart_inst *u)
{
    return mm_m2354_apb_periph_active(0u, UART_CKEN_BIT0 + (mm_u32)u->index);
}

static void uart_ensure_open(struct uart_inst *u)
{
    if (u->io.fd >= 0) return;
    if (mm_uart_io_open(&u->io, u->base)) {
        if (mm_tui_is_active()) mm_tui_attach_uart(u->label, u->io.name);
    }
}

static mm_u32 uart_fifosts(struct uart_inst *u)
{
    mm_u32 v = UART_FIFOSTS_TXEMPTY | UART_FIFOSTS_TXEMPTYF;
    if (mm_uart_io_has_rx(&u->io)) v |= UART_FIFOSTS_RXFULL;
    else                           v |= UART_FIFOSTS_RXEMPTY;
    return v;
}

static mm_u32 uart_intsts(struct uart_inst *u)
{
    mm_u32 inten = u->regs[UART_INTEN / 4u];
    mm_u32 v = UART_INTSTS_THREIF | UART_INTSTS_TXENDIF;

    if (mm_uart_io_has_rx(&u->io)) v |= UART_INTSTS_RDAIF;
    if ((v & UART_INTSTS_RDAIF) != 0u && (inten & UART_INTEN_RDAIEN) != 0u)
        v |= UART_INTSTS_RDAINT;
    if ((inten & UART_INTEN_THREIEN) != 0u)
        v |= UART_INTSTS_THREINT | UART_INTSTS_TXENDINT;
    return v;
}

static void uart_update_irq(struct uart_inst *u)
{
    mm_u32 sts;
    if (g_nvic == 0) return;
    sts = uart_intsts(u);
    if ((sts & (UART_INTSTS_RDAINT | UART_INTSTS_THREINT)) != 0u)
        mm_nvic_set_pending(g_nvic, uart_irqs[u->index], MM_TRUE);
}

static mm_bool uart_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    struct uart_inst *u = (struct uart_inst *)opaque;
    if (u == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u)
        return MM_FALSE;
    if ((offset + size_bytes) > UART_SIZE) return MM_FALSE;

    if (offset == UART_DAT) {
        mm_u32 v = 0u;
        if (mm_uart_io_has_rx(&u->io)) {
            v = mmio_peek_mode() ? (mm_u32)mm_uart_io_peek(&u->io)
                                 : (mm_u32)mm_uart_io_read(&u->io);
        }
        *value_out = v;
        return MM_TRUE;
    }
    if (offset == UART_FIFOSTS) {
        *value_out = uart_fifosts(u);
        return MM_TRUE;
    }
    if (offset == UART_INTSTS) {
        *value_out = uart_intsts(u);
        return MM_TRUE;
    }
    *value_out = 0u;
    memcpy(value_out, (mm_u8 *)u->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool uart_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct uart_inst *u = (struct uart_inst *)opaque;
    if (u == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > UART_SIZE) return MM_FALSE;

    if (offset == UART_DAT) {
        /* An unclocked UART is silent on hardware; do the same here rather
         * than hiding a missing CLK_APBCLK0 write. */
        if (uart_clock_on(u)) {
            uart_ensure_open(u);
            mm_uart_io_queue_tx(&u->io, (mm_u8)(value & 0xFFu));
            mm_uart_io_flush(&u->io);
        }
        return MM_TRUE;
    }
    if (offset == UART_FIFO) {
        /* RXRST/TXRST are self-clearing. */
        u->regs[UART_FIFO / 4u] = value & ~(UART_FIFO_RXRST | UART_FIFO_TXRST);
        return MM_TRUE;
    }
    if (offset == UART_FIFOSTS || offset == UART_INTSTS) {
        /* Both are derived; the write-1-to-clear error bits never set here. */
        return MM_TRUE;
    }
    memcpy((mm_u8 *)u->regs + offset, &value, size_bytes);
    if (offset == UART_INTEN) uart_update_irq(u);
    return MM_TRUE;
}

void mm_m2354_uart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    int i;

    if (global_init_done) return;
    global_init_done = MM_TRUE;
    g_nvic = nvic;

    for (i = 0; i < UART_COUNT; ++i) {
        struct uart_inst *u = &uarts[i];
        memset(u, 0, sizeof(*u));
        u->base  = UART_BASE0 + (mm_u32)i * UART_STRIDE;
        u->index = i;
        mm_uart_io_init(&u->io);
        sprintf(u->label, "UART%d", i);
        u->regs[UART_BAUD / 4u] = 0x0F000000u;
        mm_m2354_register_aliased(bus, u->base, UART_SIZE, u,
                                  uart_read, uart_write);
    }
}

void mm_m2354_uart_reset(void)
{
    int i;
    for (i = 0; i < UART_COUNT; ++i) mm_uart_io_close(&uarts[i].io);
    memset(uarts, 0, sizeof(uarts));
    g_nvic = 0;
    global_init_done = MM_FALSE;
}

void mm_m2354_uart_poll(void)
{
    int i;
    for (i = 0; i < UART_COUNT; ++i) {
        struct uart_inst *u = &uarts[i];
        if (u->io.fd < 0) continue;
        mm_uart_io_poll(&u->io);
        uart_update_irq(u);
    }
}
