/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rp2350/rp2350_timers.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "rp2350/cpu_config.h"
#include "rp2350/rp2350_mmio.h"
#include "rp2350/rp2350_pio.h"

#define TIMER0_BASE 0x400b0000u
#define TIMER1_BASE 0x400b8000u
#define TIMER_SIZE  0x4000u
#define TIMER_ALIAS_STRIDE 0x1000u

#define TIMER_TIMEHW 0x000u
#define TIMER_TIMELW 0x004u
#define TIMER_TIMEHR 0x008u
#define TIMER_TIMELR 0x00cu
#define TIMER_ALARM0 0x010u
#define TIMER_ALARM1 0x014u
#define TIMER_ALARM2 0x018u
#define TIMER_ALARM3 0x01cu
#define TIMER_ARMED  0x020u
#define TIMER_TIMERAWH 0x024u
#define TIMER_TIMERAWL 0x028u
#define TIMER_DBGPAUSE 0x02cu
#define TIMER_PAUSE 0x030u
#define TIMER_LOCKED 0x034u
#define TIMER_SOURCE 0x038u
#define TIMER_INTR   0x03cu
#define TIMER_INTE   0x040u
#define TIMER_INTF   0x044u
#define TIMER_INTS   0x048u

struct rp2350_timer {
    mm_u64 time_us;
    mm_u32 timelw_shadow;
    mm_u64 read_latch;
    mm_bool read_latch_valid;
    mm_u32 alarm[4];
    mm_u32 armed;
    mm_u32 intr;
    mm_u32 inte;
    mm_u32 intf;
    mm_u32 dbgpause;
    mm_u32 pause;
    mm_u32 locked;
    mm_u32 source;
    mm_u32 irq_base;
    mm_u32 last_low;
};

static struct rp2350_timer g_timers[2];
static struct mm_nvic *g_nvic = 0;
static mm_u64 g_cycle_accum = 0;
static mm_bool g_timer_trace = MM_FALSE;

static mm_u32 apply_write(mm_u32 cur, mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xffffffffu : ((1u << (size_bytes * 8u)) - 1u);
    mm_u32 shifted = (value & mask) << shift;
    return (cur & ~(mask << shift)) | shifted;
}

static mm_u32 alias_value(mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    return apply_write(0u, offset_in_reg, size_bytes, value);
}

static mm_u32 alias_base_offset(mm_u32 offset, mm_u32 *alias_out)
{
    mm_u32 alias = 0u;
    mm_u32 base = offset;
    if (offset >= TIMER_ALIAS_STRIDE && offset < TIMER_SIZE) {
        alias = (offset >> 12) & 0x3u;
        base = offset & 0xfffu;
    }
    if (alias_out != 0) {
        *alias_out = alias;
    }
    return base;
}

static void timer_raise_irq(struct rp2350_timer *t, mm_u32 alarm)
{
    if (g_nvic == 0) return;
    if (g_timer_trace) {
        printf("[TIMER%u_IRQ] alarm=%lu pending=1\n",
               (unsigned)(t->irq_base / 4u),
               (unsigned long)alarm);
    }
    if ((t->inte & (1u << alarm)) == 0u) return;
    mm_nvic_set_pending(g_nvic, t->irq_base + alarm, MM_TRUE);
}

static mm_bool timer_alarm_due(mm_u32 prev, mm_u32 now, mm_u32 target)
{
    if (prev == now) {
        return (now == target) ? MM_TRUE : MM_FALSE;
    }
    if (prev < now) {
        return (target > prev && target <= now) ? MM_TRUE : MM_FALSE;
    }
    /* wrap */
    return (target > prev || target <= now) ? MM_TRUE : MM_FALSE;
}

