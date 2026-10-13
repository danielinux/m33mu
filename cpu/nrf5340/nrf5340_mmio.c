/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include <sys/random.h>
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include "nrf5340/nrf5340_mmio.h"
#include "nrf5340/nrf5340_wdt.h"
#include "nrf5340/nrf5340_cryptocell.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/gpio.h"
#include "m33mu/spi_bus.h"
#include "m33mu/host_rng.h"

extern void mm_system_request_reset(void);

#define CLOCK_BASE_NS 0x40005000u
#define CLOCK_BASE_S  0x50005000u
#define CLOCK_SIZE    0x1000u

#define CLOCK_TASKS_HFCLKSTART 0x000u
#define CLOCK_TASKS_HFCLKSTOP  0x004u
#define CLOCK_TASKS_LFCLKSTART 0x008u
#define CLOCK_TASKS_LFCLKSTOP  0x00Cu
#define CLOCK_TASKS_HFCLK192MSTART 0x020u
#define CLOCK_TASKS_HFCLK192MSTOP  0x024u
#define CLOCK_EVENTS_HFCLKSTARTED 0x100u
#define CLOCK_EVENTS_LFCLKSTARTED 0x104u
#define CLOCK_EVENTS_HFCLK192MSTARTED 0x124u
#define CLOCK_HFCLKRUN 0x408u
#define CLOCK_HFCLKSTAT 0x40Cu
#define CLOCK_LFCLKRUN 0x414u
#define CLOCK_LFCLKSTAT 0x418u
#define CLOCK_HFCLK192MRUN 0x458u
#define CLOCK_HFCLK192MSTAT 0x45Cu
#define CLOCK_HFCLK192MSRC 0x580u

#define RTC0_BASE_NS 0x40014000u
#define RTC0_BASE_S  0x50014000u
#define RTC_SIZE     0x1000u

#define RTC_TASKS_START 0x000u
#define RTC_TASKS_STOP  0x004u
#define RTC_TASKS_CLEAR 0x008u
#define RTC_EVENTS_TICK 0x100u
#define RTC_EVENTS_OVRFLW 0x104u
#define RTC_EVENTS_COMPARE0 0x140u
#define RTC_COMPARE_COUNT 4u
#define RTC_TASKS_CAPTURE0 0x040u
#define RTC_SHORTS   0x200u
#define RTC_INTENSET 0x304u
#define RTC_INTENCLR 0x308u
#define RTC_EVTEN    0x340u
#define RTC_EVTENSET 0x344u
#define RTC_EVTENCLR 0x348u
#define RTC_COUNTER   0x504u
#define RTC_PRESCALER 0x508u
#define RTC_CC0       0x540u
#define RTC_COUNTER_MASK 0x00FFFFFFu

#define CTRLAP_BASE_NS 0x40006000u
#define CTRLAP_BASE_S  0x50006000u
#define CTRLAP_SIZE    0x1000u

#define IPC_BASE_NS 0x4002A000u
#define IPC_BASE_S  0x5002A000u
#define IPC_SIZE    0x1000u

#define QSPI_BASE_NS 0x4002B000u
#define QSPI_BASE_S  0x5002B000u
#define QSPI_SIZE    0x1000u

#define QSPI_TASKS_ACTIVATE   0x000u
#define QSPI_TASKS_READSTART  0x004u
#define QSPI_TASKS_WRITESTART 0x008u
#define QSPI_TASKS_ERASESTART 0x00Cu
#define QSPI_TASKS_DEACTIVATE 0x010u
#define QSPI_EVENTS_READY     0x100u
#define QSPI_INTEN            0x300u
#define QSPI_INTENSET         0x304u
#define QSPI_INTENCLR         0x308u
#define QSPI_PSEL_BASE        0x524u
#define QSPI_PSEL_SCK         (QSPI_PSEL_BASE + 0x000u)
#define QSPI_PSEL_CSN         (QSPI_PSEL_BASE + 0x004u)

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

