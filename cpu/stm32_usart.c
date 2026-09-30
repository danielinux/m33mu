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
#include <stdlib.h>
#include "stm32_usart.h"
#include "m33mu/snapshot.h"

#define USART_CR1   0x00u
#define USART_ISR   0x1Cu
#define USART_ICR   0x20u
#define USART_RDR   0x24u
#define USART_TDR   0x28u

#define CR1_UE  (1u << 0)
#define CR1_RE  (1u << 2)
#define CR1_TE  (1u << 3)
#define CR1_RXNEIE (1u << 5)
#define CR1_TXEIE (1u << 7)

#define ISR_RXNE (1u << 5)
#define ISR_TC   (1u << 6)
#define ISR_TXE  (1u << 7)
#define ISR_TEACK (1u << 21)
#define ISR_REACK (1u << 22)

struct usart_snapshot {
    mm_u32 regs[0x30 / 4];
    mm_u8 tx_buf[1024];
    mm_u32 tx_head;
    mm_u32 tx_tail;
    mm_u32 rx_pending;
    mm_u32 rx_byte;
    mm_u32 stdout_only;
    mm_u32 enabled;
    mm_u32 macro_match;
    mm_u32 watch_macro;
    mm_u32 rx_trace;
    mm_u32 secure_only;
    mm_u32 current_sec;
};

mm_bool stm32_usart_uart_rx_trace_enabled(void)
{
    const char *v = getenv("M33MU_UART_RX_TRACE");
    return (v && v[0] != '\0') ? MM_TRUE : MM_FALSE;
}

static mmio_peek_result_t usart_peek(void *opaque, mm_u32 offset, mm_u32 size_bytes, void *dst)
{
    struct stm32_usart_inst *u = (struct stm32_usart_inst *)opaque;
    mm_u32 val = 0;
    mm_u32 cr1;
    mm_u32 isr;
    mm_u8 *out = (mm_u8 *)dst;
    enum mm_sec_state access_sec;
    mm_bool secure_only;

    if (u == 0 || dst == 0 || size_bytes == 0u || size_bytes > 4u) {
        return MMIO_PEEK_UNSUPPORTED;
    }
    access_sec = mmio_active_sec();
    secure_only = (u->sec_reg != 0 && ((*(u->sec_reg)) & u->sec_bitmask) != 0u);
    if (secure_only && access_sec == MM_NONSECURE) {
        memset(out, 0, size_bytes);
        return MMIO_PEEK_OK;
    }
    if (offset >= sizeof(u->regs)) {
        return MMIO_PEEK_UNSUPPORTED;
    }

    if (offset == USART_RDR) {
        val = u->io.rx_pending ? u->io.rx_byte : 0u;
    } else if (offset == USART_ISR) {
        cr1 = u->regs[USART_CR1 / 4];
        isr = u->regs[USART_ISR / 4];
        isr |= ISR_TXE;
        if (u->has_tc) {
            isr |= ISR_TC;
        }
        if ((cr1 & (CR1_UE | CR1_TE)) == (CR1_UE | CR1_TE)) {
            isr |= ISR_TEACK;
        } else {
            isr &= ~ISR_TEACK;
        }
        if ((cr1 & (CR1_UE | CR1_RE)) == (CR1_UE | CR1_RE)) {
            isr |= ISR_REACK;
        } else {
            isr &= ~ISR_REACK;
        }
        val = isr;
    } else {
        memcpy(&val, (mm_u8 *)u->regs + offset, size_bytes);
    }

    if (size_bytes == 1u) {
        out[0] = (mm_u8)(val & 0xffu);
    } else if (size_bytes == 2u) {
        out[0] = (mm_u8)(val & 0xffu);
        out[1] = (mm_u8)((val >> 8) & 0xffu);
    } else {
        out[0] = (mm_u8)(val & 0xffu);
        out[1] = (mm_u8)((val >> 8) & 0xffu);
        out[2] = (mm_u8)((val >> 16) & 0xffu);
        out[3] = (mm_u8)((val >> 24) & 0xffu);
    }
    return MMIO_PEEK_OK;
}

