/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/web.h"
#include <emscripten.h>

/* Bus callbacks into the worker: Module.m33muBus = { i2c, i2cBusy, spi,
 * spiEnd } is installed by the worker's bench layer. */
EM_JS(int, web_js_i2c_xfer, (int bus, int addr, int read, mm_u8 *data, int len), {
  const host = Module.m33muBus;
  return host && host.i2c ? host.i2c(bus, addr, read !== 0, data, len) : -1;
});
EM_JS(int, web_js_i2c_busy, (int bus), {
  const host = Module.m33muBus;
  return host && host.i2cBusy && host.i2cBusy(bus) ? 1 : 0;
});
EM_JS(int, web_js_spi_xfer, (int bus, int out), {
  const host = Module.m33muBus;
  return host && host.spi ? host.spi(bus, out) : -1;
});
EM_JS(void, web_js_spi_end, (int bus), {
  const host = Module.m33muBus;
  if (host && host.spiEnd) host.spiEnd(bus);
});

static int abi_i2c_xfer(void *opaque, int bus, mm_u8 addr, mm_bool read,
                        mm_u8 *data, size_t len)
{
    (void)opaque;
    return web_js_i2c_xfer(bus, addr, read ? 1 : 0, data, (int)len);
}

static mm_bool abi_i2c_busy(void *opaque, int bus)
{
    (void)opaque;
    return web_js_i2c_busy(bus) != 0 ? MM_TRUE : MM_FALSE;
}

static int abi_spi_xfer(void *opaque, int bus, mm_u8 out)
{
    (void)opaque;
    return web_js_spi_xfer(bus, out);
}

static void abi_spi_end(void *opaque, int bus)
{
    (void)opaque;
    web_js_spi_end(bus);
}

int mm_web_abi_enable_bus_host(void *session, int enable)
{
    static const struct mm_web_bus_host host = {
        abi_i2c_xfer, abi_i2c_busy, abi_spi_xfer, abi_spi_end, 0
    };
    return (int)mm_web_set_bus_host((struct mm_web_session *)session,
                                    enable ? &host : 0);
}

static enum mm_web_error g_last_error = MM_WEB_OK;
static struct mm_web_step_result g_last_step;
static struct mm_web_gpio_output_event g_last_gpio_output;

int mm_web_abi_version(void)
{
    return MM_WEB_ABI_VERSION;
}

unsigned int mm_web_abi_features(void)
{
    unsigned int features = 0u;
#ifdef M33MU_HAS_WOLFSSL
    features |= MM_WEB_FEATURE_WOLFCRYPT | MM_WEB_FEATURE_TA100;
#endif
#ifdef M33MU_HAS_RUST_PLUGINS
    features |= MM_WEB_FEATURE_RUST_SE;
#endif
    return features;
}

void *mm_web_abi_create(void)
{
    const struct mm_web_config config = { "stm32h563", 32u * 1024u * 1024u };
    struct mm_web_session *session = mm_web_create(&config, &g_last_error);
    return session;
}

int mm_web_abi_read_memory(void *session, unsigned int addr, void *out, unsigned int len)
{
    g_last_error = mm_web_read_memory((struct mm_web_session *)session, addr, (mm_u8 *)out, len);
    return (int)g_last_error;
}

int mm_web_abi_write_memory(void *session, unsigned int addr, const void *data, unsigned int len)
{
    g_last_error = mm_web_write_memory((struct mm_web_session *)session, addr,
                                       (const mm_u8 *)data, len);
    return (int)g_last_error;
}

void *mm_web_abi_create_cpu(const char *cpu_name)
{
    const struct mm_web_config config = { cpu_name, 32u * 1024u * 1024u };
    return mm_web_create(&config, &g_last_error);
}

int mm_web_abi_load(void *session, const void *data, unsigned int size)
{
    g_last_error = mm_web_load_elf((struct mm_web_session *)session,
                                   data, (size_t)size);
    return (int)g_last_error;
}

int mm_web_abi_step(void *session, unsigned int budget)
{
    g_last_error = mm_web_step_instructions(
        (struct mm_web_session *)session, (mm_u32)budget, &g_last_step);
    return (int)g_last_error;
}

int mm_web_abi_run_slice(void *session,
                         unsigned int instruction_budget,
                         unsigned int work_budget)
{
    g_last_error = mm_web_run_slice(
        (struct mm_web_session *)session, (mm_u32)instruction_budget,
        (mm_u32)work_budget, &g_last_step);
    return (int)g_last_error;
}

int mm_web_abi_run_until(void *session,
                         unsigned int deadline_high,
                         unsigned int deadline_low,
                         unsigned int instruction_budget,
                         unsigned int work_budget,
                         unsigned int flags)
{
    mm_u64 deadline_ns = ((mm_u64)deadline_high << 32) | deadline_low;
    g_last_error = mm_web_run_until(
        (struct mm_web_session *)session, deadline_ns,
        (mm_u32)instruction_budget, (mm_u32)work_budget, (mm_u32)flags,
        &g_last_step);
    return (int)g_last_error;
}

