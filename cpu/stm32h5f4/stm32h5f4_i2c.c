/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>

#include "stm32h5f4/stm32h5f4_i2c.h"
#include "m33mu/i2c_bus.h"

#define STM32H5F4_I2C_COUNT 4
#define STM32_I2C_SIZE 0x400u

#define I2C_CR1     0x00u
#define I2C_CR2     0x04u
#define I2C_OAR1    0x08u
#define I2C_OAR2    0x0Cu
#define I2C_TIMINGR 0x10u
#define I2C_TIMEOUTR 0x14u
#define I2C_ISR     0x18u
#define I2C_ICR     0x1Cu
#define I2C_PECR    0x20u
#define I2C_RXDR    0x24u
#define I2C_TXDR    0x28u

#define CR1_PE      (1u << 0)

#define CR2_SADD_MASK 0x3ffu
#define CR2_RD_WRN   (1u << 10)
#define CR2_START    (1u << 13)
#define CR2_STOP     (1u << 14)
#define CR2_NBYTES_SHIFT 16u
#define CR2_NBYTES_MASK (0xffu << CR2_NBYTES_SHIFT)
#define CR2_RELOAD   (1u << 24)
#define CR2_AUTOEND  (1u << 25)

#define ISR_TXE      (1u << 0)
#define ISR_TXIS     (1u << 1)
#define ISR_RXNE     (1u << 2)
#define ISR_NACKF    (1u << 4)
#define ISR_STOPF    (1u << 5)
#define ISR_TC       (1u << 6)
#define ISR_TCR      (1u << 7)
#define ISR_BUSY     (1u << 15)

#define ICR_NACKCF   (1u << 4)
#define ICR_STOPCF   (1u << 5)

#define I2C_BUF_MAX 260u

struct stm32_i2c_inst {
    mm_u32 base;
    int bus_index;
    mm_u32 regs[STM32_I2C_SIZE / 4u];
    mm_u8 addr;
    mm_bool read_dir;
    mm_bool autoend;
    mm_bool active;
    size_t expected;
    size_t pos;
    mm_u8 tx_buf[I2C_BUF_MAX];
    mm_u8 rx_buf[I2C_BUF_MAX];
    size_t rx_len;
};

static const mm_u32 g_i2c_bases[STM32H5F4_I2C_COUNT] = {
    0x40005400u, 0x40005800u, 0x40005C00u, 0x40008400u
};

static const mm_u32 g_i2c_bases_sec[STM32H5F4_I2C_COUNT] = {
    0x50005400u, 0x50005800u, 0x50005C00u, 0x50008400u
};

static struct stm32_i2c_inst g_i2c[STM32H5F4_I2C_COUNT];
static mm_bool g_i2c_init_done;

static void i2c_set_idle(struct stm32_i2c_inst *i2c)
{
    i2c->active = MM_FALSE;
    i2c->regs[I2C_ISR / 4u] &= ~(ISR_BUSY | ISR_TXIS | ISR_RXNE);
    i2c->regs[I2C_ISR / 4u] |= ISR_TXE;
}

static void i2c_finish(struct stm32_i2c_inst *i2c)
{
    i2c->regs[I2C_ISR / 4u] &= ~(ISR_TXIS | ISR_RXNE);
    i2c->regs[I2C_ISR / 4u] |= ISR_TXE;
    if ((i2c->regs[I2C_CR2 / 4u] & CR2_RELOAD) != 0u) {
        i2c->regs[I2C_ISR / 4u] |= ISR_TCR;
    } else if (i2c->autoend) {
        i2c->regs[I2C_ISR / 4u] |= ISR_STOPF;
        i2c->regs[I2C_ISR / 4u] &= ~ISR_BUSY;
        i2c->active = MM_FALSE;
    } else {
        i2c->regs[I2C_ISR / 4u] |= ISR_TC;
    }
}

