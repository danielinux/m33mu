/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "mcxw71c/mcxw71c_mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/gpio_pads.h"
#include "m33mu/gpio.h"
#include "m33mu/nvic.h"

#define MRCC_BASE 0x4001C000u
#define MRCC_SEC_BASE (MRCC_BASE + 0x10000000u)
#define MRCC_SIZE 0x800u

#define WDOG_BASE 0x4001A000u
#define WDOG_SEC_BASE 0x5001A000u
#define WDOG_SIZE 0x1000u

#define SCG_BASE 0x4001E000u
#define SCG_SEC_BASE 0x5001E000u
#define SCG_SIZE 0x804u

#define CCM32K_BASE 0x4001F000u
#define CCM32K_SEC_BASE 0x5001F000u
#define CCM32K_SIZE 0x28u

#define VBAT_BASE 0x4002B000u
#define VBAT_SEC_BASE 0x5002B000u
#define VBAT_SIZE 0x340u

#define SCG_VERID 0x000u
#define SCG_PARAM 0x004u
#define SCG_CSR 0x010u
#define SCG_RCCR 0x014u
#define SCG_SOSCCSR 0x100u
#define SCG_SIRCCSR 0x200u
#define SCG_FIRCCSR 0x300u
#define SCG_ROSCCSR 0x400u

#define CCM32K_FRO32K_CTRL 0x000u
#define CCM32K_OSC32K_CTRL 0x008u
#define CCM32K_STATUS 0x00cu
#define CCM32K_CGC32K 0x01cu

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

struct wdog_state {
    mm_u32 regs[WDOG_SIZE / 4];
};

struct scg_state {
    mm_u32 regs[SCG_SIZE / 4];
};

struct ccm32k_state {
    mm_u32 regs[CCM32K_SIZE / 4];
};

struct vbat_state {
    mm_u32 regs[VBAT_SIZE / 4];
};

struct port_state {
    mm_u32 regs[PORT_SIZE / 4];
    mm_u32 pcr[32];
    mm_u32 last_pdir;
};

static struct gpio_bank gpio_banks[4];
static struct mrcc_state mrcc;
static struct wdog_state wdog;
static struct scg_state scg;
static struct ccm32k_state ccm32k;
static struct vbat_state vbat;
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

/* Pads: PORTA-C PCR PE (bit 1) and PS (bit 0, 1 = pull-up) give the pulls;
 * GPIOD has no PORT here, so no pull information. */
static struct mm_gpio_pads gpio_pads[4];
static int gpio_pad_bank_ids[4] = { 0, 1, 2, 3 };

static mm_u32 port_pull_mask(int bank, mm_bool up)
{
    mm_u32 mask = 0u;
    int pin;
    int port = port_index_for_bank(bank);
    if (port < 0) return 0u;
    for (pin = 0; pin < 32; ++pin) {
        mm_u32 pcr = ports[port].pcr[pin];
        if ((pcr & 2u) != 0u && ((pcr & 1u) != 0u) == up) mask |= 1u << pin;
    }
    return mask;
}

static void gpio_pad_config(void *opaque, int pin, struct mm_gpio_pad_config *cfg)
{
    int port = port_index_for_bank(*(const int *)opaque);
    mm_u32 pcr;
    if (port < 0) return;
    pcr = ports[port].pcr[pin];
    cfg->pull = (pcr & 2u) == 0u ? MM_GPIO_PULL_NONE : (pcr & 1u) ? MM_GPIO_PULL_UP : MM_GPIO_PULL_DOWN;
}

static void gpio_sync_pdir(struct gpio_bank *g)
{
    int bank;
    if (g == 0) return;
    bank = (int)(g - gpio_banks);
    g->regs[GPIO_PDIR / 4] = mm_gpio_pads_input(&gpio_pads[bank], g->regs[GPIO_PDDR / 4],
                                                g->regs[GPIO_PDOR / 4], port_pull_mask(bank, MM_TRUE),
                                                port_pull_mask(bank, MM_FALSE), g->regs[GPIO_PDOR / 4]);
    mm_gpio_pads_publish(&gpio_pads[bank], g->regs[GPIO_PDDR / 4], g->regs[GPIO_PDOR / 4],
                         gpio_pad_config, &gpio_pad_bank_ids[bank]);
}