/* CryptoCell-312: secure-only, no NS alias */
#define CC312_CTRL_BASE_S   0x50844000u
#define CC312_CTRL_SIZE     0x1000u
#define CC312_ENGINE_BASE_S 0x50845000u
#define CC312_ENGINE_SIZE   0x2000u

struct clock_state {
    mm_u32 regs[CLOCK_SIZE / 4];
    mm_bool hfclk_on;
    mm_bool lfclk_on;
    mm_bool hfclk192m_on;
};

struct rtc_state {
    mm_u32 regs[RTC_SIZE / 4];
    mm_u32 cc[RTC_COMPARE_COUNT];
    mm_u64 accum;
    mm_bool running;
};

struct ctrlap_state {
    mm_u32 regs[CTRLAP_SIZE / 4];
};

struct ipc_state {
    mm_u32 regs[IPC_SIZE / 4];
};

struct qspi_state {
    mm_u32 regs[QSPI_SIZE / 4];
    mm_bool active;
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
static struct rtc_state rtc0_state;
static struct ctrlap_state ctrlap_state;
static struct ipc_state ipc_state;
static struct qspi_state qspi_state;
static struct gpio_bank gpio_banks[2];
static struct nvmc_state nvmc_state;
static struct rng_state rng_state;
static struct spu_state spu_state;
static struct mm_nrf5340_cryptocell *cc312_state;

static mm_bool qspi_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("M33MU_QSPI_TRACE");
        cached = (env != 0 && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return cached ? MM_TRUE : MM_FALSE;
}

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

mm_bool mm_nrf5340_clock_lf_running(void)
{
    return clock_state.lfclk_on;
}

static mm_bool nrf5340_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len)
{
    (void)opaque;
    if (out == 0 || out_len == 0u) {
        return MM_FALSE;
    }
    if (line != 0) {
        if (line != 1) {
            return MM_FALSE;
        }
        if (!clock_state.lfclk_on) {
            return MM_FALSE;
        }
        snprintf(out, out_len, "CLOCK: LFCLK");
        return MM_TRUE;
    }
    if (!clock_state.hfclk_on) {
        return MM_FALSE;
    }
    snprintf(out, out_len, "CLOCK: HFCLK");
    return MM_TRUE;
}

