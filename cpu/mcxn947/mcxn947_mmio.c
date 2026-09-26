/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "mcxn947/mcxn947_mmio.h"
#include "mcxn947/mcxn947_romapi.h"
#include "mcxn947/mcxn947_secure.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/gpio.h"
#include "m33mu/nvic.h"

#define SYSCON_BASE 0x40000000u
#define SYSCON_SEC_BASE (SYSCON_BASE + 0x10000000u)
#define SYSCON_SIZE 0x1000u

#define GPIO0_BASE 0x40096000u
#define GPIO1_BASE 0x40097000u
#define GPIO2_BASE 0x40098000u
#define GPIO3_BASE 0x40099000u
#define GPIO4_BASE 0x4009A000u
#define GPIO5_BASE 0x4009B000u
#define GPIO_SIZE  0x128u

#define GPIO_PDOR 0x20u
#define GPIO_PSOR 0x24u
#define GPIO_PCOR 0x28u
#define GPIO_PTOR 0x2Cu
#define GPIO_PDIR 0x30u
#define GPIO_PDDR 0x34u
#define GPIO_PIDR 0x38u
#define GPIO_ISFR0 0x100u
#define GPIO_ISFR1 0x104u

#define PORT0_BASE 0x40116000u
#define PORT1_BASE 0x40117000u
#define PORT2_BASE 0x40118000u
#define PORT3_BASE 0x40119000u
#define PORT4_BASE 0x4011A000u
#define PORT5_BASE 0x4011B000u
#define PORT_SIZE  0x100u

#define PORT_PCR_BASE 0x20u
#define PORT_PCR_COUNT 32
#define PORT_GPCLR 0xC0u
#define PORT_GPCHR 0xC4u
#define PORT_ISFR0 0xE0u
#define PORT_ISFR1 0xE4u
#define PORT_DFER0 0xF0u
#define PORT_DIER0 0xF8u

#define PORT_PCR_MUX_SHIFT 8
#define PORT_PCR_MUX_MASK (0xFu << PORT_PCR_MUX_SHIFT)

#define SCG_BASE 0x40044000u
#define SCG_SEC_BASE 0x50044000u
#define SCG_SIZE 0x804u

#define SCG_VERID 0x000u
#define SCG_PARAM 0x004u
#define SCG_CSR 0x010u
#define SCG_RCCR 0x014u
#define SCG_SOSCCSR 0x100u
#define SCG_SIRCCSR 0x200u
#define SCG_FIRCCSR 0x300u
#define SCG_APLLCSR 0x500u
#define SCG_SPLLCSR 0x600u
#define SCG_UPLLCSR 0x700u

#define FMU_BASE 0x40043000u
#define FMU_SEC_BASE 0x50043000u
#define FMU_SIZE 0x30u

#define FMU_FSTAT 0x00u
#define FMU_FCNFG 0x04u
#define FMU_FCTRL 0x08u
#define FMU_FCCOB0 0x10u

#define TRDC_BASE 0x400C7000u
#define TRDC_SEC_BASE 0x500C7000u
#define TRDC_SIZE 0x1CCu

#define TRDC_MBC0_MEM0_GLBCFG 0x00u
#define TRDC_MBC0_MEM1_GLBCFG 0x04u
#define TRDC_MBC0_MEM2_GLBCFG 0x08u
#define TRDC_MBC0_MEM3_GLBCFG 0x0Cu
#define TRDC_MBC0_NSE_BLK_INDEX 0x10u
#define TRDC_MBC0_NSE_BLK_SET 0x14u
#define TRDC_MBC0_NSE_BLK_CLR 0x18u
#define TRDC_MBC0_NSE_BLK_CLR_ALL 0x1Cu
#define TRDC_MBC0_MEMN_GLBAC0 0x20u
#define TRDC_MBC0_MEMN_GLBAC7 0x3Cu

#define TRDC_NSE_WNDX_SHIFT 2u
#define TRDC_NSE_WNDX_MASK (0xFu << TRDC_NSE_WNDX_SHIFT)
#define TRDC_NSE_MEM_SEL_SHIFT 8u
#define TRDC_NSE_MEM_SEL_MASK (0xFu << TRDC_NSE_MEM_SEL_SHIFT)
#define TRDC_NSE_DID_SEL0 (1u << 16)
#define TRDC_NSE_AI (1u << 31)

