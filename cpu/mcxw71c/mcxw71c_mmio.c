/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include "mcxw71c/mcxw71c_mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/gpio.h"
#include "m33mu/nvic.h"

#define MRCC_BASE 0x4001C000u
#define MRCC_SEC_BASE (MRCC_BASE + 0x10000000u)
#define MRCC_SIZE 0x800u

#define GPIOA_BASE 0x48010000u
#define GPIOB_BASE 0x48020000u
#define GPIOC_BASE 0x48030000u
#define GPIOD_BASE 0x40046000u
#define GPIO_SIZE  0x200u

#define GPIO_PDOR 0x40u
#define GPIO_PSOR 0x44u
#define GPIO_PCOR 0x48u
#define GPIO_PTOR 0x4Cu
#define GPIO_PDIR 0x50u
#define GPIO_PDDR 0x54u
#define GPIO_ISFR 0x120u

#define PORTA_BASE 0x40042000u
#define PORTB_BASE 0x40043000u
#define PORTC_BASE 0x40044000u
#define PORT_SIZE  0x200u

#define PORT_GPCLR 0x10u
#define PORT_GPCHR 0x14u
#define PORT_CONFIG 0x20u
#define PORT_EDFR  0x40u
#define PORT_EDIER 0x44u
#define PORT_EDCR  0x48u
#define PORT_PCR0  0x80u
#define PORT_PCR16 0xC0u

#define PORT_PCR_MUX_SHIFT 8
#define PORT_PCR_MUX_MASK (0xFu << PORT_PCR_MUX_SHIFT)

struct gpio_bank {
    mm_u32 regs[GPIO_SIZE / 4];
};

struct mrcc_state {
    mm_u32 regs[MRCC_SIZE / 4];
};

struct port_state {
    mm_u32 regs[PORT_SIZE / 4];
    mm_u32 pcr[32];
    mm_u32 last_pdir;
};

static struct gpio_bank gpio_banks[4];
static struct mrcc_state mrcc;
static struct port_state ports[3];
static struct mm_nvic *g_gpio_nvic = 0;

static int port_index_for_bank(int bank)
{
    if (bank >= 0 && bank < 3) return bank;
    return -1;
}

void mm_mcxw71c_gpio_set_nvic(struct mm_nvic *nvic)
{
    g_gpio_nvic = nvic;
}

static mm_bool gpio_bank_enabled(int bank)
{
    switch (bank) {
    case 0:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_GPIOA) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_GPIOA);
    case 1:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_GPIOB) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_GPIOB);
    case 2:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_GPIOC) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_GPIOC);
    default:
        return MM_TRUE;
    }
}

static void gpio_raise_irq(int bank)
{
    mm_u32 isfr;
    mm_u32 low;
    mm_u32 high;
    int irq0 = -1;
    int irq1 = -1;

    if (g_gpio_nvic == 0) return;
    if (bank < 0 || bank >= 4) return;

    isfr = gpio_banks[bank].regs[GPIO_ISFR / 4];
    low = isfr & 0xFFFFu;
    high = isfr & 0xFFFF0000u;

    switch (bank) {
    case 0: irq0 = 59; irq1 = 60; break;
    case 1: irq0 = 61; irq1 = 62; break;
    case 2: irq0 = 63; irq1 = 64; break;
    case 3: irq0 = 65; irq1 = 66; break;
    default: break;
    }

    if (irq0 >= 0) {
        mm_nvic_set_pending(g_gpio_nvic, (mm_u32)irq0, (low != 0u) ? MM_TRUE : MM_FALSE);
    }
    if (irq1 >= 0) {
        mm_nvic_set_pending(g_gpio_nvic, (mm_u32)irq1, (high != 0u) ? MM_TRUE : MM_FALSE);
    }
}