static mm_u8 i2c_cr2_addr(mm_u32 cr2)
{
    return (mm_u8)((cr2 & CR2_SADD_MASK) >> 1);
}

static size_t i2c_cr2_nbytes(mm_u32 cr2)
{
    return (size_t)((cr2 & CR2_NBYTES_MASK) >> CR2_NBYTES_SHIFT);
}

static void i2c_start(struct stm32_i2c_inst *i2c, mm_u32 cr2)
{
    size_t nbytes;
    i2c->regs[I2C_ISR / 4u] &= ~(ISR_NACKF | ISR_STOPF | ISR_TC | ISR_TCR | ISR_TXIS | ISR_RXNE);
    i2c->addr = i2c_cr2_addr(cr2);
    i2c->read_dir = ((cr2 & CR2_RD_WRN) != 0u) ? MM_TRUE : MM_FALSE;
    i2c->autoend = ((cr2 & CR2_AUTOEND) != 0u) ? MM_TRUE : MM_FALSE;
    nbytes = i2c_cr2_nbytes(cr2);
    if (nbytes > I2C_BUF_MAX) {
        nbytes = I2C_BUF_MAX;
    }
    i2c->expected = nbytes;
    i2c->pos = 0;
    i2c->rx_len = 0;
    i2c->active = MM_TRUE;
    i2c->regs[I2C_ISR / 4u] |= ISR_BUSY | ISR_TXE;

    if ((i2c->regs[I2C_CR1 / 4u] & CR1_PE) == 0u) {
        i2c->regs[I2C_ISR / 4u] |= ISR_NACKF;
        i2c_set_idle(i2c);
        return;
    }

    if (i2c->read_dir) {
        if (nbytes == 0u) {
            i2c_finish(i2c);
            return;
        }
        if (!mm_i2c_bus_read(i2c->bus_index, i2c->addr, i2c->rx_buf, nbytes)) {
            i2c->regs[I2C_ISR / 4u] |= ISR_NACKF | ISR_STOPF;
            i2c_set_idle(i2c);
            return;
        }
        i2c->rx_len = nbytes;
        i2c->regs[I2C_ISR / 4u] |= ISR_RXNE;
    } else {
        if (nbytes == 0u) {
            if (!mm_i2c_bus_write(i2c->bus_index, i2c->addr, i2c->tx_buf, 0u)) {
                i2c->regs[I2C_ISR / 4u] |= ISR_NACKF;
            }
            i2c_finish(i2c);
            return;
        }
        i2c->regs[I2C_ISR / 4u] |= ISR_TXIS;
    }
}

static void i2c_txdr_write(struct stm32_i2c_inst *i2c, mm_u8 value)
{
    if (!i2c->active || i2c->read_dir) {
        return;
    }
    if (i2c->pos < I2C_BUF_MAX) {
        i2c->tx_buf[i2c->pos] = value;
    }
    i2c->pos++;
    if (i2c->pos >= i2c->expected) {
        size_t len = i2c->expected;
        i2c->regs[I2C_ISR / 4u] &= ~ISR_TXIS;
        if (!mm_i2c_bus_write(i2c->bus_index, i2c->addr, i2c->tx_buf, len)) {
            i2c->regs[I2C_ISR / 4u] |= ISR_NACKF | ISR_STOPF;
            i2c_set_idle(i2c);
            return;
        }
        i2c_finish(i2c);
    } else {
        i2c->regs[I2C_ISR / 4u] |= ISR_TXIS | ISR_TXE;
    }
}

static mm_u8 i2c_rxdr_read(struct stm32_i2c_inst *i2c)
{
    mm_u8 v = 0xffu;
    if (i2c->pos < i2c->rx_len) {
        v = i2c->rx_buf[i2c->pos];
        i2c->pos++;
    }
    if (i2c->pos < i2c->rx_len) {
        i2c->regs[I2C_ISR / 4u] |= ISR_RXNE;
    } else {
        i2c->regs[I2C_ISR / 4u] &= ~ISR_RXNE;
        if (i2c->active && i2c->read_dir) {
            i2c_finish(i2c);
        }
    }
    return v;
}

