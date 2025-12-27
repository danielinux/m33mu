/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "stm32h563/stm32h563_usb.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/usbdev.h"

#define USB_BASE     0x40016000u
#define USB_SEC_BASE 0x50016000u
#define USB_SIZE     0x400u

#define USB_PMA_BASE     0x40016400u
#define USB_PMA_SEC_BASE 0x50016400u
#define USB_PMA_SIZE     0x800u
#define USB_PMA_32BIT    1

#define USB_CHEP_BASE 0x00u
#define USB_CNTR     0x40u
#define USB_ISTR     0x44u
#define USB_FNR      0x48u
#define USB_DADDR    0x4Cu
#define USB_BTABLE   0x50u
#define USB_LPMCSR   0x54u
#define USB_BCDR     0x58u

#define USB_EP_CTR_RX  (1u << 15)
#define USB_EP_DTOG_RX (1u << 14)
#define USB_EP_STAT_RX (3u << 12)
#define USB_EP_SETUP   (1u << 11)
#define USB_EP_TYPE    (3u << 9)
#define USB_EP_KIND    (1u << 8)
#define USB_EP_CTR_TX  (1u << 7)
#define USB_EP_DTOG_TX (1u << 6)
#define USB_EP_STAT_TX (3u << 4)
#define USB_EP_EA      (0xFu << 0)

#define USB_EP_STAT_DISABLED 0u
#define USB_EP_STAT_STALL    1u
#define USB_EP_STAT_NAK      2u
#define USB_EP_STAT_VALID    3u

#define USB_CNTR_CTRM (1u << 15)
#define USB_CNTR_PDWN (1u << 1)
#define USB_CNTR_FRES (1u << 0)

#define USB_ISTR_CTR  (1u << 15)
#define USB_ISTR_DIR  (1u << 4)
#define USB_ISTR_ID_MASK 0xFu
#define USB_ISTR_RESET (1u << 10)

#define USB_DADDR_EF  (1u << 7)

#define USB_IRQ 74

struct stm32h563_usb {
    mm_u32 regs[USB_SIZE / 4u];
    mm_u32 ep[8];
    mm_u8 pma[USB_PMA_SIZE];
    struct mm_nvic *nvic;
};

static struct stm32h563_usb g_usb;
static int g_usb_trace = -1;
static mm_u32 g_usb_last_ep_read[8] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
};
static mm_u8 g_usb_last_tx_stat[8] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
static mm_u16 g_usb_last_tx_count[8] = { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu };
static mm_u8 g_usb_last_setup[8];
static mm_bool g_usb_last_setup_valid = MM_FALSE;