/* SYSCON register offsets */
#define SYSCON_PRESETCTRL0 0x100u
#define SYSCON_AHBCLKCTRL0 0x200u
#define SYSCON_AHBCLKCTRL1 0x204u

struct gpio_bank {
    mm_u32 regs[GPIO_SIZE / 4];
};

struct syscon_state {
    mm_u32 regs[SYSCON_SIZE / 4];
};

struct port_state {
    mm_u32 regs[PORT_SIZE / 4];
    mm_u32 pcr[PORT_PCR_COUNT];
    mm_u32 last_pdir;
};

struct scg_state {
    mm_u32 regs[SCG_SIZE / 4u];
};

struct fmu_state {
    mm_u32 regs[FMU_SIZE / 4u];
};

struct trdc_state {
    mm_u32 regs[TRDC_SIZE / 4u];
    mm_u32 nse_words[4][2];
};

static struct gpio_bank gpio_banks[6];
static struct syscon_state syscon;
static struct port_state ports[6];
static struct scg_state scg;
static struct fmu_state fmu;
static struct trdc_state trdc;
static struct mm_nvic *g_gpio_nvic = 0;

static int port_index_for_bank(int bank)
{
    if (bank >= 0 && bank < 6) return bank;
    return -1;
}

void mm_mcxn947_gpio_set_nvic(struct mm_nvic *nvic)
{
    g_gpio_nvic = nvic;
}

static mm_bool gpio_bank_enabled(int bank)
{
    mm_u32 reg;
    if (bank < 0 || bank >= 6) return MM_FALSE;

    /* GPIO0 clock is at bit 19 of AHBCLKCTRL0 */
    reg = syscon.regs[SYSCON_AHBCLKCTRL0 / 4];
    if (!((reg >> (19 + bank)) & 1u)) return MM_FALSE;

    /* GPIO0 reset is at bit 19 of PRESETCTRL0 */
    reg = syscon.regs[SYSCON_PRESETCTRL0 / 4];
    return ((reg >> (19 + bank)) & 1u) ? MM_TRUE : MM_FALSE;
}

static void gpio_raise_irq(int bank)
{
    mm_u32 isfr0, isfr1;
    int irq0 = -1;
    int irq1 = -1;

    if (g_gpio_nvic == 0) return;
    if (bank < 0 || bank >= 6) return;

    isfr0 = gpio_banks[bank].regs[GPIO_ISFR0 / 4];
    isfr1 = gpio_banks[bank].regs[GPIO_ISFR1 / 4];

    switch (bank) {
    case 0: irq0 = 17; irq1 = 18; break;
    case 1: irq0 = 19; irq1 = 20; break;
    case 2: irq0 = 21; irq1 = 22; break;
    case 3: irq0 = 23; irq1 = 24; break;
    case 4: irq0 = 25; irq1 = 26; break;
    case 5: irq0 = 27; irq1 = 28; break;
    default: break;
    }

    if (irq0 >= 0) {
        mm_nvic_set_pending(g_gpio_nvic, (mm_u32)irq0, (isfr0 != 0u) ? MM_TRUE : MM_FALSE);
    }
    if (irq1 >= 0) {
        mm_nvic_set_pending(g_gpio_nvic, (mm_u32)irq1, (isfr1 != 0u) ? MM_TRUE : MM_FALSE);
    }
}