static mm_bool usart_save(void *opaque, struct mm_snapshot_writer *w)
{
    struct stm32_usart_inst *u = (struct stm32_usart_inst *)opaque;
    struct usart_snapshot snap;
    if (u == 0 || w == 0) {
        return MM_FALSE;
    }
    memset(&snap, 0, sizeof(snap));
    memcpy(snap.regs, u->regs, sizeof(snap.regs));
    memcpy(snap.tx_buf, u->io.tx_buf, sizeof(snap.tx_buf));
    snap.tx_head = (mm_u32)u->io.tx_head;
    snap.tx_tail = (mm_u32)u->io.tx_tail;
    snap.rx_pending = u->io.rx_pending ? 1u : 0u;
    snap.rx_byte = (mm_u32)u->io.rx_byte;
    snap.stdout_only = u->io.stdout_only ? 1u : 0u;
    snap.enabled = u->enabled ? 1u : 0u;
    snap.macro_match = u->macro_match;
    snap.watch_macro = u->watch_macro ? 1u : 0u;
    snap.rx_trace = u->rx_trace ? 1u : 0u;
    snap.secure_only = u->secure_only ? 1u : 0u;
    snap.current_sec = (mm_u32)u->current_sec;
    return mm_snapshot_write(w, &snap, (mm_u32)sizeof(snap));
}

static mm_bool usart_load(void *opaque, struct mm_snapshot_reader *r)
{
    struct stm32_usart_inst *u = (struct stm32_usart_inst *)opaque;
    struct usart_snapshot snap;
    if (u == 0 || r == 0) {
        return MM_FALSE;
    }
    if ((r->size - r->offset) < (mm_u32)sizeof(snap)) {
        return MM_FALSE;
    }
    if (!mm_snapshot_read(r, &snap, (mm_u32)sizeof(snap))) {
        return MM_FALSE;
    }
    memcpy(u->regs, snap.regs, sizeof(snap.regs));
    memcpy(u->io.tx_buf, snap.tx_buf, sizeof(snap.tx_buf));
    u->io.tx_head = (size_t)(snap.tx_head % (mm_u32)sizeof(u->io.tx_buf));
    u->io.tx_tail = (size_t)(snap.tx_tail % (mm_u32)sizeof(u->io.tx_buf));
    u->io.rx_pending = snap.rx_pending ? MM_TRUE : MM_FALSE;
    u->io.rx_byte = (mm_u8)(snap.rx_byte & 0xffu);
    u->io.stdout_only = snap.stdout_only ? MM_TRUE : MM_FALSE;
    u->enabled = snap.enabled ? MM_TRUE : MM_FALSE;
    u->macro_match = (mm_u8)(snap.macro_match & 0xffu);
    u->watch_macro = snap.watch_macro ? MM_TRUE : MM_FALSE;
    u->rx_trace = snap.rx_trace ? MM_TRUE : MM_FALSE;
    u->secure_only = snap.secure_only ? MM_TRUE : MM_FALSE;
    u->current_sec = (enum mm_sec_state)snap.current_sec;
    return MM_TRUE;
}

static void ensure_enabled(struct stm32_usart_inst *u)
{
    mm_bool was;
    mm_u32 cr1 = u->regs[USART_CR1 / 4];
    mm_bool ue = (cr1 & CR1_UE) != 0u;
    if (u->clock_on != 0 && !u->clock_on(u)) {
        ue = MM_FALSE;
    }
    was = u->enabled;
    u->enabled = ue;
    if (ue && !was) {
        if (mm_uart_io_open(&u->io, u->base)) {
            u->regs[USART_ISR / 4] |= ISR_TXE;
            if (u->has_tc) {
                u->regs[USART_ISR / 4] |= ISR_TC;
            }
            if (mm_tui_is_active()) {
                mm_tui_attach_uart(u->label, u->io.name);
            }
        }
    } else if (!ue && was) {
        mm_uart_io_close(&u->io);
    }
}

