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
#include "stm32u585/stm32u585_usart.h"
#include "stm32u585/stm32u585_mmio.h"
#include "m33mu/target_hal.h"

/* Minimal register offsets */
#define USART_CR1   0x00u
#define USART_CR2   0x04u
#define USART_CR3   0x08u
#define USART_BRR   0x0Cu
#define USART_ISR   0x1Cu
#define USART_ICR   0x20u
#define USART_RDR   0x24u
#define USART_TDR   0x28u

/* Bits we care about */
#define CR1_UE  (1u << 0)
#define CR1_RE  (1u << 2)
#define CR1_TE  (1u << 3)
#define CR1_RXNEIE (1u << 5)
#define CR1_TXEIE (1u << 7)

#define ISR_RXNE (1u << 5)
#define ISR_TXE  (1u << 7)

struct usart_inst {
    mm_u32 base;
    mm_u32 regs[0x30 / 4];
    struct mm_uart_io io;
    char label[16];
    mm_bool enabled;
    int irq;
    mm_u32 *rcc_regs;
    mm_bool (*clock_on)(struct usart_inst *u);
    volatile mm_u32 *sec_reg; /* GTZC SECCFGR word controlling secure attribution (or NULL) */
    mm_u32 sec_bitmask;       /* Bit within sec_reg */
    mm_bool secure_only;      /* Derived each access from GTZC */
    enum mm_sec_state current_sec;
    mm_u8 macro_match;
    mm_bool watch_macro;
};

static struct usart_inst usarts[6];
static size_t usart_count = 0;
static struct mm_nvic *g_nvic = 0;

static mm_bool clock_apb2_usart1(struct usart_inst *u)
{
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0xa4 / 4] >> 14) & 1u) != 0u;
}

static mm_bool clock_apb1enr1_generic(struct usart_inst *u)
{
    mm_u32 idx = (mm_u32)(u - usarts);
    mm_u32 bit = 0;
    switch (idx) {
    case 1: bit = 17; break; /* USART2 */
    case 2: bit = 18; break; /* USART3 */
    case 3: bit = 19; break; /* UART4 */
    case 4: bit = 20; break; /* UART5 */
    default: return MM_TRUE;
    }
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0x9c / 4] >> bit) & 1u) != 0u;
}

static mm_bool clock_apb3_lpuart1(struct usart_inst *u)
{
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0xa8 / 4] >> 6) & 1u) != 0u;
}

static void ensure_enabled(struct usart_inst *u)
{
    mm_bool was;
    mm_u32 cr1 = u->regs[USART_CR1 / 4];
    mm_bool ue = (cr1 & CR1_UE) != 0u;
    if (u->clock_on != 0) {
        if (!u->clock_on(u)) {
            ue = MM_FALSE;
        }
    }
    was = u->enabled;
    u->enabled = ue;
    if (ue && !was) {
        if (mm_uart_io_open(&u->io, u->base)) {
            /* Mark TXE empty */
            u->regs[USART_ISR / 4] |= ISR_TXE;
            if (mm_tui_is_active()) {
                mm_tui_attach_uart(u->label, u->io.name);
            }
        }
    } else if (!ue && was) {
        mm_uart_io_close(&u->io);
    }
}

