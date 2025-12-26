/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "mcxw71c/mcxw71c_usart.h"
#include "mcxw71c/mcxw71c_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/target_hal.h"

#define LPUART_STAT 0x14u
#define LPUART_CTRL 0x18u
#define LPUART_DATA 0x1Cu

#define CTRL_RE (1u << 18)
#define CTRL_TE (1u << 19)

#define STAT_IDLE (1u << 20)
#define STAT_RDRF (1u << 21)
#define STAT_TC   (1u << 22)
#define STAT_TDRE (1u << 23)

struct lpuart_inst {
    mm_u32 base;
    mm_u32 regs[0x40 / 4];
    struct mm_uart_io io;
    char label[16];
    mm_bool enabled;
    mm_u32 mrcc_offset;
};

static struct lpuart_inst uarts[2];
static size_t uart_count = 0;

static void update_status(struct lpuart_inst *u)
{
    if (u == 0) return;
    if (mm_uart_io_tx_empty(&u->io)) {
        u->regs[LPUART_STAT / 4] |= STAT_TDRE | STAT_TC;
    }
    if (mm_uart_io_has_rx(&u->io)) {
        u->regs[LPUART_STAT / 4] |= STAT_RDRF;
    }
}

static mm_bool uart_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct lpuart_inst *u = (struct lpuart_inst *)opaque;
    if (u == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!mm_mcxw71c_mrcc_clock_on(u->mrcc_offset) ||
        !mm_mcxw71c_mrcc_reset_released(u->mrcc_offset)) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > sizeof(u->regs)) return MM_FALSE;
    if (offset == LPUART_DATA && size_bytes == 4) {
        mm_u32 v = 0;
        if (mm_uart_io_has_rx(&u->io)) {
            v = (mm_u32)mm_uart_io_read(&u->io);
        }
        u->regs[LPUART_STAT / 4] &= ~STAT_RDRF;
        *value_out = v;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)u->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool uart_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct lpuart_inst *u = (struct lpuart_inst *)opaque;
    mm_u32 ctrl;
    if (u == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!mm_mcxw71c_mrcc_clock_on(u->mrcc_offset) ||
        !mm_mcxw71c_mrcc_reset_released(u->mrcc_offset)) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > sizeof(u->regs)) return MM_FALSE;

    if (offset == LPUART_DATA && size_bytes == 4) {
        ctrl = u->regs[LPUART_CTRL / 4];
        if ((ctrl & CTRL_TE) != 0u) {
            mm_uart_io_queue_tx(&u->io, (mm_u8)(value & 0xffu));
            (void)mm_uart_io_flush(&u->io);
            u->regs[LPUART_STAT / 4] |= STAT_TDRE | STAT_TC;
        }
        return MM_TRUE;
    }

    memcpy((mm_u8 *)u->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

void mm_mcxw71c_usart_poll(void)
{
    size_t i;
    for (i = 0; i < uart_count; ++i) {
        struct lpuart_inst *u = &uarts[i];
        if (mm_uart_io_poll(&u->io)) {
            update_status(u);
        }
    }
}

void mm_mcxw71c_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40038000u, 0x40039000u
    };
    static const mm_u32 mrcc_offsets[] = {
        MCXW71C_MRCC_LPUART0, MCXW71C_MRCC_LPUART1
    };
    size_t i;
    (void)nvic;
    uart_count = sizeof(bases) / sizeof(bases[0]);
    for (i = 0; i < uart_count; ++i) {
        struct lpuart_inst *u = &uarts[i];
        struct mmio_region reg;
        memset(u, 0, sizeof(*u));
        u->base = bases[i];
        u->mrcc_offset = mrcc_offsets[i];
        mm_uart_io_init(&u->io);
        sprintf(u->label, "LPUART%u", (unsigned)i);
        u->regs[LPUART_STAT / 4] = STAT_TDRE | STAT_TC | STAT_IDLE;

        reg.base = bases[i];
        reg.size = 0x1000u;
        reg.opaque = u;
        reg.read = uart_read;
        reg.write = uart_write;
        mmio_bus_register_region(bus, &reg);
        reg.base = bases[i] + 0x10000000u;
        mmio_bus_register_region(bus, &reg);

        if (mm_uart_io_open(&u->io, u->base)) {
            if (mm_tui_is_active()) {
                mm_tui_attach_uart(u->label, u->io.name);
            }
        }
    }
}

void mm_mcxw71c_usart_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(uarts) / sizeof(uarts[0]); ++i) {
        mm_uart_io_close(&uarts[i].io);
        memset(&uarts[i], 0, sizeof(uarts[i]));
    }
    uart_count = 0;
}
