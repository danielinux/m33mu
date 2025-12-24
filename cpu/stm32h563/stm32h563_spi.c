/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "stm32h563/stm32h563_spi.h"
#include "stm32h563/stm32h563_mmio.h"
#include "m33mu/spi_bus.h"

#define SPI_CR1   0x00u
#define SPI_CR2   0x04u
#define SPI_CFG1  0x08u
#define SPI_CFG2  0x0Cu
#define SPI_IER   0x10u
#define SPI_SR    0x14u
#define SPI_IFCR  0x18u
#define SPI_TXDR  0x20u
#define SPI_RXDR  0x30u

#define CR1_SPE    (1u << 0)
#define CR1_CSTART (1u << 9)

#define IER_RXPIE  (1u << 0)
#define IER_TXPIE  (1u << 1)
#define IER_DXPIE  (1u << 2)
#define IER_EOTIE  (1u << 3)
#define IER_TXTFIE (1u << 4)

#define SR_RXP   (1u << 0)
#define SR_TXP   (1u << 1)
#define SR_DXP   (1u << 2)
#define SR_EOT   (1u << 3)
#define SR_TXTF  (1u << 4)
#define SR_BUSY  (1u << 10)
#define SR_TXC   (1u << 12)
#define SR_RXPLVL_SHIFT 13
#define SR_RXWNE (1u << 15)

struct spi_inst {
    mm_u32 base;
    mm_u32 regs[0x50 / 4];
    mm_u8 rx_fifo[32];
    mm_u8 rx_head;
    mm_u8 rx_tail;
    mm_bool enabled;
    mm_bool transfer_active;
    mm_bool eot_pending;
    mm_u32 tsize_rem;
    int irq;
    int bus_index;
};

static struct spi_inst spis[6];
static size_t spi_count = 0;
static struct mm_nvic *g_nvic = 0;

static mm_bool spi_trace_enabled(void)
{
    static mm_bool init = MM_FALSE;
    static mm_bool enabled = MM_FALSE;
    if (!init) {
        const char *env = getenv("M33MU_SPI_TRACE");
        enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
        init = MM_TRUE;
    }
    return enabled;
}

static mm_u32 spi_frame_bytes(const struct spi_inst *s)
{
    mm_u32 cfg1 = s->regs[SPI_CFG1 / 4];
    mm_u32 dsize = (cfg1 & 0x1Fu) + 1u;
    if ((cfg1 & 0x1Fu) == 0u) {
        dsize = 8u;
    }
    if (dsize <= 8u) return 1u;
    if (dsize <= 16u) return 2u;
    return 4u;
}

static mm_u8 fifo_count(const struct spi_inst *s)
{
    return (mm_u8)((s->rx_tail - s->rx_head) & 0x1Fu);
}

static mm_bool fifo_push(struct spi_inst *s, mm_u8 v)
{
    mm_u8 next = (mm_u8)((s->rx_tail + 1u) & 0x1Fu);
    if (next == s->rx_head) {
        return MM_FALSE;
    }
    s->rx_fifo[s->rx_tail] = v;
    s->rx_tail = next;
    return MM_TRUE;
}

static mm_bool fifo_pop(struct spi_inst *s, mm_u8 *out)
{
    if (s->rx_head == s->rx_tail) {
        return MM_FALSE;
    }
    if (out != 0) {
        *out = s->rx_fifo[s->rx_head];
    }
    s->rx_head = (mm_u8)((s->rx_head + 1u) & 0x1Fu);
    return MM_TRUE;
}

static void update_sr(struct spi_inst *s)
{
    mm_u32 sr = 0;
    mm_u8 count = fifo_count(s);
    mm_u32 tsize = s->transfer_active ? s->tsize_rem : 0u;
    mm_bool busy = (s->enabled && (s->transfer_active || count > 0u)) ? MM_TRUE : MM_FALSE;
    if (count > 0u) sr |= SR_RXP;
    sr |= SR_TXP;
    if ((sr & (SR_RXP | SR_TXP)) == (SR_RXP | SR_TXP)) sr |= SR_DXP;
    if (busy) sr |= SR_BUSY;
    if (s->eot_pending) {
        sr |= SR_EOT | SR_TXTF | SR_TXC;
    }
    if (count >= 4u) sr |= SR_RXWNE;
    sr |= ((mm_u32)((count > 3u) ? 3u : count) << SR_RXPLVL_SHIFT);
    sr |= ((tsize & 0xFFFFu) << 16);
    s->regs[SPI_SR / 4] = sr;
}