static mm_bool usart_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct usart_inst *u = (struct usart_inst *)opaque;
    u->current_sec = mmio_active_sec();
    u->secure_only = (u->sec_reg != 0 && ((*(u->sec_reg)) & u->sec_bitmask) != 0u);
    if (u->secure_only && u->current_sec == MM_NONSECURE) {
        if (value_out) *value_out = 0;
        return MM_TRUE;
    }
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset >= sizeof(u->regs)) return MM_FALSE;
    ensure_enabled(u);
    if (offset == USART_RDR) {
        mm_u32 v = mm_uart_io_has_rx(&u->io) ? mm_uart_io_read(&u->io) : 0u;
        *value_out = v;
        u->regs[USART_ISR / 4] &= ~ISR_RXNE;
        return MM_TRUE;
    }
    if (offset == USART_ISR) {
        /* Keep TXE set so firmware polls see the line idle immediately. */
        u->regs[USART_ISR / 4] |= ISR_TXE;
    }
    memcpy(value_out, (mm_u8 *)u->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool usart_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct usart_inst *u = (struct usart_inst *)opaque;
    static const char macro_pat[] = "macro   error";
    u->current_sec = mmio_active_sec();
    u->secure_only = (u->sec_reg != 0 && ((*(u->sec_reg)) & u->sec_bitmask) != 0u);
    if (u->secure_only && u->current_sec == MM_NONSECURE) {
        return MM_TRUE;
    }
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
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
        u->regs[USART_ISR / 4] &= ~ISR_TXE;
        if (mm_uart_io_flush(&u->io) && mm_uart_io_tx_empty(&u->io)) {
            u->regs[USART_ISR / 4] |= ISR_TXE;
        }
        return MM_TRUE;
    }
    memcpy((mm_u8 *)u->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static void poll_instance(struct usart_inst *u)
{
    ensure_enabled(u);
    if (!u->enabled) return;

    if (mm_uart_io_poll(&u->io)) {
        u->regs[USART_ISR / 4] |= ISR_RXNE;
    }
    if (mm_uart_io_tx_empty(&u->io)) {
        u->regs[USART_ISR / 4] |= ISR_TXE;
    }
    /* Interrupts */
    if (g_nvic != 0 && u->irq >= 0) {
        mm_u32 cr1 = u->regs[USART_CR1 / 4];
        mm_u32 isr = u->regs[USART_ISR / 4];
        if (((cr1 & CR1_RXNEIE) != 0u) && ((isr & ISR_RXNE) != 0u)) {
            mm_nvic_set_pending(g_nvic, (mm_u32)u->irq, MM_TRUE);
        }
        if (((cr1 & CR1_TXEIE) != 0u) && ((isr & ISR_TXE) != 0u)) {
            mm_nvic_set_pending(g_nvic, (mm_u32)u->irq, MM_TRUE);
        }
    }
}

void mm_stm32u585_usart_poll(void)
{
    size_t i;
    for (i = 0; i < usart_count; ++i) {
        poll_instance(&usarts[i]);
    }
}

void mm_stm32u585_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40013800u, 0x40004400u, 0x40004800u, 0x40004C00u,
        0x40005000u, 0x46002400u
    };
    static const int irq_map[] = {
        61, 62, 63, 64, 65, 66
    };
    static const char *labels[] = {
        "USART1", "USART2", "USART3", "UART4", "UART5", "LPUART1"
    };
    size_t i;
    mm_u32 *tz1 = mm_stm32u585_tzsc_regs();
    mm_u32 *tz1_cfgr1 = (tz1 != 0) ? tz1 + (0x10u / 4u) : 0;
    mm_u32 *tz1_cfgr2 = (tz1 != 0) ? tz1 + (0x14u / 4u) : 0;
    mm_u32 *tz2 = mm_stm32u585_tzsc2_regs();
    mm_u32 *tz2_cfgr1 = (tz2 != 0) ? tz2 + (0x10u / 4u) : 0;
    g_nvic = nvic;
    mm_stm32u585_rng_set_nvic(nvic);
    if (usart_count == 0) {
        usart_count = sizeof(bases) / sizeof(bases[0]);
    }
    for (i = 0; i < usart_count; ++i) {
        struct usart_inst *u = &usarts[i];
        struct mmio_region reg;
        memset(u, 0, sizeof(*u));
        u->base = bases[i];
        u->regs[USART_ISR / 4] = ISR_TXE; /* idle empty */
        mm_uart_io_init(&u->io);
        if (i < (sizeof(labels) / sizeof(labels[0]))) {
            strncpy(u->label, labels[i], sizeof(u->label) - 1u);
            u->label[sizeof(u->label) - 1u] = '\0';
        } else {
            sprintf(u->label, "USART%u", (unsigned)(i + 1u));
        }
        u->irq = (i < (sizeof(irq_map)/sizeof(irq_map[0]))) ? irq_map[i] : -1;
        u->rcc_regs = mm_stm32u585_rcc_regs();
        u->clock_on = 0;
        u->current_sec = MM_SECURE;
        u->watch_macro = (i == 2u) ? MM_TRUE : MM_FALSE; /* USART3 */
        u->sec_reg = tz1_cfgr1;
        if (i == 0) {
            u->clock_on = clock_apb2_usart1;
            u->sec_bitmask = (1u << 3); /* TZSC_SECCFGR2 USART1SEC bit3 */
            u->sec_reg = tz1_cfgr2;
        } else if (i >= 1 && i <= 4) {
            u->clock_on = clock_apb1enr1_generic;
            u->sec_bitmask = (1u << (i + 8u)); /* USART2..UART5 bits 9..12 */
        } else {
            u->clock_on = clock_apb3_lpuart1;
            u->sec_reg = tz2_cfgr1;
            u->sec_bitmask = (1u << 1); /* TZSC2_SECCFGR1 LPUART1SEC bit1 */
        }
        reg.base = bases[i];
        reg.size = 0x400u;
        reg.opaque = u;
        reg.read = usart_read;
        reg.write = usart_write;
        mmio_bus_register_region(bus, &reg);
    }
}

void mm_stm32u585_usart_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(usarts)/sizeof(usarts[0]); ++i) {
        struct usart_inst *u = &usarts[i];
        mm_uart_io_close(&u->io);
        memset(u, 0, sizeof(*u));
    }
    usart_count = 0;
}