static void timer_check_alarms(struct rp2350_timer *t)
{
    mm_u32 low = (mm_u32)(t->time_us & 0xffffffffu);
    mm_u32 prev = t->last_low;
    mm_u32 i;
    t->last_low = low;
    for (i = 0; i < 4u; ++i) {
        mm_u32 mask = 1u << i;
        if ((t->armed & mask) == 0u) continue;
        if (!timer_alarm_due(prev, low, t->alarm[i])) continue;
        t->armed &= ~mask;
        t->intr |= mask;
        if (g_timer_trace) {
            printf("[TIMER%u_ALARM] alarm=%lu fired low=0x%08lx target=0x%08lx\n",
                   (unsigned)(t->irq_base / 4u),
                   (unsigned long)i,
                   (unsigned long)low,
                   (unsigned long)t->alarm[i]);
        }
        timer_raise_irq(t, i);
    }
}

static mm_bool timer_locked(const struct rp2350_timer *t)
{
    return (t->locked & 1u) ? MM_TRUE : MM_FALSE;
}

static mm_bool timer_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rp2350_timer *t = (struct rp2350_timer *)opaque;
    mm_u64 time;
    mm_u32 base_off;
    mm_u32 alias;
    if (t == 0 || value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > TIMER_SIZE) return MM_FALSE;
    if (size_bytes != 4u) return MM_FALSE;

    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > TIMER_ALIAS_STRIDE) return MM_FALSE;

    switch (base_off) {
    case TIMER_TIMEHR:
        time = t->read_latch_valid ? t->read_latch : t->time_us;
        t->read_latch_valid = MM_FALSE;
        *value_out = (mm_u32)(time >> 32);
        return MM_TRUE;
    case TIMER_TIMELR:
        t->read_latch = t->time_us;
        t->read_latch_valid = MM_TRUE;
        *value_out = (mm_u32)(t->read_latch & 0xffffffffu);
        return MM_TRUE;
    case TIMER_TIMERAWH:
        *value_out = (mm_u32)(t->time_us >> 32);
        return MM_TRUE;
    case TIMER_TIMERAWL:
        *value_out = (mm_u32)(t->time_us & 0xffffffffu);
        return MM_TRUE;
    case TIMER_ARMED:
        *value_out = t->armed & 0x0fu;
        return MM_TRUE;
    case TIMER_INTR:
        *value_out = t->intr & 0x0fu;
        return MM_TRUE;
    case TIMER_INTE:
        *value_out = t->inte & 0x0fu;
        return MM_TRUE;
    case TIMER_INTF:
        *value_out = t->intf & 0x0fu;
        return MM_TRUE;
    case TIMER_INTS:
        *value_out = ((t->intr | t->intf) & t->inte) & 0x0fu;
        return MM_TRUE;
    case TIMER_DBGPAUSE:
        *value_out = t->dbgpause & 0x7u;
        return MM_TRUE;
    case TIMER_PAUSE:
        *value_out = t->pause & 0x1u;
        return MM_TRUE;
    case TIMER_LOCKED:
        *value_out = t->locked & 0x1u;
        return MM_TRUE;
    case TIMER_SOURCE:
        *value_out = t->source & 0x1u;
        return MM_TRUE;
    case TIMER_ALARM0:
    case TIMER_ALARM1:
    case TIMER_ALARM2:
    case TIMER_ALARM3: {
        mm_u32 idx = (base_off - TIMER_ALARM0) / 4u;
        *value_out = t->alarm[idx];
        return MM_TRUE;
    }
    default:
        break;
    }
    return MM_TRUE;
}