unsigned int mm_web_abi_last_deadline_reached(void)
{
    return g_last_step.deadline_reached ? 1u : 0u;
}

unsigned int mm_web_abi_last_pad_changed(void)
{
    return g_last_step.pad_changed ? 1u : 0u;
}

int mm_web_abi_cancel_gpio_inputs(void *session,
                                  const char *pad,
                                  unsigned int time_high,
                                  unsigned int time_low)
{
    size_t removed = 0u;
    mm_u64 from_ns = ((mm_u64)time_high << 32) | time_low;
    g_last_error = mm_web_cancel_gpio_inputs(
        (struct mm_web_session *)session,
        (pad != 0 && pad[0] != '\0') ? pad : 0, from_ns, &removed);
    return g_last_error == MM_WEB_OK ? (int)removed : -1;
}

/* Packed pad state: level | mode << 8 | open_drain << 16 | pull << 24 |
 * af << 26, or -1 on error. */
int mm_web_abi_pad_state(void *session, const char *pad)
{
    struct mm_web_pad_state state;
    g_last_error = mm_web_get_pad_state(
        (const struct mm_web_session *)session, pad, &state);
    if (g_last_error != MM_WEB_OK) return -1;
    return (int)((mm_u32)state.level | ((mm_u32)state.mode << 8) |
                 ((mm_u32)state.open_drain << 16) |
                 ((mm_u32)state.pull << 24) | ((mm_u32)state.af << 26));
}

unsigned int mm_web_abi_has_pending_deadline(void *session)
{
    return mm_web_has_pending_deadline(
        (const struct mm_web_session *)session) ? 1u : 0u;
}

int mm_web_abi_reset(void *session)
{
    g_last_error = mm_web_reset((struct mm_web_session *)session);
    return (int)g_last_error;
}

int mm_web_abi_gpio_input(void *session,
                          const char *pad,
                          unsigned int time_high,
                          unsigned int time_low,
                          int level)
{
    mm_u64 event_time_ns = ((mm_u64)time_high << 32) | time_low;
    g_last_error = mm_web_schedule_gpio_input(
        (struct mm_web_session *)session, pad,
        level ? MM_TRUE : MM_FALSE, event_time_ns);
    return (int)g_last_error;
}

int mm_web_abi_analog_input(void *session, const char *pad, int driven, int mv)
{
    g_last_error = mm_web_set_analog_input(
        (struct mm_web_session *)session, (pad != 0 && pad[0] != '\0') ? pad : 0,
        driven ? MM_TRUE : MM_FALSE, (mm_i32)mv);
    return (int)g_last_error;
}

int mm_web_abi_gpio_output_next(void *session)
{
    g_last_error = mm_web_pop_gpio_output(
        (struct mm_web_session *)session, &g_last_gpio_output);
    if (g_last_error == MM_WEB_OK) return 1;
    if (g_last_error == MM_WEB_NO_EVENT) return 0;
    return -1;
}

const char *mm_web_abi_gpio_output_pad(void)
{
    return g_last_gpio_output.soc_pad;
}

unsigned int mm_web_abi_gpio_output_level(void)
{
    return g_last_gpio_output.level;
}

unsigned int mm_web_abi_gpio_output_config(void)
{
    return (unsigned int)g_last_gpio_output.mode |
           ((unsigned int)g_last_gpio_output.open_drain << 8) |
           ((unsigned int)g_last_gpio_output.pull << 16) |
           ((unsigned int)g_last_gpio_output.af << 24);
}

unsigned int mm_web_abi_gpio_output_time_low(void)
{
    return (unsigned int)g_last_gpio_output.virtual_time_ns;
}

unsigned int mm_web_abi_gpio_output_time_high(void)
{
    return (unsigned int)(g_last_gpio_output.virtual_time_ns >> 32);
}

unsigned int mm_web_abi_last_error(void)
{
    return (unsigned int)g_last_error;
}

unsigned int mm_web_abi_last_instruction_count(void)
{
    return g_last_step.instructions;
}

unsigned int mm_web_abi_last_stopped(void)
{
    return g_last_step.stopped ? 1u : 0u;
}

unsigned int mm_web_abi_last_lockup(void)
{
    return g_last_step.lockup ? 1u : 0u;
}

unsigned int mm_web_abi_last_lockup_pc(void)
{
    return g_last_step.lockup_pc;
}

unsigned int mm_web_abi_last_breakpoint(void)
{
    return g_last_step.breakpoint ? 1u : 0u;
}

unsigned int mm_web_abi_last_breakpoint_pc(void)
{
    return g_last_step.breakpoint_pc;
}

int mm_web_abi_set_breakpoints(void *session, const mm_u32 *addresses,
                               unsigned int count)
{
    return (int)mm_web_set_breakpoints((struct mm_web_session *)session,
                                       addresses, count);
}

unsigned int mm_web_abi_last_system_reset(void)
{
    return g_last_step.system_reset ? 1u : 0u;
}