static void spi_set_enabled(struct spi_inst *s, mm_bool enable)
{
    if (enable) {
        s->enabled = MM_TRUE;
    } else {
        s->enabled = MM_FALSE;
        s->transfer_active = MM_FALSE;
        s->eot_pending = MM_FALSE;
        s->tsize_rem = 0;
        s->rx_head = 0;
        s->rx_tail = 0;
        mm_spi_bus_end(s->bus_index);
    }
    update_sr(s);
}

static void spi_start_transfer(struct spi_inst *s)
{
    mm_u32 tsize = s->regs[SPI_CR2 / 4] & 0xFFFFu;
    s->transfer_active = MM_TRUE;
    s->eot_pending = MM_FALSE;
    s->tsize_rem = (tsize == 0u) ? 0xFFFFFFFFu : tsize;
    s->regs[SPI_SR / 4] &= ~(SR_EOT | SR_TXTF | SR_TXC);
}

static mm_u8 spi_xfer_byte(struct spi_inst *s, mm_u8 out)
{
    return mm_spi_bus_xfer(s->bus_index, out);
}

static void spi_handle_tx(struct spi_inst *s, mm_u32 value, mm_u32 size_bytes)
{
    mm_u32 i;
    mm_u32 send_bytes = spi_frame_bytes(s);
    if (!s->enabled) return;
    if (send_bytes > size_bytes) {
        send_bytes = size_bytes;
    }
    for (i = 0; i < send_bytes; ++i) {
        mm_u8 out = (mm_u8)((value >> (i * 8u)) & 0xFFu);
        mm_u8 in = spi_xfer_byte(s, out);
        (void)fifo_push(s, in);
        if (spi_trace_enabled()) {
            printf("[SPI] SPI%d TX=0x%02x RX=0x%02x\n", s->bus_index, out, in);
        }
        if (s->transfer_active && s->tsize_rem != 0u) {
            s->tsize_rem--;
        }
    }
    if (s->transfer_active && s->tsize_rem == 0u) {
        s->transfer_active = MM_FALSE;
        s->eot_pending = MM_TRUE;
        mm_spi_bus_end(s->bus_index);
    }
    update_sr(s);
}