static mm_bool timer_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rp2350_timer *t = (struct rp2350_timer *)opaque;
    mm_u32 mask;
    mm_u32 base_off;
    mm_u32 alias;
    if (t == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > TIMER_SIZE) return MM_FALSE;
    if (size_bytes != 4u) return MM_FALSE;
    base_off = alias_base_offset(offset, &alias);
    if ((base_off + size_bytes) > TIMER_ALIAS_STRIDE) return MM_FALSE;
    if (timer_locked(t) && base_off != TIMER_LOCKED) return MM_TRUE;

    if (alias != 0u) {
        mask = alias_value(base_off & 3u, size_bytes, value);
        switch (base_off) {
        case TIMER_ARMED:
            switch (alias) {
            case 1u: t->armed ^= mask; break;
            case 2u: t->armed |= mask; break;
            case 3u: t->armed &= ~mask; break;
            default: break;
            }
            if (g_timer_trace) {
                printf("[TIMER%u_ARMED_ALIAS] alias=%lu mask=0x%08lx armed=0x%08lx\n",
                       (unsigned)(t->irq_base / 4u),
                       (unsigned long)alias,
                       (unsigned long)mask,
                       (unsigned long)t->armed);
            }
            return MM_TRUE;
        case TIMER_INTR:
            switch (alias) {
            case 1u: t->intr ^= mask; break;
            case 2u: t->intr |= mask; break;
            case 3u: t->intr &= ~mask; break;
            default: break;
            }
            if (g_timer_trace) {
                printf("[TIMER%u_INTR_ALIAS] alias=%lu mask=0x%08lx intr=0x%08lx\n",
                       (unsigned)(t->irq_base / 4u),
                       (unsigned long)alias,
                       (unsigned long)mask,
                       (unsigned long)t->intr);
            }
            return MM_TRUE;
        case TIMER_INTE:
            switch (alias) {
            case 1u: t->inte ^= mask; break;
            case 2u: t->inte |= mask; break;
            case 3u: t->inte &= ~mask; break;
            default: break;
            }
            if (g_timer_trace) {
                printf("[TIMER%u_INTE_ALIAS] alias=%lu mask=0x%08lx inte=0x%08lx\n",
                       (unsigned)(t->irq_base / 4u),
                       (unsigned long)alias,
                       (unsigned long)mask,
                       (unsigned long)t->inte);
            }
            return MM_TRUE;
        case TIMER_INTF:
            switch (alias) {
            case 1u: t->intf ^= mask; break;
            case 2u: t->intf |= mask; break;
            case 3u: t->intf &= ~mask; break;
            default: break;
            }
            if (alias != 3u) {
                t->intr |= (t->intf & mask);
                for (mm_u32 bit = 0u; bit < 4u; ++bit) {
                    mm_u32 bitmask = 1u << bit;
                    if ((t->intf & bitmask) != 0u && (mask & bitmask) != 0u) {
                        timer_raise_irq(t, bit);
                    }
                }
            }
            if (g_timer_trace) {
                printf("[TIMER%u_INTF_ALIAS] alias=%lu mask=0x%08lx intf=0x%08lx\n",
                       (unsigned)(t->irq_base / 4u),
                       (unsigned long)alias,
                       (unsigned long)mask,
                       (unsigned long)t->intf);
            }
            return MM_TRUE;
        default:
            break;
        }
    }

    switch (base_off) {
    case TIMER_TIMEHW:
        t->time_us = ((mm_u64)value << 32) | (mm_u64)t->timelw_shadow;
        t->last_low = (mm_u32)t->time_us;
        return MM_TRUE;
    case TIMER_TIMELW:
        t->timelw_shadow = value;
        return MM_TRUE;
    case TIMER_ALARM0:
    case TIMER_ALARM1:
    case TIMER_ALARM2:
    case TIMER_ALARM3: {
        mm_u32 idx = (base_off - TIMER_ALARM0) / 4u;
        t->alarm[idx] = value;
        t->armed |= (1u << idx);
        if (g_timer_trace) {
            printf("[TIMER%u_ALARM_SET] alarm=%lu target=0x%08lx armed=0x%08lx\n",
                   (unsigned)(t->irq_base / 4u),
                   (unsigned long)idx,
                   (unsigned long)value,
                   (unsigned long)t->armed);
        }
        return MM_TRUE;
    }
    case TIMER_ARMED:
        t->armed &= ~(value & 0x0fu);
        if (g_timer_trace) {
            printf("[TIMER%u_ARMED_CLR] mask=0x%08lx armed=0x%08lx\n",
                   (unsigned)(t->irq_base / 4u),
                   (unsigned long)value,
                   (unsigned long)t->armed);
        }
        return MM_TRUE;
    case TIMER_INTR:
        t->intr &= ~(value & 0x0fu);
        if (g_timer_trace) {
            printf("[TIMER%u_INTR_CLR] mask=0x%08lx intr=0x%08lx\n",
                   (unsigned)(t->irq_base / 4u),
                   (unsigned long)value,
                   (unsigned long)t->intr);
        }
        return MM_TRUE;
    case TIMER_INTE:
        t->inte = value & 0x0fu;
        if (g_timer_trace) {
            printf("[TIMER%u_INTE] val=0x%08lx\n",
                   (unsigned)(t->irq_base / 4u),
                   (unsigned long)t->inte);
        }
        if (((t->intr | t->intf) & t->inte) != 0u) {
            mm_u32 i;
            for (i = 0; i < 4u; ++i) {
                if (((t->intr | t->intf) & (1u << i)) != 0u) {
                    timer_raise_irq(t, i);
                }
            }
        }
        return MM_TRUE;
    case TIMER_INTF:
        mask = value & 0x0fu;
        t->intf = mask;
        t->intr |= mask;
        if (g_timer_trace) {
            printf("[TIMER%u_INTF] mask=0x%08lx intr=0x%08lx\n",
                   (unsigned)(t->irq_base / 4u),
                   (unsigned long)mask,
                   (unsigned long)t->intr);
        }
        for (mask = 0u; mask < 4u; ++mask) {
            if ((value & (1u << mask)) != 0u) {
                timer_raise_irq(t, mask);
            }
        }
        return MM_TRUE;
    case TIMER_DBGPAUSE:
        t->dbgpause = value & 0x7u;
        return MM_TRUE;
    case TIMER_PAUSE:
        t->pause = value & 0x1u;
        return MM_TRUE;
    case TIMER_LOCKED:
        if ((value & 1u) != 0u) {
            t->locked = 1u;
        }
        return MM_TRUE;
    case TIMER_SOURCE:
        t->source = value & 0x1u;
        return MM_TRUE;
    default:
        break;
    }
    return MM_TRUE;
}

