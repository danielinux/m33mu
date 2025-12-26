/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include "mcxw71c/mcxw71c_spi.h"
#include "mcxw71c/mcxw71c_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/spi_bus.h"

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

struct lpspi_inst {
    mm_u32 base;
    mm_u32 regs[0x80 / 4];
    int bus_index;
    mm_u8 last_rx;
    mm_bool rx_valid;
    mm_bool enabled;
    mm_u32 mrcc_offset;
};

static struct lpspi_inst spis[2];
static size_t spi_count = 0;

static void update_sr(struct lpspi_inst *s)
{
    mm_u32 sr = s->regs[LPSPI_SR / 4];
    sr |= SR_TDF;
    if (s->rx_valid) sr |= SR_RDF;
    s->regs[LPSPI_SR / 4] = sr;
}

static mm_bool spi_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct lpspi_inst *s = (struct lpspi_inst *)opaque;
    if (s == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!mm_mcxw71c_mrcc_clock_on(s->mrcc_offset) ||
        !mm_mcxw71c_mrcc_reset_released(s->mrcc_offset)) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > sizeof(s->regs)) return MM_FALSE;
    if (offset == LPSPI_RDR && size_bytes == 4) {
        mm_u32 v = s->rx_valid ? (mm_u32)s->last_rx : 0u;
        s->rx_valid = MM_FALSE;
        s->regs[LPSPI_SR / 4] &= ~SR_RDF;
        *value_out = v;
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)s->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool spi_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct lpspi_inst *s = (struct lpspi_inst *)opaque;
    mm_u32 tcr;
    if (s == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!mm_mcxw71c_mrcc_clock_on(s->mrcc_offset) ||
        !mm_mcxw71c_mrcc_reset_released(s->mrcc_offset)) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > sizeof(s->regs)) return MM_FALSE;

    if (offset == LPSPI_CR && size_bytes == 4) {
        s->regs[LPSPI_CR / 4] = value;
        if ((value & CR_RRF) != 0u) {
            s->rx_valid = MM_FALSE;
            s->regs[LPSPI_SR / 4] &= ~SR_RDF;
        }
        if ((value & CR_MEN) == 0u) {
            mm_spi_bus_end(s->bus_index);
        }
        update_sr(s);
        return MM_TRUE;
    }

    if (offset == LPSPI_TDR && size_bytes == 4) {
        mm_u8 out = (mm_u8)(value & 0xffu);
        mm_u8 in = 0xffu;
        if ((s->regs[LPSPI_CR / 4] & CR_MEN) != 0u) {
            tcr = s->regs[LPSPI_TCR / 4];
            if ((tcr & TCR_TXMSK) == 0u) {
                in = mm_spi_bus_xfer(s->bus_index, out);
            }
            if ((tcr & TCR_RXMSK) == 0u) {
                s->last_rx = in;
                s->rx_valid = MM_TRUE;
                s->regs[LPSPI_SR / 4] |= SR_RDF;
            }
            s->regs[LPSPI_SR / 4] |= SR_TCF;
            if ((tcr & TCR_CONT) == 0u) {
                mm_spi_bus_end(s->bus_index);
            }
        }
        update_sr(s);
        return MM_TRUE;
    }

    memcpy((mm_u8 *)s->regs + offset, &value, size_bytes);
    update_sr(s);
    return MM_TRUE;
}

void mm_mcxw71c_spi_poll(void)
{
    size_t i;
    for (i = 0; i < spi_count; ++i) {
        update_sr(&spis[i]);
    }
}

void mm_mcxw71c_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40036000u, 0x40037000u
    };
    static const mm_u32 mrcc_offsets[] = {
        MCXW71C_MRCC_LPSPI0, MCXW71C_MRCC_LPSPI1
    };
    size_t i;
    (void)nvic;
    spi_count = sizeof(bases) / sizeof(bases[0]);
    for (i = 0; i < spi_count; ++i) {
        struct lpspi_inst *s = &spis[i];
        struct mmio_region reg;
        memset(s, 0, sizeof(*s));
        s->base = bases[i];
        s->bus_index = (int)i;
        s->mrcc_offset = mrcc_offsets[i];
        s->regs[LPSPI_SR / 4] = SR_TDF;
        reg.base = bases[i];
        reg.size = 0x1000u;
        reg.opaque = s;
        reg.read = spi_read;
        reg.write = spi_write;
        mmio_bus_register_region(bus, &reg);
        reg.base = bases[i] + 0x10000000u;
        mmio_bus_register_region(bus, &reg);
    }
}

void mm_mcxw71c_spi_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(spis) / sizeof(spis[0]); ++i) {
        memset(&spis[i], 0, sizeof(spis[i]));
    }
    spi_count = 0;
}
