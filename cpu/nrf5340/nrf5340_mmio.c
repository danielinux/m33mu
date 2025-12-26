/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include <sys/random.h>
#include "nrf5340/nrf5340_mmio.h"
#include "nrf5340/nrf5340_wdt.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/gpio.h"

extern void mm_system_request_reset(void);

#define CLOCK_BASE_NS 0x40005000u
#define CLOCK_BASE_S  0x50005000u
#define CLOCK_SIZE    0x1000u

#define CLOCK_TASKS_HFCLKSTART 0x000u
#define CLOCK_TASKS_HFCLKSTOP  0x004u
#define CLOCK_EVENTS_HFCLKSTARTED 0x100u
#define CLOCK_HFCLKRUN 0x408u
#define CLOCK_HFCLKSTAT 0x40Cu

#define GPIO_P0_BASE_NS 0x40842500u
#define GPIO_P1_BASE_NS 0x40842800u
#define GPIO_P0_BASE_S  0x50842500u
#define GPIO_P1_BASE_S  0x50842800u
#define GPIO_SIZE       0x300u

#define GPIO_OUT     0x004u
#define GPIO_OUTSET  0x008u
#define GPIO_OUTCLR  0x00Cu
#define GPIO_IN      0x010u
#define GPIO_DIR     0x014u
#define GPIO_DIRSET  0x018u
#define GPIO_DIRCLR  0x01Cu
#define GPIO_LATCH   0x020u
#define GPIO_DETECTMODE 0x024u
#define GPIO_PIN_CNF0 0x200u

#define NVMC_BASE_NS 0x40039000u
#define NVMC_BASE_S  0x50039000u
#define NVMC_SIZE    0x1000u

#define NVMC_READY   0x400u
#define NVMC_READYNEXT 0x408u
#define NVMC_CONFIG  0x504u
#define NVMC_ERASEPAGE 0x508u
#define NVMC_ERASEALL 0x50Cu
#define NVMC_CONFIGNS 0x584u

#define RNG_BASE_NS 0x40845000u
#define RNG_BASE_S  0x50845000u
#define RNG_SIZE    0x1000u

#define RNG_TASKS_START 0x000u
#define RNG_TASKS_STOP  0x004u
#define RNG_EVENTS_VALRDY 0x100u
#define RNG_INTENSET 0x304u
#define RNG_INTENCLR 0x308u
#define RNG_CONFIG  0x504u
#define RNG_VALUE   0x508u

#define SPU_BASE_S  0x50003000u
#define SPU_SIZE    0x1000u

struct clock_state {
    mm_u32 regs[CLOCK_SIZE / 4];
    mm_bool hfclk_on;
};

struct gpio_bank {
    mm_u32 regs[GPIO_SIZE / 4];
    mm_u32 pin_cnf[32];
};

struct nvmc_state {
    mm_u32 regs[NVMC_SIZE / 4];
    mm_u8 *flash;
    mm_u32 flash_size;
    const struct mm_flash_persist *persist;
    mm_u32 flags;
    mm_u32 base_s;
    mm_u32 base_ns;
};

struct rng_state {
    mm_u32 regs[RNG_SIZE / 4];
    mm_u8 value;
    mm_bool running;
};

struct spu_state {
    mm_u32 regs[SPU_SIZE / 4];
};

static struct clock_state clock_state;
static struct gpio_bank gpio_banks[2];
static struct nvmc_state nvmc_state;
static struct rng_state rng_state;
static struct spu_state spu_state;

static mm_u32 read_slice(mm_u32 reg, mm_u32 offset_in_reg, mm_u32 size_bytes)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    return (reg >> shift) & mask;
}

static mm_u32 apply_write(mm_u32 cur, mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    mm_u32 shifted = (value & mask) << shift;
    return (cur & ~(mask << shift)) | shifted;
}

mm_bool mm_nrf5340_clock_hf_running(void)
{
    return clock_state.hfclk_on;
}