static void gpio_update_edges(int bank, mm_u32 old_pdir, mm_u32 new_pdir)
{
    mm_u32 delta = old_pdir ^ new_pdir;
    int port_idx = port_index_for_bank(bank);
    if (delta == 0u) return;
    if (bank < 0 || bank >= 6) return;
    if (port_idx >= 0) {
        mm_u32 dier0 = ports[port_idx].regs[PORT_DIER0 / 4];
        mm_u32 fired = delta & dier0;
        if (fired != 0u) {
            ports[port_idx].regs[PORT_ISFR0 / 4] |= fired;
            gpio_banks[bank].regs[GPIO_ISFR0 / 4] |= fired;
            gpio_raise_irq(bank);
        }
    } else {
        gpio_banks[bank].regs[GPIO_ISFR0 / 4] |= delta;
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

    if (offset == GPIO_ISFR0 && size_bytes == 4) {
        g->regs[GPIO_ISFR0 / 4] &= ~value;
        gpio_raise_irq(bank);
        return MM_TRUE;
    }
    if (offset == GPIO_ISFR1 && size_bytes == 4) {
        g->regs[GPIO_ISFR1 / 4] &= ~value;
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
    mm_u32 reg;
    if (idx < 0 || idx >= 6) return MM_FALSE;

    /* PORT0 clock is at bit 13 of AHBCLKCTRL0 */
    reg = syscon.regs[SYSCON_AHBCLKCTRL0 / 4];
    if (!((reg >> (13 + idx)) & 1u)) return MM_FALSE;

    /* PORT0 reset is at bit 13 of PRESETCTRL0 */
    reg = syscon.regs[SYSCON_PRESETCTRL0 / 4];
    return ((reg >> (13 + idx)) & 1u) ? MM_TRUE : MM_FALSE;
}

static mm_bool port_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct port_state *p = (struct port_state *)opaque;
    int idx = (int)(p - ports);
    if (p == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!port_enabled(idx)) return MM_FALSE;
    if ((offset + size_bytes) > PORT_SIZE) return MM_FALSE;

    if (size_bytes == 4 && offset >= PORT_PCR_BASE && offset < (PORT_PCR_BASE + PORT_PCR_COUNT * 4)) {
        int pin = (int)((offset - PORT_PCR_BASE) / 4);
        if (pin >= 0 && pin < PORT_PCR_COUNT) {
            *value_out = p->pcr[pin];
            return MM_TRUE;
        }
    }

    memcpy(value_out, (mm_u8 *)p->regs + offset, size_bytes);
    return MM_TRUE;
}

static void port_write_pcr(struct port_state *p, int pin, mm_u32 value)
{
    if (pin < 0 || pin >= PORT_PCR_COUNT) return;
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
        if (offset >= PORT_PCR_BASE && offset < (PORT_PCR_BASE + PORT_PCR_COUNT * 4)) {
            int pin = (int)((offset - PORT_PCR_BASE) / 4);
            if (pin >= 0 && pin < PORT_PCR_COUNT) {
                port_write_pcr(p, pin, value);
                return MM_TRUE;
            }
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
        if (offset == PORT_ISFR0) {
            p->regs[PORT_ISFR0 / 4] &= ~value;
            return MM_TRUE;
        }
        if (offset == PORT_ISFR1) {
            p->regs[PORT_ISFR1 / 4] &= ~value;
            return MM_TRUE;
        }
    }
    memcpy((mm_u8 *)p->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_u32 mcxn947_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= 6) return 0;
    return gpio_banks[bank].regs[GPIO_PDOR / 4];
}

static mm_u32 mcxn947_gpio_bank_read_moder(void *opaque, int bank)
{
    mm_u32 moder = 0;
    mm_u32 pddr;
    int pin;
    int port_idx;
    (void)opaque;
    if (bank < 0 || bank >= 6) return 0;
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

static mm_bool mcxn947_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    return gpio_bank_enabled(bank);
}

static mm_u32 mcxn947_gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    (void)bank;
    return 0;
}

static mm_bool mcxn947_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    static const int pin_counts[] = { 32, 32, 32, 32, 32, 32 };
    (void)opaque;
    if (bank < 0 || bank >= 6) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0u) {
        name_out[0] = (char)('0' + bank);
        if (name_len > 1u) {
            name_out[1] = '\0';
        }
    }
    if (pins_out != 0) {
        *pins_out = pin_counts[bank];
    }
    return MM_TRUE;
}

struct syscon_clk_name {
    const char *name;
    mm_u32 reg_offset;
    mm_u32 bit;
};