static void usart_update_ack(struct stm32_usart_inst *u)
{
    mm_u32 cr1 = u->regs[USART_CR1 / 4];
    mm_u32 isr = u->regs[USART_ISR / 4];
    mm_bool clock = (u->clock_on == 0 || u->clock_on(u));
    if (clock && (cr1 & (CR1_UE | CR1_TE)) == (CR1_UE | CR1_TE)) {
        isr |= ISR_TEACK;
    } else {
        isr &= ~ISR_TEACK;
    }
    if (clock && (cr1 & (CR1_UE | CR1_RE)) == (CR1_UE | CR1_RE)) {
        isr |= ISR_REACK;
    } else {
        isr &= ~ISR_REACK;
    }
    u->regs[USART_ISR / 4] = isr;
}

/* The interrupt request is the OR of each status flag with its enable bit,
 * so it is raised the moment a register write makes a pair true. */
static void usart_update_irq(struct stm32_usart_inst *u)
{
    mm_u32 cr1;
    mm_u32 isr;
    if (u->owner == 0 || u->owner->nvic == 0 || u->irq < 0 || !u->enabled) {
        return;
    }
    cr1 = u->regs[USART_CR1 / 4];
    isr = u->regs[USART_ISR / 4];
    if (((cr1 & CR1_RXNEIE) != 0u && (isr & ISR_RXNE) != 0u) ||
        ((cr1 & CR1_TXEIE) != 0u && (isr & ISR_TXE) != 0u)) {
        mm_nvic_set_pending(u->owner->nvic, (mm_u32)u->irq, MM_TRUE);
    }
}

static mm_bool usart_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct stm32_usart_inst *u = (struct stm32_usart_inst *)opaque;
    u->current_sec = mmio_active_sec();
    u->secure_only = (u->sec_reg != 0 && ((*(u->sec_reg)) & u->sec_bitmask) != 0u);
    if (u->secure_only && u->current_sec == MM_NONSECURE) {
        if (value_out) *value_out = 0;
        return MM_TRUE;
    }
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if (offset >= sizeof(u->regs)) return MM_FALSE;
    ensure_enabled(u);
    if (offset == USART_RDR) {
        mm_u32 v = 0u;
        if (mm_uart_io_has_rx(&u->io)) {
            v = mmio_peek_mode() ? mm_uart_io_peek(&u->io) : mm_uart_io_read(&u->io);
        }
        *value_out = v;
        if (!mmio_peek_mode()) {
            u->regs[USART_ISR / 4] &= ~ISR_RXNE;
        }
        if (u->rx_trace && !mmio_peek_mode()) {
            printf("[USART_RX_RDR] base=0x%08lx byte=0x%02lx\n",
                   (unsigned long)u->base,
                   (unsigned long)(v & 0xffu));
        }
        return MM_TRUE;
    }
    if (offset == USART_ISR) {
        u->regs[USART_ISR / 4] |= ISR_TXE;
        if (u->has_tc) {
            u->regs[USART_ISR / 4] |= ISR_TC;
        }
        usart_update_ack(u);
        if (u->rx_trace) {
            printf("[USART_ISR_READ] base=0x%08lx isr=0x%08lx\n",
                   (unsigned long)u->base,
                   (unsigned long)u->regs[USART_ISR / 4]);
        }
    }
    memcpy(value_out, (mm_u8 *)u->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool usart_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct stm32_usart_inst *u = (struct stm32_usart_inst *)opaque;
    static const char macro_pat[] = "macro   error";
    u->current_sec = mmio_active_sec();
    u->secure_only = (u->sec_reg != 0 && ((*(u->sec_reg)) & u->sec_bitmask) != 0u);
    if (u->secure_only && u->current_sec == MM_NONSECURE) {
        return MM_TRUE;
    }
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if (offset >= sizeof(u->regs)) return MM_FALSE;
    if (offset == USART_TDR) {
        ensure_enabled(u);
        if (!u->enabled) return MM_TRUE;
        if (u->watch_macro) {
            mm_u8 ch = (mm_u8)value;
            if ((size_t)u->macro_match < (sizeof(macro_pat) - 1u) &&
                ch == (mm_u8)macro_pat[u->macro_match]) {
                u->macro_match++;
                if ((size_t)u->macro_match == (sizeof(macro_pat) - 1u)) {
                    mm_uart_break_on_macro_set();
                    u->macro_match = 0;
                }
            } else if (ch == (mm_u8)macro_pat[0]) {
                u->macro_match = 1;
            } else {
                u->macro_match = 0;
            }
        }
        mm_uart_io_queue_tx(&u->io, (mm_u8)value);
        (void)mm_uart_io_flush(&u->io);
        /* The modeled shifter is instantaneous: the frame is out as soon as
         * it is written, whatever the host side of the console is doing. */
        u->regs[USART_ISR / 4] |= ISR_TXE;
        if (u->has_tc) {
            u->regs[USART_ISR / 4] |= ISR_TC;
        }
        usart_update_irq(u);
        return MM_TRUE;
    }
    if (u->has_tc && offset == USART_ICR) {
        if ((value & ISR_TC) != 0u) {
            u->regs[USART_ISR / 4] &= ~ISR_TC;
        }
        return MM_TRUE;
    }
    memcpy((mm_u8 *)u->regs + offset, &value, size_bytes);
    if (offset == USART_CR1) {
        usart_update_ack(u);
        ensure_enabled(u);
        usart_update_irq(u);
    }
    return MM_TRUE;
}