static mm_bool gpio_set_external_input_cb(void *opaque, int bank, int pin, mm_bool level,
                                          mm_u64 late_cycles)
{
    mm_u32 old_pdir;
    (void)opaque;
    (void)late_cycles;
    if (bank < 0 || bank >= 4 || !mm_gpio_pads_set_external(&gpio_pads[bank], pin, level)) return MM_FALSE;
    old_pdir = gpio_banks[bank].regs[GPIO_PDIR / 4];
    gpio_sync_pdir(&gpio_banks[bank]);
    gpio_update_edges(bank, old_pdir, gpio_banks[bank].regs[GPIO_PDIR / 4]);
    return MM_TRUE;
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
        gpio_sync_pdir(g);
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
    int port = (int)(p - ports);
    if (pin < 0 || pin >= 32) return;
    p->pcr[pin] = value;
    if (port >= 0 && port < 3) gpio_sync_pdir(&gpio_banks[port]);
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

static mm_bool mcxw71c_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    static const int pin_counts[] = { 9, 6, 8, 6 };
    (void)opaque;
    if (bank < 0 || bank >= 4) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0u) {
        name_out[0] = (char)('A' + bank);
        if (name_len > 1u) {
            name_out[1] = '\0';
        }
    }
    if (pins_out != 0) {
        *pins_out = pin_counts[bank];
    }
    return MM_TRUE;
}

struct mrcc_clk_name {
    const char *name;
    mm_u32 offset;
};

static const struct mrcc_clk_name mrcc_clk_names[] = {
    { "LPIT0", MCXW71C_MRCC_LPIT0 },
    { "LPSPI0", MCXW71C_MRCC_LPSPI0 },
    { "LPSPI1", MCXW71C_MRCC_LPSPI1 },
    { "LPUART0", MCXW71C_MRCC_LPUART0 },
    { "LPUART1", MCXW71C_MRCC_LPUART1 },
    { "PORTA", MCXW71C_MRCC_PORTA },
    { "PORTB", MCXW71C_MRCC_PORTB },
    { "PORTC", MCXW71C_MRCC_PORTC },
    { "GPIOA", MCXW71C_MRCC_GPIOA },
    { "GPIOB", MCXW71C_MRCC_GPIOB },
    { "GPIOC", MCXW71C_MRCC_GPIOC }
};

static mm_bool mcxw71c_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len)
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
    pos += snprintf(buf + pos, sizeof(buf) - pos, "MRCC:");
    for (i = 0; i < sizeof(mrcc_clk_names) / sizeof(mrcc_clk_names[0]); ++i) {
        if (!mm_mcxw71c_mrcc_clock_on(mrcc_clk_names[i].offset)) {
            continue;
        }
        if (pos + 2u < sizeof(buf)) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", mrcc_clk_names[i].name);
            have = MM_TRUE;
        }
    }
    if (!have) {
        return MM_FALSE;
    }
    snprintf(out, out_len, "%s", buf);
    return MM_TRUE;
}