static const struct syscon_clk_name syscon_clk_names[] = {
    { "FC0", SYSCON_AHBCLKCTRL0, 11 },
    { "FC1", SYSCON_AHBCLKCTRL0, 12 },
    { "PORT0", SYSCON_AHBCLKCTRL0, 13 },
    { "PORT1", SYSCON_AHBCLKCTRL0, 14 },
    { "PORT2", SYSCON_AHBCLKCTRL0, 15 },
    { "PORT3", SYSCON_AHBCLKCTRL0, 16 },
    { "PORT4", SYSCON_AHBCLKCTRL0, 17 },
    { "PORT5", SYSCON_AHBCLKCTRL0, 18 },
    { "GPIO0", SYSCON_AHBCLKCTRL0, 19 },
    { "GPIO1", SYSCON_AHBCLKCTRL0, 20 },
    { "GPIO2", SYSCON_AHBCLKCTRL0, 21 },
    { "GPIO3", SYSCON_AHBCLKCTRL0, 22 },
    { "GPIO4", SYSCON_AHBCLKCTRL0, 23 },
    { "GPIO5", SYSCON_AHBCLKCTRL0, 24 }
};

static mm_bool mcxn947_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len)
{
    char buf[256];
    size_t pos = 0;
    size_t i;
    mm_bool have = MM_FALSE;
    (void)opaque;
    if (out == 0 || out_len == 0u) {
        return MM_FALSE;
    }
    if (line != 0) {
        return MM_FALSE;
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SYSCON:");
    for (i = 0; i < sizeof(syscon_clk_names) / sizeof(syscon_clk_names[0]); ++i) {
        mm_u32 reg = syscon.regs[syscon_clk_names[i].reg_offset / 4];
        if (!((reg >> syscon_clk_names[i].bit) & 1u)) {
            continue;
        }
        if (pos + 2u < sizeof(buf)) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", syscon_clk_names[i].name);
            have = MM_TRUE;
        }
    }
    if (!have) {
        return MM_FALSE;
    }
    snprintf(out, out_len, "%s", buf);
    return MM_TRUE;
}

static mm_bool scg_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct scg_state *s = (struct scg_state *)opaque;
    if (s == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SCG_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)s->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool scg_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct scg_state *s = (struct scg_state *)opaque;
    const mm_u32 scs_mask = 0x0F000000u;
    const mm_u32 apll_lock = (1u << 24);
    const mm_u32 apll_en = (1u << 0) | (1u << 1);
    if (s == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SCG_SIZE) return MM_FALSE;
    if (size_bytes != 4) return MM_FALSE;

    if (offset == SCG_VERID || offset == SCG_PARAM) {
        return MM_TRUE;
    }
    s->regs[offset / 4u] = value;
    if (offset == SCG_RCCR) {
        mm_u32 csr = s->regs[SCG_CSR / 4u] & ~scs_mask;
        s->regs[SCG_CSR / 4u] = csr | (value & scs_mask);
    } else if (offset == SCG_APLLCSR) {
        if ((value & apll_en) != 0u) {
            s->regs[offset / 4u] |= apll_lock;
        } else {
            s->regs[offset / 4u] &= ~apll_lock;
        }
    }
    return MM_TRUE;
}

static mm_bool fmu_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct fmu_state *f = (struct fmu_state *)opaque;
    if (f == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FMU_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)f->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool fmu_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct fmu_state *f = (struct fmu_state *)opaque;
    if (f == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FMU_SIZE) return MM_FALSE;
    if (size_bytes != 4) return MM_FALSE;

    if (offset == FMU_FSTAT) {
        f->regs[FMU_FSTAT / 4u] &= ~value;
        return MM_TRUE;
    }
    f->regs[offset / 4u] = value;
    return MM_TRUE;
}

static mm_u32 trdc_mem_nblks(int mem)
{
    mm_u32 glbcfg;
    if (mem < 0 || mem > 3) return 0u;
    glbcfg = trdc.regs[(TRDC_MBC0_MEM0_GLBCFG / 4u) + (mm_u32)mem];
    return glbcfg & 0x3FFu;
}

static mm_u32 trdc_mem_words(int mem)
{
    mm_u32 nblks = trdc_mem_nblks(mem);
    return (nblks + 31u) / 32u;
}