unsigned int mm_web_abi_cpu_reg(void *session, unsigned int index)
{
    struct mm_web_cpu_state snapshot;
    if (index >= 16u || mm_web_cpu_snapshot(
            (const struct mm_web_session *)session, &snapshot) != MM_WEB_OK) {
        return 0u;
    }
    return snapshot.r[index];
}

static mm_bool mm_web_abi_snapshot(void *session,
                                   struct mm_web_cpu_state *snapshot)
{
    return mm_web_cpu_snapshot((const struct mm_web_session *)session,
                               snapshot) == MM_WEB_OK;
}

unsigned int mm_web_abi_cycle_low(void *session)
{
    struct mm_web_cpu_state snapshot;
    if (!mm_web_abi_snapshot(session, &snapshot)) {
        return 0u;
    }
    return (unsigned int)snapshot.cycle_total;
}

unsigned int mm_web_abi_cycle_high(void *session)
{
    struct mm_web_cpu_state snapshot;
    if (!mm_web_abi_snapshot(session, &snapshot)) {
        return 0u;
    }
    return (unsigned int)(snapshot.cycle_total >> 32);
}

unsigned int mm_web_abi_xpsr(void *session)
{
    struct mm_web_cpu_state snapshot;
    if (!mm_web_abi_snapshot(session, &snapshot)) {
        return 0u;
    }
    return snapshot.xpsr;
}

#define MM_WEB_ABI_CPU_FIELD_GETTER(name, field) \
    unsigned int name(void *session) \
    { \
        struct mm_web_cpu_state snapshot; \
        return mm_web_abi_snapshot(session, &snapshot) ? \
            (unsigned int)snapshot.field : 0u; \
    }

MM_WEB_ABI_CPU_FIELD_GETTER(mm_web_abi_msp_s, msp_s)
MM_WEB_ABI_CPU_FIELD_GETTER(mm_web_abi_msp_ns, msp_ns)
MM_WEB_ABI_CPU_FIELD_GETTER(mm_web_abi_psp_s, psp_s)
MM_WEB_ABI_CPU_FIELD_GETTER(mm_web_abi_psp_ns, psp_ns)
MM_WEB_ABI_CPU_FIELD_GETTER(mm_web_abi_security_state, security_state)
MM_WEB_ABI_CPU_FIELD_GETTER(mm_web_abi_mode, mode)
MM_WEB_ABI_CPU_FIELD_GETTER(mm_web_abi_sleeping, sleeping)
MM_WEB_ABI_CPU_FIELD_GETTER(mm_web_abi_cpu_hz, cpu_hz)

unsigned int mm_web_abi_virtual_time_low(void *session)
{
    struct mm_web_cpu_state snapshot;
    if (!mm_web_abi_snapshot(session, &snapshot)) return 0u;
    return (unsigned int)snapshot.virtual_time_ns;
}

unsigned int mm_web_abi_virtual_time_high(void *session)
{
    struct mm_web_cpu_state snapshot;
    if (!mm_web_abi_snapshot(session, &snapshot)) return 0u;
    return (unsigned int)(snapshot.virtual_time_ns >> 32);
}

#undef MM_WEB_ABI_CPU_FIELD_GETTER

unsigned int mm_web_abi_uart_count(void *session)
{
    return (unsigned int)mm_web_uart_count((const struct mm_web_session *)session);
}

const char *mm_web_abi_uart_label(void *session, unsigned int index)
{
    const char *label = 0;
    if (mm_web_uart_info((const struct mm_web_session *)session, index, &label, 0) != MM_WEB_OK) {
        return "";
    }
    return label;
}

unsigned int mm_web_abi_uart_base(void *session, unsigned int index)
{
    mm_u32 base = 0u;
    (void)mm_web_uart_info((const struct mm_web_session *)session, index, 0, &base);
    return base;
}

static mm_bool g_last_uart_overflow;

/* Returns bytes copied into buf; mm_web_abi_uart_overflow() reports loss. */
unsigned int mm_web_abi_uart_read(void *session, unsigned int index,
                                  mm_u8 *buf, unsigned int max)
{
    return (unsigned int)mm_web_uart_read((struct mm_web_session *)session, index,
                                          buf, max, &g_last_uart_overflow);
}

unsigned int mm_web_abi_uart_overflow(void)
{
    return g_last_uart_overflow ? 1u : 0u;
}

unsigned int mm_web_abi_uart_write(void *session, unsigned int index,
                                   const mm_u8 *data, unsigned int len)
{
    return (unsigned int)mm_web_uart_write((struct mm_web_session *)session, index,
                                           data, len);
}

void mm_web_abi_destroy(void *session)
{
    mm_web_destroy((struct mm_web_session *)session);
}

int mm_web_abi_attach_device(void *session, const char *spec)
{
    return (int)mm_web_attach_device((struct mm_web_session *)session, spec);
}

void mm_web_abi_detach_devices(void *session)
{
    mm_web_detach_devices((struct mm_web_session *)session);
}