static void poll_instance(struct stm32_usart_inst *u)
{
    ensure_enabled(u);
    if (!u->enabled) return;
    if (mm_uart_io_poll(&u->io)) {
        u->regs[USART_ISR / 4] |= ISR_RXNE;
        if (u->rx_trace) {
            printf("[USART_RXNE_SET] base=0x%08lx\n", (unsigned long)u->base);
        }
    }
    usart_update_irq(u);
}

void stm32_usart_state_init(struct stm32_usart_state *state,
                            size_t count,
                            struct mm_nvic *nvic)
{
    if (state == 0) {
        return;
    }
    state->usart_count = count > STM32_USART_MAX_INSTANCES ? STM32_USART_MAX_INSTANCES : count;
    state->nvic = nvic;
}

void stm32_usart_register_instance(struct stm32_usart_state *state,
                                   struct mmio_bus *bus,
                                   size_t idx,
                                   mm_u32 base,
                                   int irq,
                                   const char *label,
                                   mm_bool snapshot,
                                   mm_bool has_tc,
                                   mm_bool rx_trace)
{
    struct stm32_usart_inst *u;
    struct mmio_region reg;
    if (state == 0 || idx >= STM32_USART_MAX_INSTANCES) {
        return;
    }
    u = &state->usarts[idx];
    memset(u, 0, sizeof(*u));
    memset(&reg, 0, sizeof(reg));
    u->owner = state;
    u->index = (mm_u32)idx;
    u->base = base;
    u->irq = irq;
    u->has_tc = has_tc;
    u->rx_trace = rx_trace;
    u->current_sec = MM_SECURE;
    u->regs[USART_ISR / 4] = ISR_TXE;
    mm_uart_io_init(&u->io);
    if (label != 0) {
        strncpy(u->label, label, sizeof(u->label) - 1u);
        u->label[sizeof(u->label) - 1u] = '\0';
    }
    reg.base = base;
    reg.size = 0x400u;
    reg.opaque = u;
    reg.read = usart_read;
    reg.write = usart_write;
    if (snapshot) {
        reg.magic = MMIO_REGION_MAGIC;
        reg.flags = MMIO_REGION_F_EXT;
        reg.peek = usart_peek;
        reg.name = u->label;
        reg.version = 1u;
        reg.save = usart_save;
        reg.load = usart_load;
    }
    mmio_bus_register_region(bus, &reg);
    reg.base = base + 0x10000000u;
    reg.name = 0;
    reg.save = 0;
    reg.load = 0;
    mmio_bus_register_region(bus, &reg);
}

void stm32_usart_poll(struct stm32_usart_state *state)
{
    size_t i;
    if (state == 0) {
        return;
    }
    for (i = 0; i < state->usart_count; ++i) {
        poll_instance(&state->usarts[i]);
    }
}

void stm32_usart_reset(struct stm32_usart_state *state)
{
    size_t i;
    if (state == 0) {
        return;
    }
    for (i = 0; i < STM32_USART_MAX_INSTANCES; ++i) {
        mm_uart_io_close(&state->usarts[i].io);
        memset(&state->usarts[i], 0, sizeof(state->usarts[i]));
    }
    state->usart_count = 0;
    state->nvic = 0;
}