static mm_bool clock_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct clock_state *clk = (struct clock_state *)opaque;
    if (clk == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CLOCK_SIZE) return MM_FALSE;

    if (offset == CLOCK_HFCLKRUN && size_bytes == 4) {
        *value_out = clk->hfclk_on ? 1u : 0u;
        return MM_TRUE;
    }
    if (offset == CLOCK_HFCLKSTAT && size_bytes == 4) {
        *value_out = clk->hfclk_on ? 1u : 0u;
        return MM_TRUE;
    }
    *value_out = read_slice(clk->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool clock_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct clock_state *clk = (struct clock_state *)opaque;
    if (clk == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CLOCK_SIZE) return MM_FALSE;

    if (offset == CLOCK_TASKS_HFCLKSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->hfclk_on = MM_TRUE;
            clk->regs[CLOCK_EVENTS_HFCLKSTARTED / 4] = 1u;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_HFCLKSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->hfclk_on = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_EVENTS_HFCLKSTARTED && size_bytes == 4) {
        if (value == 0u) {
            clk->regs[CLOCK_EVENTS_HFCLKSTARTED / 4] = 0u;
        } else {
            clk->regs[CLOCK_EVENTS_HFCLKSTARTED / 4] = value;
        }
        return MM_TRUE;
    }

    clk->regs[offset / 4] = apply_write(clk->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static void gpio_sync_inputs(struct gpio_bank *g)
{
    if (g == 0) return;
    g->regs[GPIO_IN / 4] = g->regs[GPIO_OUT / 4];
}

static mm_bool gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct gpio_bank *g = (struct gpio_bank *)opaque;
    if (g == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GPIO_SIZE) return MM_FALSE;

    if (offset >= GPIO_PIN_CNF0 && size_bytes == 4) {
        mm_u32 pin = (offset - GPIO_PIN_CNF0) / 4u;
        if (pin < 32u) {
            *value_out = g->pin_cnf[pin];
            return MM_TRUE;
        }
    }
    if (offset == GPIO_IN && size_bytes == 4) {
        gpio_sync_inputs(g);
    }
    *value_out = read_slice(g->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct gpio_bank *g = (struct gpio_bank *)opaque;
    if (g == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GPIO_SIZE) return MM_FALSE;

    if (offset >= GPIO_PIN_CNF0 && size_bytes == 4) {
        mm_u32 pin = (offset - GPIO_PIN_CNF0) / 4u;
        if (pin < 32u) {
            g->pin_cnf[pin] = value;
            return MM_TRUE;
        }
    }

    if (offset == GPIO_OUT && size_bytes == 4) {
        g->regs[GPIO_OUT / 4] = value;
        gpio_sync_inputs(g);
        return MM_TRUE;
    }
    if (offset == GPIO_OUTSET && size_bytes == 4) {
        g->regs[GPIO_OUT / 4] |= value;
        gpio_sync_inputs(g);
        return MM_TRUE;
    }
    if (offset == GPIO_OUTCLR && size_bytes == 4) {
        g->regs[GPIO_OUT / 4] &= ~value;
        gpio_sync_inputs(g);
        return MM_TRUE;
    }
    if (offset == GPIO_DIR && size_bytes == 4) {
        g->regs[GPIO_DIR / 4] = value;
        return MM_TRUE;
    }
    if (offset == GPIO_DIRSET && size_bytes == 4) {
        g->regs[GPIO_DIR / 4] |= value;
        return MM_TRUE;
    }
    if (offset == GPIO_DIRCLR && size_bytes == 4) {
        g->regs[GPIO_DIR / 4] &= ~value;
        return MM_TRUE;
    }

    g->regs[offset / 4] = apply_write(g->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_u32 nrf_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= 2) return 0;
    return gpio_banks[bank].regs[GPIO_OUT / 4];
}

static mm_u32 nrf_gpio_bank_read_moder(void *opaque, int bank)
{
    mm_u32 moder = 0;
    mm_u32 dir;
    int pin;
    (void)opaque;
    if (bank < 0 || bank >= 2) return 0;
    dir = gpio_banks[bank].regs[GPIO_DIR / 4];
    for (pin = 0; pin < 16; ++pin) {
        mm_u32 mode = ((dir >> pin) & 1u) ? 1u : 0u;
        moder |= (mode << (pin * 2));
    }
    return moder;
}

static mm_bool nrf_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    (void)bank;
    return MM_TRUE;
}

static mm_u32 nrf_gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    (void)bank;
    return 0u;
}