static void gpio_update_edges(int bank, mm_u32 old_pdir, mm_u32 new_pdir)
{
    mm_u32 delta = old_pdir ^ new_pdir;
    int port_idx = port_index_for_bank(bank);
    if (delta == 0u) return;
    if (port_idx >= 0) {
        mm_u32 edier = ports[port_idx].regs[PORT_EDIER / 4];
        mm_u32 fired = delta & edier;
        if (fired != 0u) {
            ports[port_idx].regs[PORT_EDFR / 4] |= fired;
            gpio_banks[bank].regs[GPIO_ISFR / 4] |= fired;
            gpio_raise_irq(bank);
        }
    } else {
        gpio_banks[bank].regs[GPIO_ISFR / 4] |= delta;
        gpio_raise_irq(bank);
    }
}

static mm_bool gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct gpio_bank *g = (struct gpio_bank *)opaque;
    int bank = (int)(g - gpio_banks);
    if (g == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!gpio_bank_enabled(bank)) return MM_FALSE;
    if ((offset + size_bytes) > GPIO_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)g->regs + offset, size_bytes);
    return MM_TRUE;
}

static void gpio_sync_pdir(struct gpio_bank *g)
{
    if (g == 0) return;
    g->regs[GPIO_PDIR / 4] = g->regs[GPIO_PDOR / 4];
}

static mm_bool gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct gpio_bank *g = (struct gpio_bank *)opaque;
    int bank = (int)(g - gpio_banks);
    mm_u32 old_pdir;
    if (g == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!gpio_bank_enabled(bank)) return MM_FALSE;
    if ((offset + size_bytes) > GPIO_SIZE) return MM_FALSE;

    if (offset == GPIO_ISFR && size_bytes == 4) {
        g->regs[GPIO_ISFR / 4] &= ~value;
        gpio_raise_irq(bank);
        return MM_TRUE;
    }

    old_pdir = g->regs[GPIO_PDIR / 4];
    if (offset == GPIO_PDOR && size_bytes == 4) {
        g->regs[GPIO_PDOR / 4] = value;
        gpio_sync_pdir(g);
        gpio_update_edges(bank, old_pdir, g->regs[GPIO_PDIR / 4]);
        return MM_TRUE;
    }
    if (offset == GPIO_PSOR && size_bytes == 4) {
        g->regs[GPIO_PDOR / 4] |= value;
        gpio_sync_pdir(g);
        gpio_update_edges(bank, old_pdir, g->regs[GPIO_PDIR / 4]);
        return MM_TRUE;
    }
    if (offset == GPIO_PCOR && size_bytes == 4) {
        g->regs[GPIO_PDOR / 4] &= ~value;
        gpio_sync_pdir(g);
        gpio_update_edges(bank, old_pdir, g->regs[GPIO_PDIR / 4]);
        return MM_TRUE;
    }
    if (offset == GPIO_PTOR && size_bytes == 4) {
        g->regs[GPIO_PDOR / 4] ^= value;
        gpio_sync_pdir(g);
        gpio_update_edges(bank, old_pdir, g->regs[GPIO_PDIR / 4]);
        return MM_TRUE;
    }
    if (offset == GPIO_PDDR && size_bytes == 4) {
        g->regs[GPIO_PDDR / 4] = value;
        gpio_update_edges(bank, old_pdir, g->regs[GPIO_PDIR / 4]);
        return MM_TRUE;
    }

    memcpy((mm_u8 *)g->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool port_enabled(int idx)
{
    switch (idx) {
    case 0:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_PORTA) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_PORTA);
    case 1:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_PORTB) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_PORTB);
    case 2:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_PORTC) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_PORTC);
    default:
        return MM_TRUE;
    }
}