static mm_bool i2c_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    struct stm32_i2c_inst *i2c = (struct stm32_i2c_inst *)opaque;
    mm_u32 v = 0;
    if (i2c == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > STM32_I2C_SIZE) {
        return MM_FALSE;
    }
    if (offset == I2C_RXDR && size_bytes <= 4u) {
        v = (mm_u32)i2c_rxdr_read(i2c);
        memcpy(value_out, &v, size_bytes);
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)i2c->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool i2c_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    struct stm32_i2c_inst *i2c = (struct stm32_i2c_inst *)opaque;
    mm_u32 clear;
    if (i2c == 0 || size_bytes == 0u || size_bytes > 4u) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > STM32_I2C_SIZE) {
        return MM_FALSE;
    }

    if (offset == I2C_CR2 && size_bytes == 4u) {
        i2c->regs[I2C_CR2 / 4u] = value & ~(CR2_START | CR2_STOP);
        if ((value & CR2_START) != 0u) {
            i2c_start(i2c, value);
        }
        if ((value & CR2_STOP) != 0u) {
            i2c->regs[I2C_ISR / 4u] |= ISR_STOPF;
            i2c_set_idle(i2c);
        }
        return MM_TRUE;
    }
    if (offset == I2C_TXDR && size_bytes <= 4u) {
        i2c->regs[I2C_TXDR / 4u] = value & 0xffu;
        i2c_txdr_write(i2c, (mm_u8)(value & 0xffu));
        return MM_TRUE;
    }
    if (offset == I2C_ICR && size_bytes == 4u) {
        clear = 0u;
        if ((value & ICR_NACKCF) != 0u) {
            clear |= ISR_NACKF;
        }
        if ((value & ICR_STOPCF) != 0u) {
            clear |= ISR_STOPF;
        }
        i2c->regs[I2C_ISR / 4u] &= ~clear;
        return MM_TRUE;
    }
    if (offset == I2C_CR1 && size_bytes == 4u) {
        i2c->regs[I2C_CR1 / 4u] = value;
        if ((value & CR1_PE) == 0u) {
            i2c_set_idle(i2c);
        }
        return MM_TRUE;
    }
    memcpy((mm_u8 *)i2c->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

void mm_stm32h5f4_i2c_reset(void)
{
    size_t i;
    g_i2c_init_done = MM_FALSE;
    for (i = 0; i < STM32H5F4_I2C_COUNT; ++i) {
        mm_u32 base = g_i2c[i].base;
        int bus_index = g_i2c[i].bus_index;
        memset(&g_i2c[i], 0, sizeof(g_i2c[i]));
        g_i2c[i].base = base;
        g_i2c[i].bus_index = bus_index;
        g_i2c[i].regs[I2C_ISR / 4u] = ISR_TXE;
    }
}

void mm_stm32h5f4_i2c_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    size_t i;
    (void)nvic;
    if (g_i2c_init_done) {
        return;
    }
    g_i2c_init_done = MM_TRUE;
    for (i = 0; i < STM32H5F4_I2C_COUNT; ++i) {
        struct mmio_region reg;
        memset(&g_i2c[i], 0, sizeof(g_i2c[i]));
        g_i2c[i].base = g_i2c_bases[i];
        g_i2c[i].bus_index = (int)i + 1;
        g_i2c[i].regs[I2C_ISR / 4u] = ISR_TXE;

        reg.base = g_i2c_bases[i];
        reg.size = STM32_I2C_SIZE;
        reg.opaque = &g_i2c[i];
        reg.read = i2c_read;
        reg.write = i2c_write;
        mmio_bus_register_region(bus, &reg);

        reg.base = g_i2c_bases_sec[i];
        mmio_bus_register_region(bus, &reg);
    }
}