static mm_bool nvmc_write_cb(void *opaque,
                             enum mm_sec_state sec,
                             mm_u32 addr,
                             mm_u32 size_bytes,
                             mm_u32 value)
{
    struct nvmc_state *nvmc = (struct nvmc_state *)opaque;
    mm_u32 wen;
    mm_u32 base;
    mm_u32 offset;
    mm_u8 *flash;
    if (nvmc == 0 || nvmc->flash == 0) return MM_FALSE;
    (void)sec;

    wen = nvmc->regs[NVMC_CONFIG / 4] & 0x7u;
    if (wen == 0u) {
        wen = nvmc->regs[NVMC_CONFIGNS / 4] & 0x3u;
    }
    if (wen != 0x1u) {
        return MM_FALSE;
    }

    base = (addr >= nvmc->base_s) ? nvmc->base_s : nvmc->base_ns;
    if (addr < base) return MM_FALSE;
    offset = addr - base;
    if (offset + size_bytes > nvmc->flash_size) return MM_FALSE;

    flash = nvmc->flash;
    if (size_bytes == 4u) {
        mm_u32 cur = (mm_u32)flash[offset] |
                     ((mm_u32)flash[offset + 1u] << 8) |
                     ((mm_u32)flash[offset + 2u] << 16) |
                     ((mm_u32)flash[offset + 3u] << 24);
        mm_u32 next = cur & value;
        flash[offset] = (mm_u8)(next & 0xffu);
        flash[offset + 1u] = (mm_u8)((next >> 8) & 0xffu);
        flash[offset + 2u] = (mm_u8)((next >> 16) & 0xffu);
        flash[offset + 3u] = (mm_u8)((next >> 24) & 0xffu);
    } else if (size_bytes == 2u) {
        mm_u32 cur = (mm_u32)flash[offset] | ((mm_u32)flash[offset + 1u] << 8);
        mm_u32 next = cur & value;
        flash[offset] = (mm_u8)(next & 0xffu);
        flash[offset + 1u] = (mm_u8)((next >> 8) & 0xffu);
    } else if (size_bytes == 1u) {
        flash[offset] &= (mm_u8)(value & 0xffu);
    } else {
        return MM_FALSE;
    }

    if (nvmc->persist != 0 && nvmc->persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)nvmc->persist, addr, size_bytes);
    }
    return MM_TRUE;
}

static void nvmc_erase_all(struct nvmc_state *nvmc)
{
    if (nvmc == 0 || nvmc->flash == 0) return;
    memset(nvmc->flash, 0xFF, nvmc->flash_size);
    if (nvmc->persist != 0 && nvmc->persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)nvmc->persist,
                               nvmc->base_ns,
                               nvmc->flash_size);
    }
}

static void nvmc_erase_page(struct nvmc_state *nvmc, mm_u32 addr)
{
    mm_u32 base;
    mm_u32 offset;
    mm_u32 page_base;
    mm_u32 page_size = 0x1000u;
    if (nvmc == 0 || nvmc->flash == 0) return;
    base = (addr >= nvmc->base_s) ? nvmc->base_s : nvmc->base_ns;
    if (addr < base) return;
    offset = addr - base;
    if (offset >= nvmc->flash_size) return;
    page_base = (offset / page_size) * page_size;
    if (page_base + page_size > nvmc->flash_size) return;
    memset(nvmc->flash + page_base, 0xFF, page_size);
    if (nvmc->persist != 0 && nvmc->persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)nvmc->persist,
                               base + page_base,
                               page_size);
    }
}