static mm_bool port_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct port_state *p = (struct port_state *)opaque;
    int idx = (int)(p - ports);
    if (p == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!port_enabled(idx)) return MM_FALSE;
    if ((offset + size_bytes) > PORT_SIZE) return MM_FALSE;
    if (size_bytes == 4 && offset >= PORT_PCR0 && offset < PORT_PCR0 + 0x18u) {
        int pin = (int)((offset - PORT_PCR0) / 4);
        *value_out = p->pcr[pin];
        return MM_TRUE;
    }
    if (size_bytes == 4 && offset >= PORT_PCR16 && offset < PORT_PCR16 + 0x1Cu) {
        int pin = 16 + (int)((offset - PORT_PCR16) / 4);
        *value_out = p->pcr[pin];
        return MM_TRUE;
    }
    memcpy(value_out, (mm_u8 *)p->regs + offset, size_bytes);
    return MM_TRUE;
}

static void port_write_pcr(struct port_state *p, int pin, mm_u32 value)
{
    if (pin < 0 || pin >= 32) return;
    p->pcr[pin] = value;
}

static mm_bool port_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct port_state *p = (struct port_state *)opaque;
    int idx = (int)(p - ports);
    if (p == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!port_enabled(idx)) return MM_FALSE;
    if ((offset + size_bytes) > PORT_SIZE) return MM_FALSE;

    if (size_bytes == 4) {
        if (offset >= PORT_PCR0 && offset < PORT_PCR0 + 0x18u) {
            int pin = (int)((offset - PORT_PCR0) / 4);
            port_write_pcr(p, pin, value);
            return MM_TRUE;
        }
        if (offset >= PORT_PCR16 && offset < PORT_PCR16 + 0x1Cu) {
            int pin = 16 + (int)((offset - PORT_PCR16) / 4);
            port_write_pcr(p, pin, value);
            return MM_TRUE;
        }
        if (offset == PORT_GPCLR) {
            mm_u32 gpwe = value & 0xFFFFu;
            mm_u32 gpwd = (value >> 16) & 0xFFFFu;
            int pin;
            for (pin = 0; pin < 16; ++pin) {
                if ((gpwe >> pin) & 1u) {
                    port_write_pcr(p, pin, (p->pcr[pin] & 0xFFFF0000u) | gpwd);
                }
            }
            return MM_TRUE;
        }
        if (offset == PORT_GPCHR) {
            mm_u32 gpwe = value & 0xFFFFu;
            mm_u32 gpwd = (value >> 16) & 0xFFFFu;
            int pin;
            for (pin = 0; pin < 16; ++pin) {
                if ((gpwe >> pin) & 1u) {
                    int idx_pin = 16 + pin;
                    port_write_pcr(p, idx_pin, (p->pcr[idx_pin] & 0xFFFF0000u) | gpwd);
                }
            }
            return MM_TRUE;
        }
        if (offset == PORT_EDFR) {
            p->regs[PORT_EDFR / 4] &= ~value;
            return MM_TRUE;
        }
    }
    memcpy((mm_u8 *)p->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_u32 mcxw71c_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= 4) return 0;
    return gpio_banks[bank].regs[GPIO_PDOR / 4];
}

static mm_u32 mcxw71c_gpio_bank_read_moder(void *opaque, int bank)
{
    mm_u32 moder = 0;
    mm_u32 pddr;
    int pin;
    int port_idx;
    (void)opaque;
    if (bank < 0 || bank >= 4) return 0;
    pddr = gpio_banks[bank].regs[GPIO_PDDR / 4];
    port_idx = port_index_for_bank(bank);
    for (pin = 0; pin < 16; ++pin) {
        mm_u32 mux = 0;
        if (port_idx >= 0) {
            mux = (ports[port_idx].pcr[pin] & PORT_PCR_MUX_MASK) >> PORT_PCR_MUX_SHIFT;
        }
        if (mux == 0u) {
            /* Disabled */
        } else if (mux == 1u) {
            if ((pddr >> pin) & 1u) {
                moder |= (1u << (pin * 2));
            }
        } else {
            moder |= (2u << (pin * 2)); /* peripheral */
        }
    }
    return moder;
}