static mm_bool spi_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct spi_inst *s = (struct spi_inst *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset >= sizeof(s->regs)) return MM_FALSE;
    if (offset == SPI_RXDR) {
        mm_u32 v = 0;
        mm_u8 b;
        size_t i;
        mm_u32 frame_bytes = spi_frame_bytes(s);
        if (frame_bytes > size_bytes) {
            frame_bytes = size_bytes;
        }
        for (i = 0; i < frame_bytes; ++i) {
            if (!fifo_pop(s, &b)) {
                b = 0u;
            }
            v |= ((mm_u32)b << (i * 8u));
            if (spi_trace_enabled()) {
                printf("[SPI] SPI%d RXDR=0x%02x\n", s->bus_index, b);
            }
        }
        for (; i < size_bytes; ++i) {
            v |= 0u;
        }
        *value_out = v;
        update_sr(s);
        return MM_TRUE;
    }
    update_sr(s);
    memcpy(value_out, (mm_u8 *)s->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool spi_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct spi_inst *s = (struct spi_inst *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset >= sizeof(s->regs)) return MM_FALSE;
    if (offset == SPI_CR1) {
        mm_u32 prev = s->regs[SPI_CR1 / 4];
        mm_bool was_enabled = (prev & CR1_SPE) != 0u;
        mm_bool busy = (s->regs[SPI_SR / 4] & SR_BUSY) != 0u;
        s->regs[SPI_CR1 / 4] = value;
        if ((value & CR1_SPE) != 0u) {
            spi_set_enabled(s, MM_TRUE);
        } else if (!busy) {
            spi_set_enabled(s, MM_FALSE);
        } else {
            s->regs[SPI_CR1 / 4] |= CR1_SPE;
        }
        if ((value & CR1_CSTART) != 0u) {
            if (s->enabled) {
                spi_start_transfer(s);
            }
        }
        if (was_enabled && (value & CR1_SPE) == 0u) {
            mm_spi_bus_end(s->bus_index);
        }
        return MM_TRUE;
    }
    if (offset == SPI_TXDR) {
        spi_handle_tx(s, value, size_bytes);
        return MM_TRUE;
    }
    if (offset == SPI_IFCR) {
        mm_u32 sr = s->regs[SPI_SR / 4];
        if ((value & (1u << 3)) != 0u) {
            sr &= ~SR_EOT;
            s->eot_pending = MM_FALSE;
        }
        if ((value & (1u << 4)) != 0u) sr &= ~SR_TXTF;
        if ((value & (1u << 11)) != 0u) sr &= ~(1u << 11);
        s->regs[SPI_SR / 4] = sr;
        return MM_TRUE;
    }
    memcpy((mm_u8 *)s->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static void poll_instance(struct spi_inst *s)
{
    mm_u32 ier;
    mm_u32 sr;
    if (!s->enabled) return;
    update_sr(s);
    ier = s->regs[SPI_IER / 4];
    sr = s->regs[SPI_SR / 4];
    if (g_nvic == 0 || s->irq < 0) return;
    if ((ier & IER_RXPIE) && (sr & SR_RXP)) {
        mm_nvic_set_pending(g_nvic, (mm_u32)s->irq, MM_TRUE);
    }
    if ((ier & IER_TXPIE) && (sr & SR_TXP)) {
        mm_nvic_set_pending(g_nvic, (mm_u32)s->irq, MM_TRUE);
    }
    if ((ier & IER_DXPIE) && (sr & SR_DXP)) {
        mm_nvic_set_pending(g_nvic, (mm_u32)s->irq, MM_TRUE);
    }
    if ((ier & IER_EOTIE) && ((sr & (SR_EOT | SR_TXC)) != 0u)) {
        mm_nvic_set_pending(g_nvic, (mm_u32)s->irq, MM_TRUE);
    }
    if ((ier & IER_TXTFIE) && (sr & SR_TXTF)) {
        mm_nvic_set_pending(g_nvic, (mm_u32)s->irq, MM_TRUE);
    }
}

void mm_stm32h563_spi_poll(void)
{
    size_t i;
    for (i = 0; i < spi_count; ++i) {
        poll_instance(&spis[i]);
    }
}

void mm_stm32h563_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40013000u, 0x40003800u, 0x40003C00u, 0x40014C00u, 0x44002000u, 0x40015000u
    };
    static const mm_u32 bases_sec[] = {
        0x50013000u, 0x50003800u, 0x50003C00u, 0x50014C00u, 0x54002000u, 0x50015000u
    };
    static const int irq_map[] = { 55, 56, 57, 82, 83, 84 };
    size_t i;
    g_nvic = nvic;
    if (spi_count == 0) {
        spi_count = sizeof(bases) / sizeof(bases[0]);
    }
    for (i = 0; i < spi_count; ++i) {
        struct spi_inst *s = &spis[i];
        struct mmio_region reg;
        memset(s, 0, sizeof(*s));
        s->base = bases[i];
        s->bus_index = (int)(i + 1u);
        s->irq = (i < (sizeof(irq_map) / sizeof(irq_map[0]))) ? irq_map[i] : -1;
        s->regs[SPI_SR / 4] = SR_TXP;
        reg.base = bases[i];
        reg.size = 0x400u;
        reg.opaque = s;
        reg.read = spi_read;
        reg.write = spi_write;
        if (bus != 0) {
            (void)mmio_bus_register_region(bus, &reg);
            reg.base = bases_sec[i];
            (void)mmio_bus_register_region(bus, &reg);
        }
    }
}

void mm_stm32h563_spi_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(spis) / sizeof(spis[0]); ++i) {
        struct spi_inst *s = &spis[i];
        memset(s, 0, sizeof(*s));
    }
    spi_count = 0;
}