static void trdc_nse_apply(mm_u32 value, mm_bool set_bits)
{
    mm_u32 index = trdc.regs[TRDC_MBC0_NSE_BLK_INDEX / 4u];
    mm_u32 mem_sel = (index & TRDC_NSE_MEM_SEL_MASK) >> TRDC_NSE_MEM_SEL_SHIFT;
    mm_u32 wndx = (index & TRDC_NSE_WNDX_MASK) >> TRDC_NSE_WNDX_SHIFT;
    mm_u32 word_count;
    if ((index & TRDC_NSE_DID_SEL0) == 0u) return;
    if (mem_sel >= 4u) return;
    word_count = trdc_mem_words((int)mem_sel);
    if (wndx >= word_count) return;
    if (set_bits) {
        trdc.nse_words[mem_sel][wndx] |= value;
    } else {
        trdc.nse_words[mem_sel][wndx] &= ~value;
    }
    if ((index & TRDC_NSE_AI) != 0u) {
        mm_u32 next = (wndx + 1u) & 0xFu;
        index &= ~TRDC_NSE_WNDX_MASK;
        index |= (next << TRDC_NSE_WNDX_SHIFT) & TRDC_NSE_WNDX_MASK;
        trdc.regs[TRDC_MBC0_NSE_BLK_INDEX / 4u] = index;
    }
}

static mm_bool trdc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct trdc_state *t = (struct trdc_state *)opaque;
    if (t == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > TRDC_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)t->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool trdc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct trdc_state *t = (struct trdc_state *)opaque;
    mm_u32 reg_index;
    if (t == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > TRDC_SIZE) return MM_FALSE;
    if (size_bytes != 4) return MM_FALSE;

    reg_index = offset / 4u;
    if (offset == TRDC_MBC0_MEM0_GLBCFG || offset == TRDC_MBC0_MEM1_GLBCFG ||
        offset == TRDC_MBC0_MEM2_GLBCFG) {
        return MM_TRUE;
    }
    if (offset == TRDC_MBC0_MEM3_GLBCFG) {
        mm_u32 keep = t->regs[reg_index] & 0x3FFFFFFFu;
        t->regs[reg_index] = keep | (value & 0xC0000000u);
        return MM_TRUE;
    }
    if (offset == TRDC_MBC0_NSE_BLK_INDEX) {
        t->regs[reg_index] = value;
        return MM_TRUE;
    }
    if (offset == TRDC_MBC0_NSE_BLK_SET) {
        t->regs[reg_index] = value;
        trdc_nse_apply(value, MM_TRUE);
        return MM_TRUE;
    }
    if (offset == TRDC_MBC0_NSE_BLK_CLR) {
        t->regs[reg_index] = value;
        trdc_nse_apply(value, MM_FALSE);
        return MM_TRUE;
    }
    if (offset == TRDC_MBC0_NSE_BLK_CLR_ALL) {
        mm_u32 mem_sel = (value & TRDC_NSE_MEM_SEL_MASK) >> TRDC_NSE_MEM_SEL_SHIFT;
        t->regs[reg_index] = value;
        if ((value & TRDC_NSE_DID_SEL0) != 0u && mem_sel < 4u) {
            mm_u32 words = trdc_mem_words((int)mem_sel);
            mm_u32 i;
            for (i = 0; i < words; ++i) {
                trdc.nse_words[mem_sel][i] = 0u;
            }
        }
        return MM_TRUE;
    }
    if (offset >= TRDC_MBC0_MEMN_GLBAC0 && offset <= TRDC_MBC0_MEMN_GLBAC7) {
        mm_u32 current = t->regs[reg_index];
        if ((current & 0x80000000u) != 0u) {
            return MM_TRUE;
        }
        t->regs[reg_index] = value;
        return MM_TRUE;
    }

    t->regs[reg_index] = value;
    return MM_TRUE;
}