static mm_bool nrf5340_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= 2) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0u) {
        snprintf(name_out, name_len, "P%d", bank);
    }
    if (pins_out != 0) {
        *pins_out = (bank == 0) ? 32 : 16;
    }
    return MM_TRUE;
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
    if (offset == CLOCK_LFCLKRUN && size_bytes == 4) {
        *value_out = clk->lfclk_on ? 1u : 0u;
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLKSTAT && size_bytes == 4) {
        *value_out = clk->lfclk_on ? 1u : 0u;
        return MM_TRUE;
    }
    if (offset == CLOCK_HFCLK192MRUN && size_bytes == 4) {
        *value_out = clk->hfclk192m_on ? 1u : 0u;
        return MM_TRUE;
    }
    if (offset == CLOCK_HFCLK192MSTAT && size_bytes == 4) {
        mm_u32 src = clk->regs[CLOCK_HFCLK192MSRC / 4u] & 1u;
        *value_out = clk->hfclk192m_on ? (src | 0x10000u) : src;
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
    if (offset == CLOCK_TASKS_LFCLKSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->lfclk_on = MM_TRUE;
            clk->regs[CLOCK_EVENTS_LFCLKSTARTED / 4] = 1u;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_LFCLKSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->lfclk_on = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_HFCLK192MSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->hfclk192m_on = MM_TRUE;
            clk->regs[CLOCK_EVENTS_HFCLK192MSTARTED / 4] = 1u;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_HFCLK192MSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->hfclk192m_on = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_EVENTS_HFCLKSTARTED && size_bytes == 4) {
        if (value == 0u) {
            clk->regs[CLOCK_EVENTS_HFCLKSTARTED / 4] = 0u;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_EVENTS_LFCLKSTARTED && size_bytes == 4) {
        if (value == 0u) {
            clk->regs[CLOCK_EVENTS_LFCLKSTARTED / 4] = 0u;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_EVENTS_HFCLK192MSTARTED && size_bytes == 4) {
        if (value == 0u) {
            clk->regs[CLOCK_EVENTS_HFCLK192MSTARTED / 4] = 0u;
        }
        return MM_TRUE;
    }

    clk->regs[offset / 4] = apply_write(clk->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool rtc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rtc_state *rtc = (struct rtc_state *)opaque;
    if (rtc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RTC_SIZE) return MM_FALSE;
    if (offset == RTC_COUNTER && size_bytes == 4) {
        *value_out = rtc->regs[RTC_COUNTER / 4] & RTC_COUNTER_MASK;
        return MM_TRUE;
    }
    if (offset >= RTC_CC0 && offset < RTC_CC0 + RTC_COMPARE_COUNT * 4u && size_bytes == 4) {
        mm_u32 idx = (offset - RTC_CC0) / 4u;
        *value_out = rtc->cc[idx] & RTC_COUNTER_MASK;
        return MM_TRUE;
    }
    *value_out = read_slice(rtc->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool rtc_compare_enabled(const struct rtc_state *rtc, mm_u32 idx)
{
    mm_u32 mask = (1u << (16u + idx));
    if (rtc == 0 || idx >= RTC_COMPARE_COUNT) return MM_FALSE;
    if ((rtc->regs[RTC_EVTEN / 4] & mask) != 0u) return MM_TRUE;
    if ((rtc->regs[RTC_INTENSET / 4] & mask) != 0u) return MM_TRUE;
    return MM_FALSE;
}

static void rtc_set_compare_event(struct rtc_state *rtc, mm_u32 idx)
{
    mm_u32 off = RTC_EVENTS_COMPARE0 + idx * 4u;
    if (rtc == 0 || idx >= RTC_COMPARE_COUNT) return;
    rtc->regs[off / 4] = 1u;
}

static mm_bool rtc_crossed(mm_u32 old, mm_u32 now, mm_u64 ticks, mm_u32 target)
{
    if (ticks == 0u) return MM_FALSE;
    if (ticks >= ((mm_u64)RTC_COUNTER_MASK + 1u)) return MM_TRUE;
    if (old <= now) {
        return (target > old) && (target <= now);
    }
    /* Wrapped. */
    return (target > old) || (target <= now);
}

static mm_bool rtc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rtc_state *rtc = (struct rtc_state *)opaque;
    if (rtc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RTC_SIZE) return MM_FALSE;
    if (size_bytes != 4) return MM_FALSE;

    if (offset == RTC_TASKS_START) {
        if ((value & 1u) != 0u) {
            rtc->running = MM_TRUE;
        }
        return MM_TRUE;
    }
    if (offset == RTC_TASKS_STOP) {
        if ((value & 1u) != 0u) {
            rtc->running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == RTC_TASKS_CLEAR) {
        if ((value & 1u) != 0u) {
            rtc->regs[RTC_COUNTER / 4] = 0u;
        }
        return MM_TRUE;
    }
    if (offset >= RTC_TASKS_CAPTURE0 && offset < RTC_TASKS_CAPTURE0 + RTC_COMPARE_COUNT * 4u) {
        if ((value & 1u) != 0u) {
            mm_u32 idx = (offset - RTC_TASKS_CAPTURE0) / 4u;
            if (idx < RTC_COMPARE_COUNT) {
                rtc->cc[idx] = rtc->regs[RTC_COUNTER / 4] & RTC_COUNTER_MASK;
            }
        }
        return MM_TRUE;
    }
    if (offset == RTC_EVENTS_TICK || offset == RTC_EVENTS_OVRFLW) {
        if (value == 0u) {
            rtc->regs[offset / 4] = 0u;
        }
        return MM_TRUE;
    }
    if (offset >= RTC_EVENTS_COMPARE0 && offset < RTC_EVENTS_COMPARE0 + RTC_COMPARE_COUNT * 4u) {
        if (value == 0u) {
            rtc->regs[offset / 4] = 0u;
        }
        return MM_TRUE;
    }
    if (offset == RTC_SHORTS) {
        rtc->regs[RTC_SHORTS / 4] = value;
        return MM_TRUE;
    }
    if (offset == RTC_INTENSET) {
        rtc->regs[RTC_INTENSET / 4] |= value;
        return MM_TRUE;
    }
    if (offset == RTC_INTENCLR) {
        rtc->regs[RTC_INTENSET / 4] &= ~value;
        return MM_TRUE;
    }
    if (offset == RTC_EVTEN) {
        rtc->regs[RTC_EVTEN / 4] = value;
        return MM_TRUE;
    }
    if (offset == RTC_EVTENSET) {
        rtc->regs[RTC_EVTEN / 4] |= value;
        return MM_TRUE;
    }
    if (offset == RTC_EVTENCLR) {
        rtc->regs[RTC_EVTEN / 4] &= ~value;
        return MM_TRUE;
    }
    if (offset >= RTC_CC0 && offset < RTC_CC0 + RTC_COMPARE_COUNT * 4u) {
        mm_u32 idx = (offset - RTC_CC0) / 4u;
        rtc->cc[idx] = value & RTC_COUNTER_MASK;
        return MM_TRUE;
    }

    rtc->regs[offset / 4] = value;
    return MM_TRUE;
}

void mm_nrf5340_rtc_tick(mm_u64 cycles)
{
    struct rtc_state *rtc = &rtc0_state;
    mm_u64 cpu_hz;
    mm_u64 denom;
    mm_u64 ticks;
    mm_u32 presc;
    mm_u32 old;
    mm_u32 now;
    mm_u32 idx;

    if (!rtc->running) return;
    if (!mm_nrf5340_clock_lf_running()) return;

    cpu_hz = mm_nrf5340_cpu_hz();
    if (cpu_hz == 0u) return;

    presc = rtc->regs[RTC_PRESCALER / 4] & 0x0FFFu;
    denom = cpu_hz * (mm_u64)(presc + 1u);

    rtc->accum += cycles * 32768ull;
    ticks = rtc->accum / denom;
    rtc->accum = rtc->accum % denom;
    if (ticks == 0u) return;

    old = rtc->regs[RTC_COUNTER / 4] & RTC_COUNTER_MASK;
    now = (mm_u32)((old + (mm_u32)ticks) & RTC_COUNTER_MASK);
    rtc->regs[RTC_COUNTER / 4] = now;

    for (idx = 0; idx < RTC_COMPARE_COUNT; ++idx) {
        mm_bool crossed;
        if (!rtc_compare_enabled(rtc, idx)) continue;
        crossed = rtc_crossed(old, now, ticks, rtc->cc[idx] & RTC_COUNTER_MASK);
        if (!crossed) continue;
        rtc_set_compare_event(rtc, idx);
        if (idx == 0 && (rtc->regs[RTC_SHORTS / 4] & 1u) != 0u) {
            rtc->regs[RTC_COUNTER / 4] = 0u;
        }
    }
}

static mm_bool ctrlap_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct ctrlap_state *ap = (struct ctrlap_state *)opaque;
    if (ap == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CTRLAP_SIZE) return MM_FALSE;
    *value_out = read_slice(ap->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool ctrlap_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct ctrlap_state *ap = (struct ctrlap_state *)opaque;
    if (ap == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CTRLAP_SIZE) return MM_FALSE;
    ap->regs[offset / 4] = apply_write(ap->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool ipc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct ipc_state *ipc = (struct ipc_state *)opaque;
    if (ipc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > IPC_SIZE) return MM_FALSE;
    *value_out = read_slice(ipc->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool ipc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct ipc_state *ipc = (struct ipc_state *)opaque;
    if (ipc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > IPC_SIZE) return MM_FALSE;
    ipc->regs[offset / 4] = apply_write(ipc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool qspi_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct qspi_state *qspi = (struct qspi_state *)opaque;
    if (qspi == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > QSPI_SIZE) return MM_FALSE;
    *value_out = read_slice(qspi->regs[offset / 4], offset & 3u, size_bytes);
    if (qspi_trace_enabled() && offset == QSPI_EVENTS_READY && size_bytes == 4u) {
        fprintf(stderr, "[QSPI] EVENTS_READY read => 0x%08lx\n", (unsigned long)*value_out);
    }
    return MM_TRUE;
}

static mm_bool qspi_csn_decode(mm_u32 cfg, int *bank_out, int *pin_out)
{
    mm_u32 connect = (cfg >> 31) & 1u;
    mm_u32 pin = cfg & 0x1Fu;
    mm_u32 port = (cfg >> 5) & 1u;
    if (bank_out == 0 || pin_out == 0) return MM_FALSE;
    if (connect != 0u) return MM_FALSE;
    if (pin > 31u || port > 1u) return MM_FALSE;
    *bank_out = (int)port;
    *pin_out = (int)pin;
    return MM_TRUE;
}

static void qspi_drive_csn(mm_u8 level)
{
    int bank;
    int pin;
    mm_u32 cfg = qspi_state.regs[QSPI_PSEL_CSN / 4];
    if (!qspi_csn_decode(cfg, &bank, &pin)) return;
    if (bank < 0 || bank >= 2 || pin < 0 || pin >= 32) return;
    {
        mm_u32 mask = (pin >= 31) ? 0x80000000u : (1u << (mm_u32)pin);
        if (level == 0u) {
            gpio_banks[bank].regs[GPIO_OUT / 4] &= ~mask;
        } else {
            gpio_banks[bank].regs[GPIO_OUT / 4] |= mask;
        }
        gpio_banks[bank].regs[GPIO_DIR / 4] |= mask;
    }
    mm_spi_bus_poll_cs(0);
}

static void qspi_complete_task(void)
{
    /* QSPI READY is raised in response to tasks except DEACTIVATE. */
    qspi_state.regs[QSPI_EVENTS_READY / 4] = 1u;
}

static mm_bool qspi_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct qspi_state *qspi = (struct qspi_state *)opaque;
    mm_u32 reg_index;
    mm_u32 prev;
    mm_u32 next;
    mm_bool task_trigger = MM_FALSE;
    if (qspi == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > QSPI_SIZE) return MM_FALSE;
    reg_index = offset / 4u;
    prev = qspi->regs[reg_index];
    next = apply_write(prev, offset & 3u, size_bytes, value);

    if (offset == QSPI_EVENTS_READY) {
        if (next == 0u) {
            /* Nordic SDK often clears READY aggressively; keep it asserted
             * while the peripheral is active to avoid deadlock in wait loops. */
            qspi->regs[reg_index] = qspi->active ? 1u : 0u;
        } else {
            qspi->regs[reg_index] = next;
        }
        if (qspi_trace_enabled()) {
            fprintf(stderr,
                    "[QSPI] EVENTS_READY write size=%lu val=0x%08lx => 0x%08lx\n",
                    (unsigned long)size_bytes,
                    (unsigned long)value,
                    (unsigned long)qspi->regs[reg_index]);
        }
        return MM_TRUE;
    }
    if (offset == QSPI_INTEN) {
        qspi->regs[QSPI_INTEN / 4] = next & 1u;
        return MM_TRUE;
    }
    if (offset == QSPI_INTENSET) {
        qspi->regs[QSPI_INTEN / 4] |= (next & 1u);
        return MM_TRUE;
    }
    if (offset == QSPI_INTENCLR) {
        qspi->regs[QSPI_INTEN / 4] &= ~(next & 1u);
        return MM_TRUE;
    }
    if (offset == QSPI_PSEL_CSN) {
        /* Ensure CS is deasserted when pin selection changes. */
        qspi->regs[reg_index] = next;
        qspi_drive_csn(1u);
        return MM_TRUE;
    }

    qspi->regs[reg_index] = next;

    if (offset == QSPI_TASKS_ACTIVATE) {
        task_trigger = (next & 1u) != 0u;
    } else if (offset == QSPI_TASKS_READSTART ||
               offset == QSPI_TASKS_WRITESTART ||
               offset == QSPI_TASKS_ERASESTART) {
        task_trigger = (next & 1u) != 0u;
    } else if (offset == QSPI_TASKS_DEACTIVATE) {
        task_trigger = (next & 1u) != 0u;
        if (task_trigger) {
            qspi->active = MM_FALSE;
            qspi->regs[QSPI_EVENTS_READY / 4] = 0u;
            qspi_drive_csn(1u);
            mm_spi_bus_end(0);
            if (qspi_trace_enabled()) {
                fprintf(stderr, "[QSPI] TASKS_DEACTIVATE trigger\n");
            }
        }
        return MM_TRUE;
    }
    if (qspi_trace_enabled() &&
        (offset == QSPI_TASKS_ACTIVATE ||
         offset == QSPI_TASKS_READSTART ||
         offset == QSPI_TASKS_WRITESTART ||
         offset == QSPI_TASKS_ERASESTART)) {
        fprintf(stderr,
                "[QSPI] TASKS write off=0x%03lx size=%lu val=0x%08lx next=0x%08lx\n",
                (unsigned long)offset,
                (unsigned long)size_bytes,
                (unsigned long)value,
                (unsigned long)next);
    }

    if (!task_trigger) {
        return MM_TRUE;
    }

    qspi->active = MM_TRUE;
    if (offset == QSPI_TASKS_ACTIVATE) {
        qspi_drive_csn(1u);
    } else {
        qspi_drive_csn(0u);
        /* We do not emulate full QSPI transactions yet; release CS. */
        qspi_drive_csn(1u);
        mm_spi_bus_end(0);
    }
    qspi_complete_task();
    if (qspi_trace_enabled()) {
        fprintf(stderr, "[QSPI] task 0x%03lx trigger => EVENTS_READY=1\n", (unsigned long)offset);
    }
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
        rng->value = (mm_u8)(mm_host_rng_u32() & 0xFFu);
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
            rng->value = (mm_u8)(mm_host_rng_u32() & 0xFFu);
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
    memset(&reg, 0, sizeof(reg));

    memset(&clock_state, 0, sizeof(clock_state));
    clock_state.hfclk_on = MM_TRUE;

    memset(&rtc0_state, 0, sizeof(rtc0_state));
    memset(&ctrlap_state, 0, sizeof(ctrlap_state));
    memset(&ipc_state, 0, sizeof(ipc_state));
    memset(&qspi_state, 0, sizeof(qspi_state));
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

    reg.base = RTC0_BASE_NS;
    reg.size = RTC_SIZE;
    reg.opaque = &rtc0_state;
    reg.read = rtc_read;
    reg.write = rtc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = RTC0_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = CTRLAP_BASE_NS;
    reg.size = CTRLAP_SIZE;
    reg.opaque = &ctrlap_state;
    reg.read = ctrlap_read;
    reg.write = ctrlap_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = CTRLAP_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = IPC_BASE_NS;
    reg.size = IPC_SIZE;
    reg.opaque = &ipc_state;
    reg.read = ipc_read;
    reg.write = ipc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = IPC_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = QSPI_BASE_NS;
    reg.size = QSPI_SIZE;
    reg.opaque = &qspi_state;
    reg.read = qspi_read;
    reg.write = qspi_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = QSPI_BASE_S;
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

    /* Note: RNG_BASE_NS/S (0x40845000/0x50845000) is the CC-312 engine base.
     * The real nRF5340 does not have a standalone RNG at this address —
     * random generation is provided by CC_RNG inside the CryptoCell block.
     * The CC-312 engine region registered below covers this address range.
     * The rng_state and rng_read/write are retained for potential future
     * use but are not registered as a separate MMIO region. */
    (void)rng_read;
    (void)rng_write;

    reg.base = SPU_BASE_S;
    reg.size = SPU_SIZE;
    reg.opaque = &spu_state;
    reg.read = spu_read;
    reg.write = spu_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    if (!mm_nrf5340_wdt_register(bus)) return MM_FALSE;

    /* CryptoCell-312: secure-only regions */
    if (cc312_state == NULL) {
        cc312_state = mm_nrf5340_cryptocell_new();
        if (cc312_state == NULL) return MM_FALSE;
    }
    mm_nrf5340_cryptocell_reset(cc312_state);

    reg.base  = CC312_CTRL_BASE_S;
    reg.size  = CC312_CTRL_SIZE;
    reg.opaque = cc312_state;
    reg.read  = mm_nrf5340_cryptocell_ctrl_read;
    reg.write = mm_nrf5340_cryptocell_ctrl_write;
    reg.name  = "cc312_ctrl";
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base  = CC312_ENGINE_BASE_S;
    reg.size  = CC312_ENGINE_SIZE;
    reg.opaque = cc312_state;
    reg.read  = mm_nrf5340_cryptocell_read;
    reg.write = mm_nrf5340_cryptocell_write;
    reg.name  = "cc312_engine";
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

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

    /* Wire memmap into CryptoCell-312 so DMA can read/write target RAM */
    if (cc312_state != 0) {
        mm_nrf5340_cryptocell_set_memmap(cc312_state, map);
    }
}

void mm_nrf5340_set_nvic(struct mm_nvic *nvic)
{
    if (cc312_state != 0) {
        mm_nrf5340_cryptocell_set_nvic(cc312_state, nvic);
    }
}

mm_u64 mm_nrf5340_cpu_hz(void)
{
    return 128000000ull;
}

void mm_nrf5340_mmio_reset(void)
{
    memset(&clock_state, 0, sizeof(clock_state));
    clock_state.hfclk_on = MM_TRUE;
    clock_state.lfclk_on = MM_FALSE;
    clock_state.hfclk192m_on = MM_FALSE;
    clock_state.regs[CLOCK_EVENTS_HFCLKSTARTED / 4] = 0u;
    clock_state.regs[CLOCK_EVENTS_LFCLKSTARTED / 4] = 0u;
    clock_state.regs[CLOCK_EVENTS_HFCLK192MSTARTED / 4] = 0u;
    clock_state.regs[CLOCK_HFCLK192MSRC / 4] = 1u;

    memset(&rtc0_state, 0, sizeof(rtc0_state));
    memset(&ctrlap_state, 0, sizeof(ctrlap_state));
    memset(&ipc_state, 0, sizeof(ipc_state));
    memset(&qspi_state, 0, sizeof(qspi_state));
    memset(&gpio_banks, 0, sizeof(gpio_banks));
    memset(&rng_state, 0, sizeof(rng_state));
    memset(&spu_state, 0, sizeof(spu_state));

    memset(nvmc_state.regs, 0, sizeof(nvmc_state.regs));
    nvmc_state.regs[NVMC_READY / 4] = 1u;
    nvmc_state.regs[NVMC_READYNEXT / 4] = 1u;
    nvmc_state.regs[NVMC_CONFIG / 4] = 0u;

    mm_gpio_set_bank_info_reader(nrf5340_gpio_bank_info, 0);
    mm_rcc_set_clock_list_reader(nrf5340_rcc_clock_list_line, 0);
    mm_nrf5340_wdt_reset();

    if (cc312_state != 0) {
        mm_nrf5340_cryptocell_reset(cc312_state);
    }
}
