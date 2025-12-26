/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include "nrf5340/nrf5340_wdt.h"
#include "nrf5340/nrf5340_mmio.h"

extern void mm_system_request_reset(void);

#define WDT0_BASE_NS 0x40018000u
#define WDT1_BASE_NS 0x40019000u
#define WDT0_BASE_S  0x50018000u
#define WDT1_BASE_S  0x50019000u
#define WDT_SIZE     0x1000u

#define WDT_TASKS_START 0x000u
#define WDT_TASKS_STOP  0x004u
#define WDT_EVENTS_TIMEOUT 0x100u
#define WDT_INTENSET 0x304u
#define WDT_INTENCLR 0x308u
#define WDT_RUNSTATUS 0x400u
#define WDT_REQSTATUS 0x404u
#define WDT_CRV     0x504u
#define WDT_RREN    0x508u
#define WDT_CONFIG  0x50Cu
#define WDT_RR0     0x600u

#define WDT_INT_TIMEOUT (1u << 0)

struct wdt_state {
    mm_u32 regs[WDT_SIZE / 4];
    mm_u32 crv;
    mm_u32 counter;
    mm_u64 accum;
    mm_bool running;
    int irq;
};

static struct wdt_state wdts[2];
static struct mm_nvic *g_nvic = 0;

static mm_u64 wdt_cycles_per_tick(void)
{
    mm_u64 hz = mm_nrf5340_cpu_hz();
    if (hz == 0u) return 1u;
    return hz / 32768u;
}

static void wdt_raise_irq(struct wdt_state *w)
{
    if (w == 0 || g_nvic == 0) return;
    if ((w->regs[WDT_INTENSET / 4] & WDT_INT_TIMEOUT) == 0u) return;
    mm_nvic_set_pending(g_nvic, (mm_u32)w->irq, MM_TRUE);
}

static mm_bool wdt_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct wdt_state *w = (struct wdt_state *)opaque;
    if (w == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > WDT_SIZE) return MM_FALSE;

    if (offset == WDT_RUNSTATUS && size_bytes == 4) {
        *value_out = w->running ? 1u : 0u;
        return MM_TRUE;
    }
    if (offset == WDT_REQSTATUS && size_bytes == 4) {
        *value_out = w->running ? (w->regs[WDT_RREN / 4] & 0xFFu) : 0u;
        return MM_TRUE;
    }
    if (offset == WDT_CRV && size_bytes == 4) {
        *value_out = w->crv;
        return MM_TRUE;
    }
    *value_out = w->regs[offset / 4];
    return MM_TRUE;
}

static void wdt_reload(struct wdt_state *w)
{
    if (w == 0) return;
    w->counter = w->crv;
    w->regs[WDT_EVENTS_TIMEOUT / 4] = 0u;
}

static mm_bool wdt_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct wdt_state *w = (struct wdt_state *)opaque;
    if (w == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > WDT_SIZE) return MM_FALSE;

    if (offset == WDT_TASKS_START && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            w->running = MM_TRUE;
            wdt_reload(w);
        }
        return MM_TRUE;
    }
    if (offset == WDT_TASKS_STOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            w->running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == WDT_CRV && size_bytes == 4) {
        w->crv = value;
        return MM_TRUE;
    }
    if (offset == WDT_RREN && size_bytes == 4) {
        w->regs[WDT_RREN / 4] = value & 0xFFu;
        return MM_TRUE;
    }
    if (offset == WDT_INTENSET && size_bytes == 4) {
        w->regs[WDT_INTENSET / 4] |= value;
        return MM_TRUE;
    }
    if (offset == WDT_INTENCLR && size_bytes == 4) {
        w->regs[WDT_INTENSET / 4] &= ~value;
        return MM_TRUE;
    }
    if (offset == WDT_EVENTS_TIMEOUT && size_bytes == 4) {
        if (value == 0u) {
            w->regs[WDT_EVENTS_TIMEOUT / 4] = 0u;
        } else {
            w->regs[WDT_EVENTS_TIMEOUT / 4] = value;
        }
        return MM_TRUE;
    }
    if (offset == WDT_RR0 && size_bytes == 4) {
        wdt_reload(w);
        return MM_TRUE;
    }

    w->regs[offset / 4] = value;
    return MM_TRUE;
}

mm_bool mm_nrf5340_wdt_register(struct mmio_bus *bus)
{
    struct mmio_region reg;
    size_t i;
    mm_u32 bases_ns[2] = { WDT0_BASE_NS, WDT1_BASE_NS };
    mm_u32 bases_s[2] = { WDT0_BASE_S, WDT1_BASE_S };
    int irqs[2] = { 24, 25 };

    if (bus == 0) return MM_FALSE;

    memset(&wdts, 0, sizeof(wdts));
    for (i = 0; i < 2; ++i) {
        wdts[i].irq = irqs[i];
        wdts[i].crv = 0xFFFFFFFFu;
        wdts[i].regs[WDT_RREN / 4] = 1u;
    }

    reg.size = WDT_SIZE;
    reg.read = wdt_read;
    reg.write = wdt_write;

    for (i = 0; i < 2; ++i) {
        reg.base = bases_ns[i];
        reg.opaque = &wdts[i];
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
        reg.base = bases_s[i];
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    }

    return MM_TRUE;
}

void mm_nrf5340_wdt_reset(void)
{
    size_t i;
    for (i = 0; i < 2; ++i) {
        int irq = wdts[i].irq;
        memset(&wdts[i], 0, sizeof(wdts[i]));
        wdts[i].irq = irq;
        wdts[i].crv = 0xFFFFFFFFu;
        wdts[i].regs[WDT_RREN / 4] = 1u;
    }
}

void mm_nrf5340_wdt_set_nvic(struct mm_nvic *nvic)
{
    g_nvic = nvic;
}

void mm_nrf5340_wdt_tick(mm_u64 cycles)
{
    mm_u64 div = wdt_cycles_per_tick();
    size_t i;
    if (div == 0u) div = 1u;

    for (i = 0; i < 2; ++i) {
        struct wdt_state *w = &wdts[i];
        mm_u64 ticks;
        if (!w->running) continue;
        w->accum += cycles;
        ticks = w->accum / div;
        w->accum = w->accum % div;
        if (ticks == 0u) continue;
        if (w->counter <= ticks) {
            w->counter = 0u;
            w->regs[WDT_EVENTS_TIMEOUT / 4] = 1u;
            wdt_raise_irq(w);
            mm_system_request_reset();
        } else {
            w->counter -= (mm_u32)ticks;
        }
    }
}