void mm_mcxn947_mmio_reset(void)
{
    memset(gpio_banks, 0, sizeof(gpio_banks));
    memset(&syscon, 0, sizeof(syscon));
    memset(ports, 0, sizeof(ports));
    memset(&scg, 0, sizeof(scg));
    memset(&fmu, 0, sizeof(fmu));
    memset(&trdc, 0, sizeof(trdc));
    g_gpio_nvic = 0;

    /* Initialize SYSCON reset states */
    syscon.regs[SYSCON_AHBCLKCTRL0 / 4] = 0x021FFE603u; /* + ELS clock bit */
    syscon.regs[SYSCON_PRESETCTRL0 / 4] = 0x021FFE000u; /* + ELS reset released */

    scg.regs[SCG_VERID / 4u] = 0x00000000u;
    scg.regs[SCG_PARAM / 4u] = 0x000001FEu;
    scg.regs[SCG_CSR / 4u] = 0x03000000u;
    scg.regs[SCG_RCCR / 4u] = 0x03000000u;
    scg.regs[SCG_SOSCCSR / 4u] = 0x00000000u;
    scg.regs[SCG_SIRCCSR / 4u] = 0x01000020u;
    scg.regs[SCG_FIRCCSR / 4u] = 0x03000031u;
    scg.regs[SCG_APLLCSR / 4u] = 0x00000000u;
    scg.regs[SCG_SPLLCSR / 4u] = 0x00000000u;
    scg.regs[SCG_UPLLCSR / 4u] = 0x00000000u;

    fmu.regs[FMU_FSTAT / 4u] = 0x00000080u;
    fmu.regs[FMU_FCNFG / 4u] = 0x00000000u;
    fmu.regs[FMU_FCTRL / 4u] = 0x00000000u;
    fmu.regs[FMU_FCCOB0 / 4u] = 0x00000000u;

    trdc.regs[TRDC_MBC0_MEM0_GLBCFG / 4u] = 0x000F0040u;
    trdc.regs[TRDC_MBC0_MEM1_GLBCFG / 4u] = 0x000D0008u;
    trdc.regs[TRDC_MBC0_MEM2_GLBCFG / 4u] = 0x000C0004u;
    trdc.regs[TRDC_MBC0_MEM3_GLBCFG / 4u] = 0x00000000u;
    trdc.regs[TRDC_MBC0_MEMN_GLBAC0 / 4u] = 0x00006600u;
    trdc.regs[(TRDC_MBC0_MEMN_GLBAC0 / 4u) + 1u] = 0x80006600u;
    trdc.regs[(TRDC_MBC0_MEMN_GLBAC0 / 4u) + 2u] = 0x80005500u;
    trdc.regs[(TRDC_MBC0_MEMN_GLBAC0 / 4u) + 3u] = 0x80004400u;
    trdc.regs[(TRDC_MBC0_MEMN_GLBAC0 / 4u) + 4u] = 0x00005500u;
    trdc.regs[(TRDC_MBC0_MEMN_GLBAC0 / 4u) + 5u] = 0x00001100u;
    trdc.regs[(TRDC_MBC0_MEMN_GLBAC0 / 4u) + 6u] = 0x80001100u;
    trdc.regs[(TRDC_MBC0_MEMN_GLBAC0 / 4u) + 7u] = 0x80000000u;

    {
        mm_u32 words = trdc_mem_words(0);
        mm_u32 i;
        if (words > (mm_u32)(sizeof(trdc.nse_words[0]) / sizeof(trdc.nse_words[0][0]))) {
            words = (mm_u32)(sizeof(trdc.nse_words[0]) / sizeof(trdc.nse_words[0][0]));
        }
        for (i = 0; i < words; ++i) {
            trdc.nse_words[0][i] = 0xffffffffu;
        }
    }

    mm_gpio_bank_set_reader(mcxn947_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(mcxn947_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(mcxn947_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(mcxn947_gpio_bank_read_seccfgr, 0);
    mm_gpio_set_bank_info_reader(mcxn947_gpio_bank_info, 0);
    mm_rcc_set_clock_list_reader(mcxn947_rcc_clock_list_line, 0);
    mm_mcxn947_secure_reset();
    mm_mcxn947_romapi_reset();
}

mm_bool mm_mcxn947_syscon_clock_on(mm_u32 reg_offset)
{
    mm_u32 reg;
    if (reg_offset >= SYSCON_SIZE) return MM_FALSE;
    reg = syscon.regs[reg_offset / 4];
    return (reg != 0u) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_mcxn947_syscon_clock_bit_on(mm_u32 reg_offset, mm_u32 bit)
{
    mm_u32 reg;
    if (reg_offset >= SYSCON_SIZE || bit >= 32u) return MM_FALSE;
    reg = syscon.regs[reg_offset / 4u];
    return ((reg >> bit) & 1u) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_mcxn947_syscon_reset_released(mm_u32 reg_offset)
{
    mm_u32 reg;
    if (reg_offset >= SYSCON_SIZE) return MM_FALSE;
    reg = syscon.regs[reg_offset / 4];
    return (reg != 0u) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_mcxn947_syscon_reset_bit_released(mm_u32 reg_offset, mm_u32 bit)
{
    mm_u32 reg;
    if (reg_offset >= SYSCON_SIZE || bit >= 32u) return MM_FALSE;
    reg = syscon.regs[reg_offset / 4u];
    return ((reg >> bit) & 1u) ? MM_TRUE : MM_FALSE;
}

static mm_bool syscon_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct syscon_state *s = (struct syscon_state *)opaque;
    if (s == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SYSCON_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)s->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool syscon_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct syscon_state *s = (struct syscon_state *)opaque;
    if (s == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SYSCON_SIZE) return MM_FALSE;
    if (size_bytes != 4) return MM_FALSE;
    s->regs[offset / 4] = value;
    return MM_TRUE;
}

mm_bool mm_mcxn947_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;

    if (!mm_mcxn947_romapi_register_mmio(bus)) return MM_FALSE;

    /* SYSCON */
    reg.base = SYSCON_BASE;
    reg.size = SYSCON_SIZE;
    reg.opaque = &syscon;
    reg.read = syscon_read;
    reg.write = syscon_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = SYSCON_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = GPIO_SIZE;
    reg.read = gpio_read;
    reg.write = gpio_write;

    /* GPIO0-5 */
    reg.base = GPIO0_BASE;
    reg.opaque = &gpio_banks[0];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO0_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO1_BASE;
    reg.opaque = &gpio_banks[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO1_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO2_BASE;
    reg.opaque = &gpio_banks[2];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO2_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO3_BASE;
    reg.opaque = &gpio_banks[3];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO3_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO4_BASE;
    reg.opaque = &gpio_banks[4];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO4_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO5_BASE;
    reg.opaque = &gpio_banks[5];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO5_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* PORT0-5 */
    reg.size = PORT_SIZE;
    reg.read = port_read;
    reg.write = port_write;

    reg.base = PORT0_BASE;
    reg.opaque = &ports[0];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORT0_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = PORT1_BASE;
    reg.opaque = &ports[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORT1_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = PORT2_BASE;
    reg.opaque = &ports[2];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORT2_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = PORT3_BASE;
    reg.opaque = &ports[3];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORT3_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = PORT4_BASE;
    reg.opaque = &ports[4];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORT4_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = PORT5_BASE;
    reg.opaque = &ports[5];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PORT5_BASE + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* SCG */
    reg.base = SCG_BASE;
    reg.size = SCG_SIZE;
    reg.opaque = &scg;
    reg.read = scg_read;
    reg.write = scg_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = SCG_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* FMU (Flash) */
    reg.base = FMU_BASE;
    reg.size = FMU_SIZE;
    reg.opaque = &fmu;
    reg.read = fmu_read;
    reg.write = fmu_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = FMU_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    /* TRDC (MBC) */
    reg.base = TRDC_BASE;
    reg.size = TRDC_SIZE;
    reg.opaque = &trdc;
    reg.read = trdc_read;
    reg.write = trdc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = TRDC_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    if (!mm_mcxn947_secure_register_mmio(bus)) return MM_FALSE;

    return MM_TRUE;
}

void mm_mcxn947_flash_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist,
                           mm_u32 flags)
{
    mm_mcxn947_romapi_set_persist(persist);
    mm_mcxn947_secure_flash_bind(map, flash, flash_size, persist, flags);
}

mm_u64 mm_mcxn947_cpu_hz(void)
{
    return 150000000ull;
}

mm_bool mm_mcxn947_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    mm_u32 word;
    mm_u32 bit;
    mm_u32 nblks;
    if (bank < 0 || bank >= 4) {
        return MM_FALSE;
    }
    nblks = trdc_mem_nblks(bank);
    if (block_index >= nblks) {
        return MM_FALSE;
    }
    word = block_index / 32u;
    bit = block_index % 32u;
    if (word >= 2u) {
        return MM_FALSE;
    }
    return ((trdc.nse_words[bank][word] >> bit) & 1u) == 0u;
}