static mm_bool mcxw71c_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    switch (bank) {
    case 0:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_GPIOA) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_GPIOA);
    case 1:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_GPIOB) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_GPIOB);
    case 2:
        return mm_mcxw71c_mrcc_clock_on(MCXW71C_MRCC_GPIOC) &&
               mm_mcxw71c_mrcc_reset_released(MCXW71C_MRCC_GPIOC);
    default:
        return MM_TRUE;
    }
}

static mm_u32 mcxw71c_gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    (void)bank;
    return 0;
}

void mm_mcxw71c_mmio_reset(void)
{
    memset(gpio_banks, 0, sizeof(gpio_banks));
    memset(&mrcc, 0, sizeof(mrcc));
    memset(ports, 0, sizeof(ports));
    g_gpio_nvic = 0;
    mrcc.regs[MCXW71C_MRCC_LPIT0 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_LPSPI0 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_LPSPI1 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_LPUART0 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_LPUART1 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_PORTA / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_PORTB / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_PORTC / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_GPIOA / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_GPIOB / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_GPIOC / 4] = 0x80000000u;
    mm_gpio_bank_set_reader(mcxw71c_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(mcxw71c_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(mcxw71c_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(mcxw71c_gpio_bank_read_seccfgr, 0);
}

mm_bool mm_mcxw71c_mrcc_clock_on(mm_u32 offset)
{
    mm_u32 reg;
    if (offset >= MRCC_SIZE) return MM_FALSE;
    reg = mrcc.regs[offset / 4];
    return ((reg & 0x3u) != 0u) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_mcxw71c_mrcc_reset_released(mm_u32 offset)
{
    mm_u32 reg;
    if (offset >= MRCC_SIZE) return MM_FALSE;
    reg = mrcc.regs[offset / 4];
    return ((reg >> 30) & 1u) ? MM_TRUE : MM_FALSE;
}

static mm_bool mrcc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct mrcc_state *m = (struct mrcc_state *)opaque;
    if (m == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > MRCC_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)m->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool mrcc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct mrcc_state *m = (struct mrcc_state *)opaque;
    mm_u32 reg;
    if (m == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > MRCC_SIZE) return MM_FALSE;
    if (size_bytes != 4) return MM_FALSE;
    reg = value | 0x80000000u; /* PR bit stays set */
    m->regs[offset / 4] = reg;
    return MM_TRUE;
}

mm_bool mm_mcxw71c_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;

    reg.base = MRCC_BASE;
    reg.size = MRCC_SIZE;
    reg.opaque = &mrcc;
    reg.read = mrcc_read;
    reg.write = mrcc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = MRCC_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = GPIO_SIZE;
    reg.read = gpio_read;
    reg.write = gpio_write;

    /* GPIOA */
    reg.base = GPIOA_BASE;
    reg.opaque = &gpio_banks[0];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIOA_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GPIOB */
    reg.base = GPIOB_BASE;
    reg.opaque = &gpio_banks[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIOB_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GPIOC */
    reg.base = GPIOC_BASE;
    reg.opaque = &gpio_banks[2];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIOC_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* GPIOD */
    reg.base = GPIOD_BASE;
    reg.opaque = &gpio_banks[3];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIOD_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* PORTA/B/C */
    reg.base = PORTA_BASE;
    reg.size = PORT_SIZE;
    reg.opaque = &ports[0];
    reg.read = port_read;
    reg.write = port_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORTA_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = PORTB_BASE;
    reg.opaque = &ports[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORTB_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = PORTC_BASE;
    reg.opaque = &ports[2];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORTC_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    return MM_TRUE;
}

void mm_mcxw71c_flash_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist,
                           mm_u32 flags)
{
    (void)map;
    (void)flash;
    (void)flash_size;
    (void)persist;
    (void)flags;
}

mm_u64 mm_mcxw71c_cpu_hz(void)
{
    return 48000000ull;
}