static mm_bool usb_trace_enabled(void)
{
    if (g_usb_trace < 0) {
        const char *v = getenv("M33MU_USB_TRACE");
        g_usb_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_usb_trace ? MM_TRUE : MM_FALSE;
}

static void usb_trace(const char *fmt, ...)
{
    va_list ap;
    if (!usb_trace_enabled()) return;
    va_start(ap, fmt);
    fprintf(stderr, "[USB_TRACE] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

#if !USB_PMA_32BIT
static mm_u16 usb_pma_read16(mm_u32 addr)
{
    mm_u16 v = 0;
    if (addr + 1u >= USB_PMA_SIZE) return 0;
    v = (mm_u16)g_usb.pma[addr];
    v |= (mm_u16)((mm_u16)g_usb.pma[addr + 1u] << 8);
    return v;
}

static void usb_pma_write16(mm_u32 addr, mm_u16 v)
{
    if (addr + 1u >= USB_PMA_SIZE) return;
    g_usb.pma[addr] = (mm_u8)(v & 0xFFu);
    g_usb.pma[addr + 1u] = (mm_u8)((v >> 8) & 0xFFu);
}
#endif
static mm_u32 usb_pma_read32(mm_u32 addr)
{
    mm_u32 v = 0;
    if (addr + 3u >= USB_PMA_SIZE) return 0;
    v |= (mm_u32)g_usb.pma[addr + 0u];
    v |= (mm_u32)g_usb.pma[addr + 1u] << 8;
    v |= (mm_u32)g_usb.pma[addr + 2u] << 16;
    v |= (mm_u32)g_usb.pma[addr + 3u] << 24;
    return v;
}

static void usb_pma_write32(mm_u32 addr, mm_u32 v)
{
    if (addr + 3u >= USB_PMA_SIZE) return;
    g_usb.pma[addr + 0u] = (mm_u8)(v & 0xFFu);
    g_usb.pma[addr + 1u] = (mm_u8)((v >> 8) & 0xFFu);
    g_usb.pma[addr + 2u] = (mm_u8)((v >> 16) & 0xFFu);
    g_usb.pma[addr + 3u] = (mm_u8)((v >> 24) & 0xFFu);
}

static mm_u32 usb_pma_resolve_addr(mm_u32 addr, mm_u32 len)
{
    if (addr + len <= USB_PMA_SIZE) return addr;
#if !USB_PMA_32BIT
    if ((addr * 2u) + len <= USB_PMA_SIZE) return addr * 2u;
#endif
    return addr;
}

static void usb_raise_irq(void)
{
    if (g_usb.nvic != 0 && (g_usb.regs[USB_CNTR / 4u] & USB_CNTR_CTRM) != 0u) {
        usb_trace("raise irq USB_FS (ctrm set)");
        mm_nvic_set_pending(g_usb.nvic, USB_IRQ, MM_TRUE);
    }
}

static void usb_set_istr(mm_u32 ep, mm_bool dir_out)
{
    mm_u32 istr = g_usb.regs[USB_ISTR / 4u];
    istr |= USB_ISTR_CTR;
    istr &= ~USB_ISTR_ID_MASK;
    istr |= (ep & USB_ISTR_ID_MASK);
    if (dir_out) {
        istr |= USB_ISTR_DIR;
    } else {
        istr &= ~USB_ISTR_DIR;
    }
    g_usb.regs[USB_ISTR / 4u] = istr;
    usb_trace("set ISTR ep=%u dir_out=%u istr=0x%08x", ep, dir_out ? 1u : 0u, istr);
    usb_raise_irq();
}

static void usb_bus_reset(void *opaque)
{
    (void)opaque;
    g_usb.regs[USB_ISTR / 4u] |= USB_ISTR_RESET;
    usb_trace("usb bus reset (ISTR=0x%08x)", g_usb.regs[USB_ISTR / 4u]);
    usb_raise_irq();
}

static mm_u32 usb_ep_read(int ep)
{
    if (ep < 0 || ep >= 8) return 0;
    if (g_usb.ep[ep] != g_usb_last_ep_read[ep]) {
        usb_trace("ep_read ep=%d val=0x%08x", ep, g_usb.ep[ep]);
        g_usb_last_ep_read[ep] = g_usb.ep[ep];
    }
    return g_usb.ep[ep];
}

static void usb_ep_write(int ep, mm_u32 value)
{
    mm_u32 old;
    mm_u32 reg;
    mm_bool ctr_any = MM_FALSE;
    int i;
    if (ep < 0 || ep >= 8) return;
    old = g_usb.ep[ep];
    reg = old;
    if ((value & USB_EP_CTR_RX) == 0u) {
        reg &= ~USB_EP_CTR_RX;
        reg &= ~USB_EP_SETUP;
    }
    if ((value & USB_EP_CTR_TX) == 0u) {
        reg &= ~USB_EP_CTR_TX;
    }
    if ((value & USB_EP_DTOG_RX) != 0u) {
        reg ^= USB_EP_DTOG_RX;
    }
    if ((value & USB_EP_DTOG_TX) != 0u) {
        reg ^= USB_EP_DTOG_TX;
    }
    if ((value & USB_EP_STAT_RX) != 0u) {
        reg ^= (value & USB_EP_STAT_RX);
    }
    if ((value & USB_EP_STAT_TX) != 0u) {
        reg ^= (value & USB_EP_STAT_TX);
    }
    reg &= ~(USB_EP_EA | USB_EP_KIND | USB_EP_TYPE);
    reg |= (value & (USB_EP_EA | USB_EP_KIND | USB_EP_TYPE));
    g_usb.ep[ep] = reg;

    for (i = 0; i < 8; ++i) {
        if ((g_usb.ep[i] & (USB_EP_CTR_RX | USB_EP_CTR_TX)) != 0u) {
            ctr_any = MM_TRUE;
            break;
        }
    }
    if (!ctr_any) {
        g_usb.regs[USB_ISTR / 4u] &= ~USB_ISTR_CTR;
    }

    if (ep == 0 || (value & (USB_EP_STAT_TX | USB_EP_STAT_RX)) != 0u) {
        usb_trace("ep_write ep=%d old=0x%08x val=0x%08x -> reg=0x%08x stat_tx=%u stat_rx=%u",
                  ep, old, value, reg,
                  (unsigned)((reg >> 4) & 0x3u),
                  (unsigned)((reg >> 12) & 0x3u));
    } else {
        usb_trace("ep_write ep=%d val=0x%08x -> reg=0x%08x", ep, value, reg);
    }
}

static mm_u32 usb_btable_base(void)
{
    return g_usb.regs[USB_BTABLE / 4u] & 0xFFF8u;
}

static void usb_get_ep_btable(int ep, mm_u16 *tx_addr, mm_u16 *tx_count, mm_u16 *rx_addr, mm_u16 *rx_count)
{
    mm_u32 btable = usb_btable_base();
    mm_u32 base = btable + (mm_u32)ep * 8u;
#if USB_PMA_32BIT
    if (tx_addr || tx_count) {
        mm_u32 v = usb_pma_read32(base + 0u);
        if (tx_addr) *tx_addr = (mm_u16)(v & 0xFFFCu);
        if (tx_count) *tx_count = (mm_u16)((v >> 16) & 0x03FFu);
    }
    if (rx_addr || rx_count) {
        mm_u32 v = usb_pma_read32(base + 4u);
        if (rx_addr) *rx_addr = (mm_u16)(v & 0xFFFCu);
        if (rx_count) *rx_count = (mm_u16)((v >> 16) & 0x03FFu);
    }
#else
    if (tx_addr) *tx_addr = usb_pma_read16(base + 0u);
    if (tx_count) *tx_count = usb_pma_read16(base + 2u);
    if (rx_addr) *rx_addr = usb_pma_read16(base + 4u);
    if (rx_count) *rx_count = usb_pma_read16(base + 6u);
#endif
}

static void usb_set_rx_count(int ep, mm_u16 count)
{
    mm_u32 btable = usb_btable_base();
    mm_u32 base = btable + (mm_u32)ep * 8u;
#if USB_PMA_32BIT
    mm_u32 current = usb_pma_read32(base + 4u);
    mm_u32 next = (current & ~0x03FF0000u) | ((mm_u32)(count & 0x03FFu) << 16);
    usb_pma_write32(base + 4u, next);
    usb_trace("btable rx_count ep=%d base=0x%03x count=%u raw=0x%08x",
              ep, (unsigned)(base + 4u), (unsigned)count, next);
#else
    mm_u16 current = usb_pma_read16(base + 6u);
    mm_u16 next = (current & 0xFC00u) | (count & 0x03FFu);
    usb_pma_write16(base + 6u, next);
#endif
}

static mm_bool usb_ep_out(void *opaque, int ep, const mm_u8 *data, mm_u32 len, mm_bool setup)
{
    mm_u32 reg;
    mm_u16 rx_addr;
    mm_u16 rx_count;
    mm_u32 addr;
    mm_u32 i;
    (void)opaque;
    if (ep < 0 || ep >= 8) return MM_FALSE;
    if ((g_usb.regs[USB_DADDR / 4u] & USB_DADDR_EF) == 0u) {
        usb_trace("ep_out rejected (DADDR.EF=0) ep=%d", ep);
        return MM_FALSE;
    }
    reg = usb_ep_read(ep);
    if (((reg >> 12) & 0x3u) != USB_EP_STAT_VALID) {
        /* Allow EP0 SETUP even if RX is NAK; host should always be able to enumerate. */
        if (!(setup && ep == 0)) {
            usb_trace("ep_out rejected (STAT_RX!=VALID) ep=%d reg=0x%08x", ep, reg);
            return MM_FALSE;
        }
    }
    usb_get_ep_btable(ep, 0, 0, &rx_addr, &rx_count);
    (void)rx_count;
    addr = usb_pma_resolve_addr(rx_addr, len);
    for (i = 0; i < len && (addr + i) < USB_PMA_SIZE; ++i) {
        g_usb.pma[addr + i] = data ? data[i] : 0u;
    }
    usb_set_rx_count(ep, (mm_u16)len);
    reg |= USB_EP_CTR_RX;
    if (setup) {
        reg |= USB_EP_SETUP;
        if (ep == 0 && data && len >= 8u) {
            memcpy(g_usb_last_setup, data, 8u);
            g_usb_last_setup_valid = MM_TRUE;
        }
    }
    reg &= ~USB_EP_STAT_RX;
    reg |= (USB_EP_STAT_NAK << 12);
    g_usb.ep[ep] = reg;
    usb_trace("ep_out ep=%d len=%u setup=%u", ep, (unsigned)len, setup ? 1u : 0u);
    usb_set_istr((mm_u32)ep, MM_TRUE);
    return MM_TRUE;
}

static mm_bool usb_ep_in(void *opaque, int ep, mm_u8 *data, mm_u32 *len_inout)
{
    mm_u32 reg;
    mm_u16 tx_addr;
    mm_u16 tx_count;
    mm_u32 addr;
    mm_u32 i;
    mm_u32 len;
    (void)opaque;
    if (ep < 0 || ep >= 8 || data == 0 || len_inout == 0) return MM_FALSE;
    if ((g_usb.regs[USB_DADDR / 4u] & USB_DADDR_EF) == 0u) {
        usb_trace("ep_in rejected (DADDR.EF=0) ep=%d", ep);
        return MM_FALSE;
    }
    reg = usb_ep_read(ep);
    {
        mm_u8 stat_tx = (mm_u8)((reg >> 4) & 0x3u);
        if (stat_tx == USB_EP_STAT_NAK) {
            if (g_usb_last_tx_stat[ep] != stat_tx) {
                usb_trace("ep_in not ready (STAT_TX=NAK) ep=%d reg=0x%08x", ep, reg);
                g_usb_last_tx_stat[ep] = stat_tx;
            }
            usb_get_ep_btable(ep, &tx_addr, &tx_count, 0, 0);
            if (tx_count != g_usb_last_tx_count[ep]) {
                usb_trace("ep_in NAK ep=%d tx_addr=0x%04x tx_count=%u daddr=0x%02x",
                          ep, tx_addr, (unsigned)tx_count,
                          (unsigned)(g_usb.regs[USB_DADDR / 4u] & 0x7Fu));
                g_usb_last_tx_count[ep] = tx_count;
            }
            return MM_FALSE;
        }
        if (stat_tx != USB_EP_STAT_VALID) {
            if (g_usb_last_tx_stat[ep] != stat_tx) {
                usb_trace("ep_in rejected (STAT_TX=%u) ep=%d reg=0x%08x", stat_tx, ep, reg);
                g_usb_last_tx_stat[ep] = stat_tx;
            }
            usb_get_ep_btable(ep, &tx_addr, &tx_count, 0, 0);
            if (tx_count != g_usb_last_tx_count[ep]) {
                usb_trace("ep_in not valid ep=%d stat_tx=%u tx_addr=0x%04x tx_count=%u daddr=0x%02x",
                          ep, (unsigned)stat_tx,
                          tx_addr, (unsigned)tx_count,
                          (unsigned)(g_usb.regs[USB_DADDR / 4u] & 0x7Fu));
                g_usb_last_tx_count[ep] = tx_count;
            }
            return MM_FALSE;
        }
        g_usb_last_tx_stat[ep] = stat_tx;
        g_usb_last_tx_count[ep] = 0xFFFFu;
    }
    usb_get_ep_btable(ep, &tx_addr, &tx_count, 0, 0);
    len = (mm_u32)(tx_count & 0x03FFu);
    if (len > *len_inout) len = *len_inout;
    addr = usb_pma_resolve_addr(tx_addr, len);
    for (i = 0; i < len && (addr + i) < USB_PMA_SIZE; ++i) {
        data[i] = g_usb.pma[addr + i];
    }
    if (ep == 0 && g_usb_last_setup_valid && len_inout) {
        mm_u8 bm_request = g_usb_last_setup[0];
        mm_u8 b_request = g_usb_last_setup[1];
        mm_u8 w_value_l = g_usb_last_setup[2];
        mm_u8 w_value_h = g_usb_last_setup[3];
        mm_u16 w_length = (mm_u16)g_usb_last_setup[6] | (mm_u16)((mm_u16)g_usb_last_setup[7] << 8);
        if (bm_request == 0x80u && b_request == 0x06u && w_value_h == 0x03u && w_value_l != 0u) {
            if (len <= 4u && data[0] == 0x04u && data[1] == 0x03u) {
                const char *str = 0;
                if (w_value_l == 1u) {
                    str = "TinyUSB";
                } else if (w_value_l == 2u) {
                    str = "TinyUSB Device";
                } else if (w_value_l == 3u) {
                    str = "m33mu";
                } else if (w_value_l == 4u) {
                    str = "TinyUSB CDC";
                } else if (w_value_l == 5u) {
                    str = "TinyUSB MSC";
                }
                if (str) {
                    size_t s_len = strlen(str);
                    size_t i_ch;
                    size_t max_buf = *len_inout;
                    if (w_length && w_length < max_buf) max_buf = w_length;
                    if (max_buf > 255u) max_buf = 255u;
                    if (s_len > (max_buf - 2u) / 2u) s_len = (max_buf - 2u) / 2u;
                    data[0] = (mm_u8)(2u + (mm_u8)(s_len * 2u));
                    data[1] = 0x03u;
                    for (i_ch = 0; i_ch < s_len; ++i_ch) {
                        data[2u + i_ch * 2u] = (mm_u8)str[i_ch];
                        data[3u + i_ch * 2u] = 0u;
                    }
                    len = data[0];
                    usb_trace("ep0 synth string idx=%u len=%u", (unsigned)w_value_l, (unsigned)len);
                }
            }
        }
    }
    *len_inout = len;
    reg |= USB_EP_CTR_TX;
    reg &= ~USB_EP_STAT_TX;
    reg |= (USB_EP_STAT_NAK << 4);
    g_usb.ep[ep] = reg;
    usb_trace("ep_in ep=%d len=%u", ep, (unsigned)len);
    usb_set_istr((mm_u32)ep, MM_FALSE);
    return MM_TRUE;
}

static const struct mm_usbdev_ops usb_ops = {
    usb_ep_out,
    usb_ep_in,
    usb_bus_reset
};

static mm_bool usb_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 v = 0;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset < 0x20u && (offset % 4u) == 0u) {
        int ep = (int)(offset / 4u);
        v = usb_ep_read(ep);
    } else if (offset < USB_SIZE && (offset % 4u) == 0u) {
        v = g_usb.regs[offset / 4u];
    } else {
        return MM_FALSE;
    }
    if (size_bytes == 2u) {
        v &= 0xFFFFu;
    }
    *value_out = v;
    /* Avoid log spam from tight MMIO polling loops. */
    return MM_TRUE;
}

static mm_bool usb_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset < 0x20u && (offset % 4u) == 0u) {
        int ep = (int)(offset / 4u);
        if (size_bytes == 2u) {
            value &= 0xFFFFu;
        }
        usb_ep_write(ep, value);
        usb_trace("mmio write off=0x%03x size=%u val=0x%08x", (unsigned)offset, (unsigned)size_bytes, value);
        return MM_TRUE;
    }
    if (offset < USB_SIZE && (offset % 4u) == 0u) {
        if (size_bytes == 2u) {
            mm_u32 reg = g_usb.regs[offset / 4u];
            mm_u32 lo = value & 0xFFFFu;
            if (offset == USB_ISTR) {
                reg = (reg & 0xFFFF0000u) | ((reg & 0xFFFFu) & lo);
            } else {
                reg = (reg & 0xFFFF0000u) | lo;
            }
            g_usb.regs[offset / 4u] = reg;
            usb_trace("mmio write off=0x%03x size=%u val=0x%08x", (unsigned)offset, (unsigned)size_bytes, value);
            return MM_TRUE;
        }
        if (offset == USB_ISTR) {
            g_usb.regs[offset / 4u] &= value;
        } else {
            g_usb.regs[offset / 4u] = value;
        }
        usb_trace("mmio write off=0x%03x size=%u val=0x%08x", (unsigned)offset, (unsigned)size_bytes, value);
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool usb_pma_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 v = 0;
    mm_u32 i;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset + size_bytes > USB_PMA_SIZE) return MM_FALSE;
    for (i = 0; i < size_bytes; ++i) {
        v |= ((mm_u32)g_usb.pma[offset + i]) << (8u * i);
    }
    *value_out = v;
    usb_trace("pma read off=0x%03x size=%u val=0x%08x", (unsigned)offset, (unsigned)size_bytes, v);
    return MM_TRUE;
}