static mm_bool nvmc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct nvmc_state *nvmc = (struct nvmc_state *)opaque;
    if (nvmc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > NVMC_SIZE) return MM_FALSE;
    if (offset == NVMC_READY && size_bytes == 4) {
        *value_out = 1u;
        return MM_TRUE;
    }
    if (offset == NVMC_READYNEXT && size_bytes == 4) {
        *value_out = 1u;
        return MM_TRUE;
    }
    *value_out = read_slice(nvmc->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool nvmc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct nvmc_state *nvmc = (struct nvmc_state *)opaque;
    mm_u32 wen;
    if (nvmc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > NVMC_SIZE) return MM_FALSE;

    wen = nvmc->regs[NVMC_CONFIG / 4] & 0x7u;
    if (wen == 0u) {
        wen = nvmc->regs[NVMC_CONFIGNS / 4] & 0x3u;
    }

    if (offset == NVMC_ERASEALL && size_bytes == 4) {
        if (wen == 0x2u && (value & 1u) != 0u) {
            nvmc_erase_all(nvmc);
        }
        return MM_TRUE;
    }

    if (offset == NVMC_ERASEPAGE && size_bytes == 4) {
        if (wen == 0x2u) {
            nvmc_erase_page(nvmc, value);
        }
        return MM_TRUE;
    }

    nvmc->regs[offset / 4] = apply_write(nvmc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool rng_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rng_state *rng = (struct rng_state *)opaque;
    if (rng == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RNG_SIZE) return MM_FALSE;

    if (offset == RNG_VALUE && size_bytes == 4) {
        if (!rng->running) {
            *value_out = (mm_u32)rng->value;
            return MM_TRUE;
        }
        (void)getrandom(&rng->value, sizeof(rng->value), 0);
        rng->regs[RNG_EVENTS_VALRDY / 4] = 1u;
        *value_out = (mm_u32)rng->value;
        return MM_TRUE;
    }

    *value_out = read_slice(rng->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool rng_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rng_state *rng = (struct rng_state *)opaque;
    if (rng == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RNG_SIZE) return MM_FALSE;

    if (offset == RNG_TASKS_START && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            rng->running = MM_TRUE;
            (void)getrandom(&rng->value, sizeof(rng->value), 0);
            rng->regs[RNG_EVENTS_VALRDY / 4] = 1u;
        }
        return MM_TRUE;
    }
    if (offset == RNG_TASKS_STOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            rng->running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == RNG_EVENTS_VALRDY && size_bytes == 4) {
        if (value == 0u) {
            rng->regs[RNG_EVENTS_VALRDY / 4] = 0u;
        } else {
            rng->regs[RNG_EVENTS_VALRDY / 4] = value;
        }
        return MM_TRUE;
    }
    if (offset == RNG_INTENSET && size_bytes == 4) {
        rng->regs[RNG_INTENSET / 4] |= value;
        return MM_TRUE;
    }
    if (offset == RNG_INTENCLR && size_bytes == 4) {
        rng->regs[RNG_INTENSET / 4] &= ~value;
        return MM_TRUE;
    }

    rng->regs[offset / 4] = apply_write(rng->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool spu_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct spu_state *spu = (struct spu_state *)opaque;
    if (spu == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SPU_SIZE) return MM_FALSE;
    *value_out = read_slice(spu->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool spu_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct spu_state *spu = (struct spu_state *)opaque;
    if (spu == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SPU_SIZE) return MM_FALSE;
    spu->regs[offset / 4] = apply_write(spu->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

mm_bool mm_nrf5340_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;

    memset(&clock_state, 0, sizeof(clock_state));
    clock_state.hfclk_on = MM_TRUE;

    memset(&gpio_banks, 0, sizeof(gpio_banks));
    memset(&nvmc_state, 0, sizeof(nvmc_state));
    memset(&rng_state, 0, sizeof(rng_state));
    memset(&spu_state, 0, sizeof(spu_state));

    reg.base = CLOCK_BASE_NS;
    reg.size = CLOCK_SIZE;
    reg.opaque = &clock_state;
    reg.read = clock_read;
    reg.write = clock_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = CLOCK_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO_P0_BASE_NS;
    reg.size = GPIO_SIZE;
    reg.opaque = &gpio_banks[0];
    reg.read = gpio_read;
    reg.write = gpio_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO_P0_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = GPIO_P1_BASE_NS;
    reg.opaque = &gpio_banks[1];
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO_P1_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = NVMC_BASE_NS;
    reg.size = NVMC_SIZE;
    reg.opaque = &nvmc_state;
    reg.read = nvmc_read;
    reg.write = nvmc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = NVMC_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = RNG_BASE_NS;
    reg.size = RNG_SIZE;
    reg.opaque = &rng_state;
    reg.read = rng_read;
    reg.write = rng_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = RNG_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = SPU_BASE_S;
    reg.size = SPU_SIZE;
    reg.opaque = &spu_state;
    reg.read = spu_read;
    reg.write = spu_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    if (!mm_nrf5340_wdt_register(bus)) return MM_FALSE;

    mm_gpio_bank_set_reader(nrf_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(nrf_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(nrf_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(nrf_gpio_bank_read_seccfgr, 0);

    return MM_TRUE;
}

void mm_nrf5340_flash_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist,
                           mm_u32 flags)
{
    if (map == 0) return;
    nvmc_state.flash = flash;
    nvmc_state.flash_size = flash_size;
    nvmc_state.persist = persist;
    nvmc_state.flags = flags;
    nvmc_state.base_s = map->flash_base_s;
    nvmc_state.base_ns = map->flash_base_ns;
    mm_memmap_set_flash_writer(map, nvmc_write_cb, &nvmc_state);
}

mm_u64 mm_nrf5340_cpu_hz(void)
{
    return 128000000ull;
}

void mm_nrf5340_mmio_reset(void)
{
    memset(&clock_state, 0, sizeof(clock_state));
    clock_state.hfclk_on = MM_TRUE;
    clock_state.regs[CLOCK_EVENTS_HFCLKSTARTED / 4] = 0u;

    memset(&gpio_banks, 0, sizeof(gpio_banks));
    memset(&rng_state, 0, sizeof(rng_state));
    memset(&spu_state, 0, sizeof(spu_state));

    memset(nvmc_state.regs, 0, sizeof(nvmc_state.regs));
    nvmc_state.regs[NVMC_READY / 4] = 1u;
    nvmc_state.regs[NVMC_READYNEXT / 4] = 1u;
    nvmc_state.regs[NVMC_CONFIG / 4] = 0u;

    mm_nrf5340_wdt_reset();
}