void mm_mcxw71c_mmio_reset(void)
{
    memset(gpio_banks, 0, sizeof(gpio_banks));
    {
        int b;
        for (b = 0; b < 4; ++b) mm_gpio_pads_init(&gpio_pads[b], b, 32);
    }
    memset(&mrcc, 0, sizeof(mrcc));
    memset(&wdog, 0, sizeof(wdog));
    memset(&scg, 0, sizeof(scg));
    memset(&ccm32k, 0, sizeof(ccm32k));
    memset(&vbat, 0, sizeof(vbat));
    memset(ports, 0, sizeof(ports));
    g_gpio_nvic = 0;
    mrcc.regs[MCXW71C_MRCC_LPIT0 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_LPSPI0 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_LPSPI1 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_LPUART0 / 4] = 0x80000000u;
    mrcc.regs[MCXW71C_MRCC_LPUART1 / 4] = 0x80000000u;
    /* PORT/GPIO blocks are reachable during early pinctrl on Zephyr. */
    mrcc.regs[MCXW71C_MRCC_PORTA / 4] = 0xC0000001u;
    mrcc.regs[MCXW71C_MRCC_PORTB / 4] = 0xC0000001u;
    mrcc.regs[MCXW71C_MRCC_PORTC / 4] = 0xC0000001u;
    mrcc.regs[MCXW71C_MRCC_GPIOA / 4] = 0xC0000001u;
    mrcc.regs[MCXW71C_MRCC_GPIOB / 4] = 0xC0000001u;
    mrcc.regs[MCXW71C_MRCC_GPIOC / 4] = 0xC0000001u;
    /* SystemInit() waits for RCS, ULK and CMD32EN in WDOG0->CS. */
    wdog.regs[0x0u / 4] = (1u << 10) | (1u << 11) | (1u << 13);
    scg.regs[SCG_VERID / 4u] = 0x00000000u;
    scg.regs[SCG_PARAM / 4u] = 0x000001FEu;
    scg.regs[SCG_CSR / 4u] = 0x03000000u;
    scg.regs[SCG_RCCR / 4u] = 0x03000000u;
    scg.regs[SCG_SOSCCSR / 4u] = 0x01000020u;
    scg.regs[SCG_SIRCCSR / 4u] = 0x01000020u;
    scg.regs[SCG_FIRCCSR / 4u] = 0x03000031u;
    scg.regs[SCG_ROSCCSR / 4u] = 0x03000000u;
    ccm32k.regs[CCM32K_FRO32K_CTRL / 4u] = 0x00000001u;
    ccm32k.regs[CCM32K_STATUS / 4u] = 0x00000001u;
    ccm32k.regs[CCM32K_CGC32K / 4u] = 0x00000020u;
    vbat.regs[0x10u / 4u] = 0x00000011u;
    mm_gpio_bank_set_reader(mcxw71c_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(mcxw71c_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(mcxw71c_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(mcxw71c_gpio_bank_read_seccfgr, 0);
    mm_gpio_set_bank_info_reader(mcxw71c_gpio_bank_info, 0);
    mm_gpio_set_external_input_writer(gpio_set_external_input_cb, 0);
    mm_rcc_set_clock_list_reader(mcxw71c_rcc_clock_list_line, 0);
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

static mm_bool wdog_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct wdog_state *w = (struct wdog_state *)opaque;
    if (w == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > WDOG_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)w->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool wdog_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct wdog_state *w = (struct wdog_state *)opaque;
    if (w == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > WDOG_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)w->regs + offset, &value, size_bytes);
    /* Keep SystemInit()-visible ready bits asserted in CS. */
    if (offset == 0x0u && size_bytes == 4u) {
        w->regs[0x0u / 4] |= (1u << 10) | (1u << 11) | (1u << 13);
    }
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
    } else if (offset == SCG_SOSCCSR || offset == SCG_SIRCCSR || offset == SCG_FIRCCSR) {
        s->regs[offset / 4u] |= (1u << 24) | (1u << 5);
    } else if (offset == SCG_ROSCCSR) {
        s->regs[offset / 4u] |= (1u << 24) | (1u << 25);
    }
    return MM_TRUE;
}

static mm_bool ccm32k_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct ccm32k_state *c = (struct ccm32k_state *)opaque;
    if (c == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CCM32K_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)c->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool ccm32k_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct ccm32k_state *c = (struct ccm32k_state *)opaque;
    if (c == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CCM32K_SIZE) return MM_FALSE;
    if (offset == CCM32K_STATUS) {
        return MM_TRUE;
    }
    memcpy((mm_u8 *)c->regs + offset, &value, size_bytes);
    if (offset == CCM32K_FRO32K_CTRL || offset == CCM32K_OSC32K_CTRL || offset == CCM32K_CGC32K) {
        c->regs[CCM32K_STATUS / 4u] |= 1u;
        scg.regs[SCG_ROSCCSR / 4u] |= (1u << 24) | (1u << 25);
    }
    return MM_TRUE;
}

static mm_bool vbat_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct vbat_state *v = (struct vbat_state *)opaque;
    if (v == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > VBAT_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)v->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool vbat_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct vbat_state *v = (struct vbat_state *)opaque;
    if (v == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > VBAT_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)v->regs + offset, &value, size_bytes);
    /* Keep LDO ready asserted and allow STATUSA flag clearing writes. */
    if (offset == 0x10u && size_bytes == 4u) {
        v->regs[0x10u / 4u] = (v->regs[0x10u / 4u] & ~value) | 0x10u;
    }
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

    reg.base = WDOG_BASE;
    reg.size = WDOG_SIZE;
    reg.opaque = &wdog;
    reg.read = wdog_read;
    reg.write = wdog_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = WDOG_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = SCG_BASE;
    reg.size = SCG_SIZE;
    reg.opaque = &scg;
    reg.read = scg_read;
    reg.write = scg_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = SCG_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = CCM32K_BASE;
    reg.size = CCM32K_SIZE;
    reg.opaque = &ccm32k;
    reg.read = ccm32k_read;
    reg.write = ccm32k_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = CCM32K_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = VBAT_BASE;
    reg.size = VBAT_SIZE;
    reg.opaque = &vbat;
    reg.read = vbat_read;
    reg.write = vbat_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = VBAT_SEC_BASE;
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