void mm_rp2350_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    struct mmio_region reg;
    if (bus == 0) return;
    g_nvic = nvic;
    mm_rp2350_pio_bind_nvic(nvic);

    memset(&reg, 0, sizeof(reg));
    reg.size = TIMER_SIZE;
    reg.base = TIMER0_BASE;
    reg.opaque = &g_timers[0];
    reg.read = timer_read;
    reg.write = timer_write;
    (void)mmio_bus_register_region(bus, &reg);

    reg.base = TIMER1_BASE;
    reg.opaque = &g_timers[1];
    (void)mmio_bus_register_region(bus, &reg);
}

void mm_rp2350_timers_reset(void)
{
    memset(g_timers, 0, sizeof(g_timers));
    g_timers[0].irq_base = 0u;
    g_timers[1].irq_base = 4u;
    g_timers[0].dbgpause = 7u;
    g_timers[1].dbgpause = 7u;
    g_cycle_accum = 0;
    {
        const char *env = getenv("M33MU_TIMER_TRACE");
        g_timer_trace = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
    }
}

void mm_rp2350_timers_tick(mm_u64 cycles)
{
    mm_u64 hz = RP2350_CLOCK_GET_HZ();
    mm_u64 cycles_per_us = (hz == 0u) ? 1u : (hz / 1000000ull);
    mm_u64 inc;
    int i;

    mm_rp2350_pio_tick(cycles);
    if (cycles_per_us == 0u) cycles_per_us = 1u;
    g_cycle_accum += cycles;
    inc = g_cycle_accum / cycles_per_us;
    if (inc == 0u) return;
    g_cycle_accum -= inc * cycles_per_us;
    for (i = 0; i < 2; ++i) {
        struct rp2350_timer *t = &g_timers[i];
        if ((t->pause & 1u) != 0u) {
            continue;
        }
        t->time_us += inc;
        timer_check_alarms(t);
    }
}