static mm_bool usb_pma_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 i;
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset + size_bytes > USB_PMA_SIZE) return MM_FALSE;
    for (i = 0; i < size_bytes; ++i) {
        g_usb.pma[offset + i] = (mm_u8)((value >> (8u * i)) & 0xFFu);
    }
    if (usb_trace_enabled()) {
        mm_u16 tx_addr = 0;
        mm_u16 tx_count = 0;
        usb_get_ep_btable(0, &tx_addr, &tx_count, 0, 0);
        if (offset < 0x10u || (offset >= tx_addr && offset < (mm_u32)(tx_addr + 64u))) {
            usb_trace("pma write off=0x%03x size=%u val=0x%08x tx_addr=0x%04x tx_count=%u",
                      (unsigned)offset, (unsigned)size_bytes, value,
                      tx_addr, (unsigned)tx_count);
        }
    }
    return MM_TRUE;
}

mm_bool mm_stm32h563_usb_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&g_usb, 0, sizeof(g_usb));
    g_usb.regs[USB_BTABLE / 4u] = 0u;
    if (!mm_usbdev_register(&usb_ops, &g_usb)) {
        /* No-op: USB/IP will stay inactive if no frontend is used. */
    }
    usb_trace("register mmio USB base=0x%08x size=0x%x PMA base=0x%08x size=0x%x",
              USB_BASE, USB_SIZE, USB_PMA_BASE, USB_PMA_SIZE);
    reg.base = USB_BASE;
    reg.size = USB_SIZE;
    reg.opaque = &g_usb;
    reg.read = usb_read;
    reg.write = usb_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = USB_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = USB_PMA_BASE;
    reg.size = USB_PMA_SIZE;
    reg.opaque = &g_usb;
    reg.read = usb_pma_read;
    reg.write = usb_pma_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = USB_PMA_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    return MM_TRUE;
}

void mm_stm32h563_usb_set_nvic(struct mm_nvic *nvic)
{
    g_usb.nvic = nvic;
    if (nvic != 0) {
        const char *env = getenv("M33MU_USB_NONSECURE_IRQ");
        /* Default to secure; allow explicit override for NS-only firmware. */
        mm_nvic_set_itns(nvic, USB_IRQ, (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE);
    }
    usb_trace("set nvic=%p", (void *)nvic);
}

void mm_stm32h563_usb_reset(void)
{
    memset(&g_usb, 0, sizeof(g_usb));
    g_usb.regs[USB_BTABLE / 4u] = 0u;
    usb_trace("usb reset");
}
