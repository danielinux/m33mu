/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/web.h"
#include "m33mu/system_reset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m33mu/decode.h"
#include "m33mu/execution_state.h"
#include "m33mu/execution_exception.h"
#include "m33mu/gpio.h"
#include "m33mu/target_hal.h"
#include "m33mu/i2c_bus.h"
#include "m33mu/spi_bus.h"
#include "m33mu/vector.h"

#define MM_WEB_DEFAULT_IMAGE_LIMIT (32u * 1024u * 1024u)
#define MM_WEB_CPUID 0x410fc241u
#define MM_WEB_GPIO_EVENT_CAPACITY 64u
#define MM_WEB_GPIO_OUTPUT_CAPACITY 4096u
#define MM_WEB_NSEC_PER_SEC 1000000000u
#define MM_WEB_TARGET_POLL_CYCLES 64u
#define MM_WEB_PAD_BANKS 16
#define MM_WEB_PAD_PINS 32

struct mm_web_gpio_event {
    int bank;
    int pin;
    mm_bool level;
    mm_u64 time_ns;
    mm_u64 cycle;
    mm_u64 sequence;
};

struct mm_web_gpio_output_entry {
    int bank;
    int pin;
    struct mm_web_pad_state pad;
    mm_u64 virtual_time_ns;
};

struct mm_web_uart {
    struct mm_web_session *session;
    mm_u32 base;
    const char *label;
    mm_u8 tx[MM_WEB_UART_TX_CAPACITY];
    size_t tx_head;
    size_t tx_count;
    mm_bool tx_overflow;
    mm_u8 rx[MM_WEB_UART_RX_CAPACITY];
    size_t rx_head;
    size_t rx_count;
};

struct mm_web_session {
    struct mm_execution_state execution;
    size_t max_image_size;
    mm_u64 clock_epoch_cycle;
    mm_u64 clock_epoch_ns;
    mm_u64 clock_hz;
    mm_bool loaded;
    mm_bool stopped;
    enum mm_web_error pending_error;
    mm_web_pad_listener_fn pad_listener;
    void *pad_listener_opaque;
    struct mm_web_gpio_event gpio_events[MM_WEB_GPIO_EVENT_CAPACITY];
    size_t gpio_event_count;
    mm_u64 gpio_event_sequence;
    struct mm_web_gpio_output_entry
        gpio_output_events[MM_WEB_GPIO_OUTPUT_CAPACITY];
    size_t gpio_output_head;
    size_t gpio_output_count;
    mm_bool gpio_output_overflow;
    struct mm_web_pad_state pads[MM_WEB_PAD_BANKS][MM_WEB_PAD_PINS];
    mm_bool pad_changed;
    mm_bool locked_up;
    mm_u32 lockup_pc;
    struct mm_web_uart uarts[MM_UART_INSTANCE_MAX];
    size_t uart_count;
    struct mm_web_bus_host bus_host;
};

static struct mm_web_session *g_web_active_session;

static size_t web_uart_write_tx(void *opaque, const mm_u8 *data, size_t len)
{
    struct mm_web_uart *u = (struct mm_web_uart *)opaque;
    size_t i;
    for (i = 0; i < len; ++i) {
        if (u->tx_count == MM_WEB_UART_TX_CAPACITY) {
            u->tx_head = (u->tx_head + 1u) % MM_WEB_UART_TX_CAPACITY;
            u->tx_count--;
            u->tx_overflow = MM_TRUE;
        }
        u->tx[(u->tx_head + u->tx_count) % MM_WEB_UART_TX_CAPACITY] = data[i];
        u->tx_count++;
    }
    return len;
}

static mm_bool web_uart_read_rx(void *opaque, mm_u8 *byte_out)
{
    struct mm_web_uart *u = (struct mm_web_uart *)opaque;
    if (u->rx_count == 0u) return MM_FALSE;
    *byte_out = u->rx[u->rx_head];
    u->rx_head = (u->rx_head + 1u) % MM_WEB_UART_RX_CAPACITY;
    u->rx_count--;
    return MM_TRUE;
}

static mm_bool web_i2c_host_xfer(void *opaque, int bus, mm_u8 addr, mm_bool read,
                                 mm_u8 *data, size_t len, enum mm_i2c_status *status)
{
    struct mm_web_session *session = (struct mm_web_session *)opaque;
    int r;
    if (session->bus_host.i2c_xfer == 0) return MM_FALSE;
    r = session->bus_host.i2c_xfer(session->bus_host.opaque, bus, addr, read, data, len);
    if (r < 0) return MM_FALSE;
    *status = r > (int)MM_I2C_ARLO ? MM_I2C_NACK : (enum mm_i2c_status)r;
    return MM_TRUE;
}

static mm_bool web_i2c_host_busy(void *opaque, int bus)
{
    struct mm_web_session *session = (struct mm_web_session *)opaque;
    return session->bus_host.i2c_busy != 0 &&
           session->bus_host.i2c_busy(session->bus_host.opaque, bus);
}

static mm_bool web_spi_host_xfer(void *opaque, int bus, mm_u8 out, mm_u8 *in)
{
    struct mm_web_session *session = (struct mm_web_session *)opaque;
    int r;
    if (session->bus_host.spi_xfer == 0) return MM_FALSE;
    r = session->bus_host.spi_xfer(session->bus_host.opaque, bus, out);
    if (r < 0) return MM_FALSE;
    *in = (mm_u8)r;
    return MM_TRUE;
}

static void web_spi_host_end(void *opaque, int bus)
{
    struct mm_web_session *session = (struct mm_web_session *)opaque;
    if (session->bus_host.spi_end != 0) {
        session->bus_host.spi_end(session->bus_host.opaque, bus);
    }
}

static const struct mm_uart_backend_ops g_web_uart_ops = {
    web_uart_write_tx, web_uart_read_rx, 0
};

/* Bind every registered target UART to a session ring before firmware can
 * enable it; the USART model opens its backend when UE is set. */
static void web_attach_uarts(struct mm_web_session *session)
{
    size_t i;
    session->uart_count = 0u;
    for (i = 0; i < mm_uart_instance_count(); ++i) {
        struct mm_web_uart *u = &session->uarts[session->uart_count];
        mm_u32 base;
        const char *label;
        if (!mm_uart_instance_get(i, &base, &label)) continue;
        mm_uart_backend_detach(base);
        u->session = session;
        u->base = base;
        u->label = label;
        if (mm_uart_backend_attach(base, label, &g_web_uart_ops, u)) {
            session->uart_count++;
        }
    }
}

static void web_detach_uarts(struct mm_web_session *session)
{
    size_t i;
    for (i = 0; i < session->uart_count; ++i) {
        mm_uart_backend_detach(session->uarts[i].base);
    }
    session->uart_count = 0u;
}

static void web_queue_pad_state(struct mm_web_session *session,
                                int bank,
                                int pin)
{
    session->pad_changed = MM_TRUE;
    if (session->gpio_output_count < MM_WEB_GPIO_OUTPUT_CAPACITY) {
        size_t tail = (session->gpio_output_head +
                       session->gpio_output_count) %
            MM_WEB_GPIO_OUTPUT_CAPACITY;
        session->gpio_output_events[tail].bank = bank;
        session->gpio_output_events[tail].pin = pin;
        session->gpio_output_events[tail].pad = session->pads[bank][pin];
        session->gpio_output_events[tail].virtual_time_ns =
            mm_execution_state_signal_now_ns(&session->execution);
        session->gpio_output_count++;
    } else {
        session->gpio_output_overflow = MM_TRUE;
    }
}

static mm_bool web_pad_in_range(const struct mm_web_session *session,
                                int bank,
                                int pin)
{
    int pins;
    return session != 0 && session == g_web_active_session &&
           bank >= 0 && bank < MM_WEB_PAD_BANKS &&
           pin >= 0 && pin < MM_WEB_PAD_PINS &&
           mm_gpio_bank_info(bank, 0, 0u, &pins) && pin < pins;
}

static void web_gpio_pad_changed(void *opaque,
                                 int bank,
                                 int pin,
                                 mm_u8 level)
{
    struct mm_web_session *session =
        (struct mm_web_session *)opaque;
    char pad_name[16];
    if (!web_pad_in_range(session, bank, pin)) return;
    session->pads[bank][pin].level = level;
    web_queue_pad_state(session, bank, pin);
    if (session->pad_listener == 0) return;
    (void)snprintf(pad_name, sizeof(pad_name), "P%c%d", 'A' + bank, pin);
    session->pad_listener(session->pad_listener_opaque, pad_name, level);
}

static void web_gpio_pad_config_changed(
    void *opaque,
    int bank,
    int pin,
    const struct mm_gpio_pad_config *config)
{
    struct mm_web_session *session =
        (struct mm_web_session *)opaque;
    struct mm_web_pad_state *pad;
    if (!web_pad_in_range(session, bank, pin)) return;
    pad = &session->pads[bank][pin];
    if (pad->mode == config->mode && pad->open_drain == config->open_drain &&
        pad->pull == config->pull && pad->af == config->af) {
        return;
    }
    pad->af = config->af;
    pad->mode = config->mode;
    pad->open_drain = config->open_drain;
    pad->pull = config->pull;
    web_queue_pad_state(session, bank, pin);
}

static void web_init_pad_states(struct mm_web_session *session)
{
    int bank;
    int pin;
    for (bank = 0; bank < MM_WEB_PAD_BANKS; ++bank) {
        for (pin = 0; pin < MM_WEB_PAD_PINS; ++pin) {
            session->pads[bank][pin].level = (mm_u8)MM_GPIO_LEVEL_Z;
            session->pads[bank][pin].mode = (mm_u8)MM_GPIO_MODE_INPUT;
            session->pads[bank][pin].open_drain = 0u;
            session->pads[bank][pin].pull = (mm_u8)MM_GPIO_PULL_NONE;
        }
    }
}

static mm_bool web_parse_pad(const char *name, int *bank_out, int *pin_out)
{
    int bank;
    int pin = 0;
    const char *p;
    int available;
    if (name == 0 || bank_out == 0 || pin_out == 0 ||
        (name[0] != 'P' && name[0] != 'p') ||
        name[1] < 'A' || name[1] > 'Z') {
        return MM_FALSE;
    }
    bank = name[1] - 'A';
    if (!mm_gpio_bank_info(bank, 0, 0u, &available)) {
        return MM_FALSE;
    }
    p = name + 2;
    if (*p == '\0') return MM_FALSE;
    while (*p != '\0') {
        if (*p < '0' || *p > '9' || pin > 100000) {
            return MM_FALSE;
        }
        pin = pin * 10 + (*p - '0');
        ++p;
    }
    if (pin >= available) return MM_FALSE;
    *bank_out = bank;
    *pin_out = pin;
    return MM_TRUE;
}

static mm_bool web_event_time_to_cycle(const struct mm_web_session *session,
                                       mm_u64 event_time_ns,
                                       mm_u64 *cycle_out)
{
    mm_u64 delta_ns;
    mm_u64 seconds;
    mm_u64 remainder;
    mm_u64 fractional_numerator;
    mm_u64 whole_cycles;
    mm_u64 fractional_cycles;
    if (session == 0 || cycle_out == 0) return MM_FALSE;
    if (session->clock_hz == 0u) return MM_FALSE;
    if (event_time_ns <= session->clock_epoch_ns) {
        *cycle_out = session->clock_epoch_cycle;
        return MM_TRUE;
    }
    delta_ns = event_time_ns - session->clock_epoch_ns;
    seconds = delta_ns / MM_WEB_NSEC_PER_SEC;
    remainder = delta_ns % MM_WEB_NSEC_PER_SEC;
    if (seconds > (mm_u64)-1 / session->clock_hz ||
        remainder > (mm_u64)-1 / session->clock_hz) {
        return MM_FALSE;
    }
    whole_cycles = seconds * session->clock_hz;
    fractional_numerator = remainder * session->clock_hz;
    fractional_cycles = fractional_numerator / MM_WEB_NSEC_PER_SEC;
    if ((fractional_numerator % MM_WEB_NSEC_PER_SEC) != 0u) {
        fractional_cycles++;
    }
    if (whole_cycles > (mm_u64)-1 - fractional_cycles) {
        return MM_FALSE;
    }
    whole_cycles += fractional_cycles;
    if (whole_cycles > (mm_u64)-1 - session->clock_epoch_cycle)
        return MM_FALSE;
    *cycle_out = session->clock_epoch_cycle + whole_cycles;
    return MM_TRUE;
}

static mm_bool web_set_clock_rate(struct mm_web_session *session,
                                  mm_u64 hz)
{
    size_t i;
    if (hz == 0u) return MM_FALSE;
    session->clock_epoch_ns =
        mm_execution_state_signal_now_ns(&session->execution);
    session->clock_epoch_cycle = session->execution.clock.virtual_cycles;
    session->clock_hz = hz;
    if (!mm_execution_state_signal_set_cpu_hz(&session->execution, hz))
        return MM_FALSE;
    for (i = 0u; i < session->gpio_event_count; ++i) {
        if (!web_event_time_to_cycle(session,
                                     session->gpio_events[i].time_ns,
                                     &session->gpio_events[i].cycle)) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static mm_bool web_sync_clock_rate(struct mm_web_session *session)
{
    mm_u64 hz = mm_target_cpu_hz(&session->execution.target);
    return hz != 0u && (hz == session->clock_hz ||
                        web_set_clock_rate(session, hz));
}

static mm_bool web_apply_due_gpio_events(struct mm_web_session *session)
{
    struct mm_execution_state *state = &session->execution;
    while (session->gpio_event_count != 0u &&
           session->gpio_events[0].cycle <= state->clock.virtual_cycles) {
        struct mm_web_gpio_event event = session->gpio_events[0];
        if (!mm_gpio_external_input_writer_present()) {
            return MM_FALSE;
        }
        /* The board model accepts a pad stimulus even when the current GPIO
         * mode hides it from IDR (for example, reset-default analog mode). */
        (void)mm_gpio_set_external_input(event.bank, event.pin,
                                         event.level, 0u);
        if (session->gpio_event_count > 1u) {
            memmove(&session->gpio_events[0], &session->gpio_events[1],
                    (session->gpio_event_count - 1u) *
                        sizeof(session->gpio_events[0]));
        }
        session->gpio_event_count--;
    }
    return MM_TRUE;
}

static struct mm_decoded web_decode(
    const struct mm_fetch_result *fetch,
    const struct mm_cpu *cpu,
    const struct mm_scs *scs)
{
    (void)cpu;
    (void)scs;
    return mm_decode_t32(fetch);
}

static mm_bool web_exception_enter(void *opaque,
                                   struct mm_cpu *cpu,
                                   struct mm_memmap *map,
                                   struct mm_scs *scs,
                                   mm_u32 exc_num,
                                   mm_u32 return_pc,
                                   mm_u32 xpsr,
                                   enum mm_sec_state handler_sec);

static mm_bool web_enter_fault(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               struct mm_scs *scs,
                               mm_u32 exception,
                               mm_u32 fault_pc,
                               mm_u32 fault_xpsr,
                               enum mm_sec_state handler_sec)
{
    if (g_web_active_session == 0) return MM_FALSE;
    mm_scs_exception_set_active(scs, handler_sec, exception);
    if (web_exception_enter(g_web_active_session, cpu, map, scs,
                            exception, fault_pc, fault_xpsr, handler_sec)) {
        return MM_TRUE;
    }
    g_web_active_session->pending_error = MM_WEB_EXECUTION_FAULT;
    return MM_FALSE;
}

static mm_bool web_raise_hard_fault(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 fault_pc,
                                    mm_u32 fault_xpsr)
{
    enum mm_sec_state handler_sec = cpu->sec_state;
    if (handler_sec == MM_SECURE && (scs->aircr_s & (1u << 13)) != 0u) {
        handler_sec = MM_NONSECURE; /* AIRCR.BFHFNMINS */
    }
    scs->hfsr |= 1u << 30; /* FORCED */
    return web_enter_fault(cpu, map, scs, MM_VECT_HARDFAULT,
                           fault_pc, fault_xpsr, handler_sec);
}

static mm_bool web_raise_mem_fault(struct mm_cpu *cpu,
                                   struct mm_memmap *map,
                                   struct mm_scs *scs,
                                   mm_u32 fault_pc,
                                   mm_u32 fault_xpsr,
                                   mm_u32 addr,
                                   mm_bool is_exec)
{
    enum mm_sec_state sec = cpu->sec_state;
    mm_u32 *cfsr;
    mm_u32 *mmfar;
    mm_u32 enabled;
    if (scs->securefault_pending) {
        scs->securefault_pending = MM_FALSE;
        return web_enter_fault(cpu, map, scs, MM_VECT_SECUREFAULT,
                               fault_pc, fault_xpsr, MM_SECURE);
    }
    cfsr = mm_scs_cfsr_ptr(scs, sec);
    mmfar = mm_scs_mmfar_ptr(scs, sec);
    *cfsr |= (is_exec ? 1u : 2u) | (1u << 7); /* I/DACCVIOL, MMARVALID */
    *mmfar = addr;
    enabled = sec == MM_NONSECURE ? scs->shcsr_ns : scs->shcsr_s;
    if ((enabled & (1u << 16)) != 0u) {
        return web_enter_fault(cpu, map, scs, MM_VECT_MEMMANAGE,
                               fault_pc, fault_xpsr, sec);
    }
    return web_raise_hard_fault(cpu, map, scs, fault_pc, fault_xpsr);
}

/* Match production fault routing while using shared architectural exception
 * entry. CLI diagnostic and full lockup policy remain separate. */
static mm_bool web_raise_usage_fault(struct mm_cpu *cpu,
                                     struct mm_memmap *map,
                                     struct mm_scs *scs,
                                     mm_u32 fault_pc,
                                     mm_u32 fault_xpsr,
                                     mm_u32 ufsr_bits)
{
    enum mm_sec_state sec;
    mm_u32 *cfsr;
    mm_u32 enabled;
    if (cpu == 0 || map == 0 || scs == 0) return MM_FALSE;
    sec = cpu->sec_state;
    cfsr = mm_scs_cfsr_ptr(scs, sec);
    if (cfsr == 0) return MM_FALSE;
    *cfsr |= ufsr_bits != 0u ? ufsr_bits : (1u << 16);
    enabled = sec == MM_NONSECURE ? scs->shcsr_ns : scs->shcsr_s;
    if ((enabled & (1u << 18)) != 0u) {
        return web_enter_fault(cpu, map, scs, MM_VECT_USAGEFAULT,
                               fault_pc, fault_xpsr, sec);
    }
    return web_raise_hard_fault(cpu, map, scs, fault_pc, fault_xpsr);
}

static mm_bool web_execution_exc_return(struct mm_cpu *cpu,
                                        struct mm_memmap *map,
                                        struct mm_scs *scs,
                                        mm_u32 exc_return);

static mm_bool web_handle_pc_write(struct mm_cpu *cpu,
                                   struct mm_memmap *map,
                                   struct mm_scs *scs,
                                   mm_u32 value,
                                   mm_u8 *it_pattern,
                                   mm_u8 *it_remaining,
                                   mm_u8 *it_cond)
{
    (void)map;
    (void)scs;
    if (cpu != 0 && (value & 0xffffff00u) == 0xffffff00u) {
        return web_execution_exc_return(cpu, map, scs, value);
    }
    if (cpu == 0) {
        if (g_web_active_session != 0) {
            g_web_active_session->pending_error = MM_WEB_EXECUTION_FAULT;
        }
        return MM_FALSE;
    }
    if ((value & 1u) == 0u) {
        if (g_web_active_session != 0) {
            g_web_active_session->pending_error = MM_WEB_EXECUTION_FAULT;
        }
        return MM_FALSE;
    }
    cpu->r[15] = value | 1u;
    cpu->xpsr = itstate_set(cpu->xpsr, 0u);
    if (it_pattern != 0) *it_pattern = 0u;
    if (it_remaining != 0) *it_remaining = 0u;
    if (it_cond != 0) *it_cond = 0u;
    return MM_TRUE;
}

static mm_bool web_exception_fault(void *opaque,
                                   struct mm_cpu *cpu,
                                   struct mm_memmap *map,
                                   struct mm_scs *scs,
                                   mm_u32 fault_pc,
                                   mm_u32 fault_xpsr)
{
    (void)cpu;
    (void)map;
    (void)scs;
    (void)fault_pc;
    (void)fault_xpsr;
    ((struct mm_web_session *)opaque)->pending_error = MM_WEB_EXECUTION_FAULT;
    return MM_FALSE;
}

static mm_bool web_exception_usage_fault(void *opaque,
                                         struct mm_cpu *cpu,
                                         struct mm_memmap *map,
                                         struct mm_scs *scs,
                                         mm_u32 fault_pc,
                                         mm_u32 fault_xpsr,
                                         mm_u32 ufsr_bits)
{
    (void)ufsr_bits;
    return web_exception_fault(opaque, cpu, map, scs, fault_pc, fault_xpsr);
}

static mm_bool web_exception_enter(void *opaque,
                                   struct mm_cpu *cpu,
                                   struct mm_memmap *map,
                                   struct mm_scs *scs,
                                   mm_u32 exc_num,
                                   mm_u32 return_pc,
                                   mm_u32 xpsr,
                                   enum mm_sec_state handler_sec)
{
    struct mm_web_session *session = (struct mm_web_session *)opaque;
    const struct mm_execution_exception_ops ops = {
        session, web_exception_fault, web_exception_usage_fault,
        0, 0, 0, 0, 0, 0, 0, &session->execution.cores[0].nvic
    };
    return mm_execution_exception_enter(cpu, map, scs, exc_num,
                                        return_pc, xpsr, handler_sec, &ops);
}

static mm_bool web_exception_unstack(struct mm_web_session *session,
                                     struct mm_cpu *cpu,
                                     struct mm_memmap *map,
                                     struct mm_scs *scs,
                                     mm_u32 exc_return)
{
    const struct mm_execution_exception_ops ops = {
        session, web_exception_fault, web_exception_usage_fault,
        0, 0, 0, 0, 0, 0, 0, &session->execution.cores[0].nvic
    };
    return mm_execution_exception_unstack(cpu, map, scs, exc_return, &ops);
}

static mm_bool web_execution_irq_enter(void *opaque,
                                       struct mm_cpu *cpu,
                                       struct mm_memmap *map,
                                       struct mm_scs *scs,
                                       mm_u32 exc_num,
                                       mm_u32 return_pc,
                                       mm_u32 xpsr,
                                       enum mm_sec_state handler_sec)
{
    return web_exception_enter(opaque, cpu, map, scs, exc_num,
                               return_pc, xpsr, handler_sec);
}

static mm_bool web_execution_exc_return(struct mm_cpu *cpu,
                                        struct mm_memmap *map,
                                        struct mm_scs *scs,
                                        mm_u32 exc_return)
{
    if (g_web_active_session == 0) return MM_FALSE;
    return web_exception_unstack(g_web_active_session, cpu, map, scs,
                                 exc_return);
}

static mm_bool web_execution_instruction_exception(
    struct mm_cpu *cpu,
    struct mm_memmap *map,
    struct mm_scs *scs,
    mm_u32 exc_num,
    mm_u32 return_pc,
    mm_u32 xpsr)
{
    if (g_web_active_session == 0) return MM_FALSE;
    return web_exception_enter(g_web_active_session, cpu, map, scs,
                               exc_num, return_pc, xpsr, cpu->sec_state);
}

static mm_bool web_initialize_target_hardware(
    struct mm_web_session *session)
{
    struct mm_execution_state *state = &session->execution;
    mm_execution_state_init_memory_map(state);
    mm_execution_state_reset_soc(state);
    if (!mm_execution_state_configure_memory(state) ||
        !mm_execution_state_register_target_mmio(state) ||
        !mm_execution_state_bind_target_flash(
            state, state->storage.flash, state->target.flash_size_s, 0)) {
        return MM_FALSE;
    }
    mm_execution_state_init_code_cache(state);
    mm_execution_state_reset_core_system(state);
    mm_execution_state_init_target_devices(state);
    if (!mm_execution_state_init_core_system(state, MM_WEB_CPUID)) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

struct mm_web_session *mm_web_create(const struct mm_web_config *config,
                                     enum mm_web_error *error_out)
{
    struct mm_web_session *session;
    const char *cpu_name = config != 0 && config->cpu_name != 0 ?
        config->cpu_name : "stm32h563";
    if (error_out != 0) *error_out = MM_WEB_INVALID_ARGUMENT;
    if (strcmp(cpu_name, "stm32h563") != 0) {
        if (error_out != 0) *error_out = MM_WEB_UNSUPPORTED_TARGET;
        return 0;
    }
    if (g_web_active_session != 0) {
        if (error_out != 0) *error_out = MM_WEB_SESSION_BUSY;
        return 0;
    }
    session = (struct mm_web_session *)calloc(1u, sizeof(*session));
    if (session == 0) {
        if (error_out != 0) *error_out = MM_WEB_OUT_OF_MEMORY;
        return 0;
    }
    if (!mm_execution_state_init(&session->execution)) {
        free(session);
        if (error_out != 0) *error_out = MM_WEB_INITIALIZATION_FAILED;
        return 0;
    }
    g_web_active_session = session;
    mm_system_clear_reset();
    web_init_pad_states(session);
    mm_gpio_set_pad_listener(web_gpio_pad_changed, session);
    mm_gpio_set_pad_config_listener(web_gpio_pad_config_changed, session);
    session->max_image_size = config != 0 && config->max_image_size != 0u ?
        config->max_image_size : MM_WEB_DEFAULT_IMAGE_LIMIT;
    if (!mm_execution_state_select_target(&session->execution, cpu_name) ||
        !mm_execution_state_allocate_storage(&session->execution) ||
        !web_initialize_target_hardware(session)) {
        mm_execution_state_destroy(&session->execution);
        g_web_active_session = 0;
        mm_gpio_set_pad_listener(0, 0);
        mm_gpio_set_pad_config_listener(0, 0);
        free(session);
        if (error_out != 0) *error_out = MM_WEB_INITIALIZATION_FAILED;
        return 0;
    }
    web_attach_uarts(session);
    if (error_out != 0) *error_out = MM_WEB_OK;
    return session;
}

enum mm_web_error mm_web_load_elf(struct mm_web_session *session,
                                  const void *data,
                                  size_t size)
{
    struct mm_image_load_result result;
    if (session == 0 || data == 0 || size == 0u) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    if (size > session->max_image_size) return MM_WEB_IMAGE_REJECTED;
    session->loaded = MM_FALSE;
    mm_system_clear_reset();
    mm_execution_state_end_run(&session->execution);
    memset(session->execution.storage.flash, 0xff,
           session->execution.storage.flash_size);
    memset(session->execution.storage.ram, 0,
           session->execution.storage.ram_size);
    if (!web_initialize_target_hardware(session)) {
        return MM_WEB_INITIALIZATION_FAILED;
    }
    mm_execution_clock_reset(&session->execution.clock);
    if (mm_execution_state_load_elf(&session->execution,
                                    (const mm_u8 *)data, size,
                                    session->max_image_size, 0u,
                                    &result) != 0) {
        return MM_WEB_IMAGE_REJECTED;
    }
    mm_execution_state_reset_cores(&session->execution,
                                   session->execution.target.core_count,
                                   session->execution.target.flash_base_s,
                                   session->execution.target.flash_base_ns);
    if (!mm_execution_state_apply_reset_vector(&session->execution,
                                               MM_SECURE)) {
        return MM_WEB_IMAGE_REJECTED;
    }
    session->gpio_event_count = 0u;
    session->gpio_event_sequence = 0u;
    session->gpio_output_head = 0u;
    session->gpio_output_count = 0u;
    session->gpio_output_overflow = MM_FALSE;
    (void)mm_execution_state_signal_reset(&session->execution);
    if (!web_set_clock_rate(session,
                            mm_target_cpu_hz(&session->execution.target))) {
        return MM_WEB_INITIALIZATION_FAILED;
    }
    (void)mm_execution_state_signal_prepare_for_run(&session->execution);
    session->loaded = MM_TRUE;
    session->stopped = MM_FALSE;
    session->locked_up = MM_FALSE;
    return MM_WEB_OK;
}

static enum mm_web_error web_reinitialize(
    struct mm_web_session *session,
    mm_bool reset_external_timeline)
{
    if (session == 0) return MM_WEB_INVALID_ARGUMENT;
    if (!session->loaded) return MM_WEB_NOT_BOOTED;
    mm_system_clear_reset();
    if (reset_external_timeline) {
        session->gpio_event_count = 0u;
        session->gpio_event_sequence = 0u;
        (void)mm_execution_state_signal_reset(&session->execution);
    }
    mm_execution_state_end_run(&session->execution);
    if (!web_initialize_target_hardware(session)) {
        return MM_WEB_INITIALIZATION_FAILED;
    }
    mm_execution_clock_reset(&session->execution.clock);
    mm_execution_state_reset_cores(&session->execution,
                                   session->execution.target.core_count,
                                   session->execution.target.flash_base_s,
                                   session->execution.target.flash_base_ns);
    if (!mm_execution_state_apply_reset_vector(&session->execution,
                                               MM_SECURE)) {
        return MM_WEB_INITIALIZATION_FAILED;
    }
    if (reset_external_timeline) {
        session->gpio_output_head = 0u;
        session->gpio_output_count = 0u;
        session->gpio_output_overflow = MM_FALSE;
    }
    if (!web_set_clock_rate(session,
                            mm_target_cpu_hz(&session->execution.target))) {
        return MM_WEB_INITIALIZATION_FAILED;
    }
    (void)mm_execution_state_signal_prepare_for_run(&session->execution);
    session->stopped = MM_FALSE;
    session->locked_up = MM_FALSE;
    return MM_WEB_OK;
}

enum mm_web_error mm_web_reset(struct mm_web_session *session)
{
    return web_reinitialize(session, MM_TRUE);
}

static void web_lockup(struct mm_web_session *session,
                       mm_u32 pc,
                       struct mm_web_step_result *result_out)
{
    session->stopped = MM_TRUE;
    session->locked_up = MM_TRUE;
    session->lockup_pc = pc;
    result_out->stopped = MM_TRUE;
    result_out->lockup = MM_TRUE;
    result_out->lockup_pc = pc;
    result_out->pad_changed = session->pad_changed;
    result_out->error = MM_WEB_OK;
}

/* Cycle of the first instruction boundary at or after the run deadline. */
static mm_bool web_deadline_cycle(const struct mm_web_session *session,
                                  mm_bool has_deadline,
                                  mm_u64 deadline_ns,
                                  mm_u64 *cycle_out)
{
    if (!has_deadline) {
        *cycle_out = (mm_u64)-1;
        return MM_TRUE;
    }
    return web_event_time_to_cycle(session, deadline_ns, cycle_out);
}

static enum mm_web_error web_execute_budgeted(
    struct mm_web_session *session,
    mm_u32 instruction_budget,
    mm_u32 work_budget,
    mm_bool has_deadline,
    mm_u64 deadline_ns,
    mm_u32 flags,
    struct mm_web_step_result *result_out)
{
    struct mm_execution_state *state;
    mm_u32 i;
    mm_u32 work;
    mm_u64 deadline_cycle;
    mm_bool stop_on_pad = (flags & MM_WEB_RUN_STOP_ON_PAD_CHANGE) != 0u;
    if (result_out != 0) memset(result_out, 0, sizeof(*result_out));
    if (session == 0 || result_out == 0 || instruction_budget == 0u ||
        work_budget == 0u) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    if (!session->loaded) return MM_WEB_NOT_BOOTED;
    if (session->stopped) {
        result_out->stopped = MM_TRUE;
        result_out->lockup = session->locked_up;
        result_out->lockup_pc = session->lockup_pc;
        return MM_WEB_OK;
    }
    state = &session->execution;
    i = 0u;
    work = 0u;
    session->pad_changed = MM_FALSE;
    while (i < instruction_budget && work < work_budget &&
           !session->stopped) {
        struct mm_execute_ctx context;
        struct mm_fetch_result fetch;
        struct mm_decoded decoded;
        enum mm_exec_status exec_status = MM_EXEC_CONTINUE;
        enum mm_execution_instruction_outcome outcome;
        mm_bool fpu_ok = MM_FALSE;
        mm_bool done = MM_FALSE;
        session->pending_error = MM_WEB_OK;
        if (!web_sync_clock_rate(session) ||
            !web_deadline_cycle(session, has_deadline, deadline_ns,
                                &deadline_cycle)) {
            result_out->error = MM_WEB_INITIALIZATION_FAILED;
            return result_out->error;
        }
        if (state->clock.virtual_cycles >= deadline_cycle) {
            result_out->deadline_reached = MM_TRUE;
            break;
        }
        if (stop_on_pad && session->pad_changed) break;
        if (state->cores[0].cpu.sleeping) {
            mm_u64 external_deadline = 0u;
            mm_u64 poll_remaining;
            mm_u64 delta;
            if (!web_apply_due_gpio_events(session)) {
                result_out->error = MM_WEB_EXECUTION_FAULT;
                return result_out->error;
            }
            if (mm_execution_state_wake_sleeping_core(state, 0u)) {
                continue;
            }
            if (session->gpio_event_count != 0u) {
                mm_u64 now = state->clock.virtual_cycles;
                mm_u64 event_cycle = session->gpio_events[0].cycle;
                external_deadline = event_cycle > now ? event_cycle - now : 1u;
            }
            /* A run deadline is a host event too: an external circuit may
             * wake the core, so sleep advances to it rather than idling. */
            if (has_deadline &&
                (external_deadline == 0u ||
                 deadline_cycle - state->clock.virtual_cycles <
                     external_deadline)) {
                external_deadline = deadline_cycle - state->clock.virtual_cycles;
            }
            delta = mm_execution_state_cycles_until_wake(
                state, 0u, external_deadline);
            if (delta == (mm_u64)-1) break;
            poll_remaining = state->clock.cycles_since_poll >=
                MM_WEB_TARGET_POLL_CYCLES ? 1u :
                MM_WEB_TARGET_POLL_CYCLES - state->clock.cycles_since_poll;
            if (delta > poll_remaining) delta = poll_remaining;
            if (delta > deadline_cycle - state->clock.virtual_cycles) {
                delta = deadline_cycle - state->clock.virtual_cycles;
            }
            if (delta == 0u) delta = 1u;
            mm_execution_state_advance_cycles(
                state, 0u, delta, MM_TRUE,
                state->cores[0].cpu.r[15] & ~1u);
            work++;
            (void)mm_execution_state_poll_target_devices_if_due(
                state, MM_WEB_TARGET_POLL_CYCLES);
            if (!web_sync_clock_rate(session)) {
                result_out->error = MM_WEB_INITIALIZATION_FAILED;
                return result_out->error;
            }
            if (!web_apply_due_gpio_events(session)) {
                result_out->error = MM_WEB_EXECUTION_FAULT;
                return result_out->error;
            }
            (void)mm_execution_state_wake_sleeping_core(state, 0u);
            continue;
        }
        {
            enum mm_execution_irq_dispatch_result irq_result =
                mm_execution_state_dispatch_pending(
                    state, 0u, MM_EXECUTION_IRQ_SKIP_BLOCKED,
                    web_execution_irq_enter, session, &done);
            if (session->pending_error != MM_WEB_OK) {
                result_out->error = session->pending_error;
                return result_out->error;
            }
            if (irq_result == MM_EXECUTION_IRQ_DELIVERED) {
                work++;
                continue;
            }
        }
        /* the previous instruction's flash operation, if any */
        (void)mm_execution_state_charge_flash_stall(
            state, 0u, mm_target_cpu_hz(&state->target));
        if (!mm_execution_state_advance_for_instruction(state, 0u)) {
            result_out->error = MM_WEB_INITIALIZATION_FAILED;
            return result_out->error;
        }
        work++;
        if (!web_apply_due_gpio_events(session)) {
            result_out->error = MM_WEB_EXECUTION_FAULT;
            return result_out->error;
        }
        i++;
        memset(&context, 0, sizeof(context));
        context.done = &done;
        /* Stop on BKPT for a deterministic, host-budgeted headless API. */
        context.opt_expect_bkpt = MM_TRUE;
        context.handle_pc_write = web_handle_pc_write;
        context.raise_mem_fault = web_raise_mem_fault;
        context.raise_usage_fault = web_raise_usage_fault;
        context.exc_return_unstack = web_execution_exc_return;
        context.enter_exception = web_execution_instruction_exception;
        outcome = mm_execution_state_step_instruction(
            state, 0u, &context, mm_execution_exception_fpu_access_allowed, web_decode,
            web_raise_mem_fault, &fetch, &decoded, &fpu_ok,
            &exec_status);
        if (!web_sync_clock_rate(session)) {
            result_out->error = MM_WEB_INITIALIZATION_FAILED;
            return result_out->error;
        }
        (void)mm_execution_state_poll_target_devices_if_due(
            state, MM_WEB_TARGET_POLL_CYCLES);
        if (!web_apply_due_gpio_events(session)) {
            result_out->error = MM_WEB_EXECUTION_FAULT;
            return result_out->error;
        }
        if (session->pending_error == MM_WEB_EXECUTION_FAULT) {
            /* A fault whose exception entry itself failed (for example,
             * stacking to an invalid SP): the CLI reports a lockup. */
            web_lockup(session, state->cores[0].cpu.r[15] & ~1u, result_out);
            return MM_WEB_OK;
        }
        if (session->pending_error != MM_WEB_OK) {
            result_out->error = session->pending_error;
            return result_out->error;
        }
        if (outcome == MM_EXECUTION_INSTRUCTION_UNDEFINED) {
            enum mm_execution_fault_status fault_status =
                mm_execution_state_raise_undefined(
                    state, 0u, web_raise_usage_fault, &fetch, &decoded);
            if (fault_status != MM_EXECUTION_FAULT_HANDLED) {
                if (session->pending_error != MM_WEB_OK &&
                    session->pending_error != MM_WEB_EXECUTION_FAULT) {
                    result_out->error = session->pending_error;
                    return result_out->error;
                }
                /* Like the CLI: an undefined instruction whose fault cannot
                 * be taken locks the core up. */
                web_lockup(session, fetch.pc_fetch, result_out);
                return MM_WEB_OK;
            }
            continue;
        }
        if (outcome == MM_EXECUTION_INSTRUCTION_FETCH_FAULT_UNHANDLED) {
            web_lockup(session, state->cores[0].cpu.r[15] & ~1u, result_out);
            return MM_WEB_OK;
        }
        if (outcome == MM_EXECUTION_INSTRUCTION_INVALID) {
            result_out->error = MM_WEB_EXECUTION_FAULT;
            return result_out->error;
        }
        if (outcome == MM_EXECUTION_INSTRUCTION_FETCH_FAULT_HANDLED) {
            continue;
        }
        if (outcome != MM_EXECUTION_INSTRUCTION_EXECUTED &&
            outcome != MM_EXECUTION_INSTRUCTION_SKIPPED &&
            outcome != MM_EXECUTION_INSTRUCTION_YIELDED) {
            result_out->error = MM_WEB_EXECUTION_FAULT;
            return result_out->error;
        }
        result_out->instructions++;
        if (mm_system_reset_pending()) {
            mm_system_clear_reset();
            if (web_reinitialize(session, MM_FALSE) != MM_WEB_OK) {
                result_out->error = MM_WEB_INITIALIZATION_FAILED;
                return result_out->error;
            }
            result_out->system_reset = MM_TRUE;
            result_out->stopped = MM_FALSE;
            return MM_WEB_OK;
        }
        session->stopped = done;
    }
    result_out->stopped = session->stopped;
    result_out->pad_changed = session->pad_changed;
    if (has_deadline && !result_out->deadline_reached &&
        web_deadline_cycle(session, MM_TRUE, deadline_ns, &deadline_cycle) &&
        state->clock.virtual_cycles >= deadline_cycle) {
        result_out->deadline_reached = MM_TRUE;
    }
    result_out->error = MM_WEB_OK;
    return MM_WEB_OK;
}

enum mm_web_error mm_web_step_instructions(
    struct mm_web_session *session,
    mm_u32 instruction_budget,
    struct mm_web_step_result *result_out)
{
    return web_execute_budgeted(session, instruction_budget, instruction_budget,
                                MM_FALSE, 0u, 0u, result_out);
}

enum mm_web_error mm_web_run_slice(
    struct mm_web_session *session,
    mm_u32 instruction_budget,
    mm_u32 work_budget,
    struct mm_web_step_result *result_out)
{
    return web_execute_budgeted(session, instruction_budget, work_budget,
                                MM_FALSE, 0u, 0u, result_out);
}

enum mm_web_error mm_web_run_until(
    struct mm_web_session *session,
    mm_u64 deadline_ns,
    mm_u32 instruction_budget,
    mm_u32 work_budget,
    mm_u32 flags,
    struct mm_web_step_result *result_out)
{
    if ((flags & ~MM_WEB_RUN_STOP_ON_PAD_CHANGE) != 0u) {
        if (result_out != 0) memset(result_out, 0, sizeof(*result_out));
        return MM_WEB_INVALID_ARGUMENT;
    }
    return web_execute_budgeted(session, instruction_budget, work_budget,
                                MM_TRUE, deadline_ns, flags, result_out);
}

mm_bool mm_web_has_pending_deadline(
    const struct mm_web_session *session)
{
    mm_u64 host_deadline = 0u;
    mm_u64 now;
    mm_u64 event_cycle;
    if (session == 0 || !session->loaded ||
        !session->execution.cores[0].cpu.sleeping) {
        return MM_FALSE;
    }
    if (session->gpio_event_count != 0u) {
        now = session->execution.clock.virtual_cycles;
        event_cycle = session->gpio_events[0].cycle;
        host_deadline = event_cycle > now ? event_cycle - now : 1u;
    }
    return mm_execution_state_cycles_until_wake(
        &session->execution, 0u, host_deadline) != (mm_u64)-1;
}

enum mm_web_error mm_web_set_gpio_input(
    struct mm_web_session *session,
    const char *soc_pad,
    mm_bool level)
{
    if (session == 0) return MM_WEB_INVALID_ARGUMENT;
    if (!web_sync_clock_rate(session)) return MM_WEB_INITIALIZATION_FAILED;
    return mm_web_schedule_gpio_input(
        session, soc_pad, level,
        mm_execution_state_signal_now_ns(&session->execution));
}

enum mm_web_error mm_web_schedule_gpio_input(
    struct mm_web_session *session,
    const char *soc_pad,
    mm_bool level,
    mm_u64 event_time_ns)
{
    int bank;
    int pin;
    mm_u64 current_time_ns;
    mm_u64 event_cycle;
    size_t insert_at;
    if (session == 0 || soc_pad == 0 ||
        (level != MM_FALSE && level != MM_TRUE)) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    if (session != g_web_active_session) return MM_WEB_INITIALIZATION_FAILED;
    if (!session->loaded) return MM_WEB_NOT_BOOTED;
    if (!web_parse_pad(soc_pad, &bank, &pin)) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    if (!web_sync_clock_rate(session)) return MM_WEB_INITIALIZATION_FAILED;
    current_time_ns = mm_execution_state_signal_now_ns(&session->execution);
    if (event_time_ns < current_time_ns) return MM_WEB_EVENT_IN_PAST;
    if (!web_event_time_to_cycle(session, event_time_ns, &event_cycle)) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    if (event_cycle <= session->execution.clock.virtual_cycles) {
        if (!mm_gpio_external_input_writer_present()) {
            return MM_WEB_EXECUTION_FAULT;
        }
        (void)mm_gpio_set_external_input(bank, pin, level, 0u);
        return MM_WEB_OK;
    }
    if (session->gpio_event_count >= MM_WEB_GPIO_EVENT_CAPACITY) {
        return MM_WEB_EVENT_QUEUE_FULL;
    }
    insert_at = session->gpio_event_count;
    while (insert_at > 0u &&
           (session->gpio_events[insert_at - 1u].time_ns > event_time_ns ||
            (session->gpio_events[insert_at - 1u].time_ns == event_time_ns &&
             session->gpio_events[insert_at - 1u].sequence >
                session->gpio_event_sequence))) {
        session->gpio_events[insert_at] =
            session->gpio_events[insert_at - 1u];
        insert_at--;
    }
    session->gpio_events[insert_at].bank = bank;
    session->gpio_events[insert_at].pin = pin;
    session->gpio_events[insert_at].level = level;
    session->gpio_events[insert_at].time_ns = event_time_ns;
    session->gpio_events[insert_at].cycle = event_cycle;
    session->gpio_events[insert_at].sequence = session->gpio_event_sequence++;
    session->gpio_event_count++;
    return MM_WEB_OK;
}

enum mm_web_error mm_web_cancel_gpio_inputs(
    struct mm_web_session *session,
    const char *soc_pad,
    mm_u64 from_time_ns,
    size_t *removed_out)
{
    int bank = -1;
    int pin = -1;
    size_t i;
    size_t kept = 0u;
    if (removed_out != 0) *removed_out = 0u;
    if (session == 0) return MM_WEB_INVALID_ARGUMENT;
    if (session != g_web_active_session) return MM_WEB_INITIALIZATION_FAILED;
    if (soc_pad != 0 && !web_parse_pad(soc_pad, &bank, &pin)) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    for (i = 0u; i < session->gpio_event_count; ++i) {
        const struct mm_web_gpio_event *event = &session->gpio_events[i];
        if (event->time_ns >= from_time_ns &&
            (soc_pad == 0 || (event->bank == bank && event->pin == pin))) {
            continue;
        }
        session->gpio_events[kept++] = *event;
    }
    if (removed_out != 0) *removed_out = session->gpio_event_count - kept;
    session->gpio_event_count = kept;
    return MM_WEB_OK;
}

enum mm_web_error mm_web_get_pad_state(
    const struct mm_web_session *session,
    const char *soc_pad,
    struct mm_web_pad_state *state_out)
{
    int bank;
    int pin;
    if (session == 0 || state_out == 0) return MM_WEB_INVALID_ARGUMENT;
    if (session != g_web_active_session) return MM_WEB_INITIALIZATION_FAILED;
    if (!web_parse_pad(soc_pad, &bank, &pin) ||
        bank >= MM_WEB_PAD_BANKS || pin >= MM_WEB_PAD_PINS) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    *state_out = session->pads[bank][pin];
    return MM_WEB_OK;
}

enum mm_web_error mm_web_set_pad_listener(
    struct mm_web_session *session,
    mm_web_pad_listener_fn listener,
    void *opaque)
{
    if (session == 0 || session != g_web_active_session) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    session->pad_listener = listener;
    session->pad_listener_opaque = opaque;
    mm_gpio_set_pad_listener(web_gpio_pad_changed, session);
    return MM_WEB_OK;
}

enum mm_web_error mm_web_pop_gpio_output(
    struct mm_web_session *session,
    struct mm_web_gpio_output_event *event_out)
{
    struct mm_web_gpio_output_entry entry;
    if (session == 0 || event_out == 0) return MM_WEB_INVALID_ARGUMENT;
    if (session != g_web_active_session) return MM_WEB_INITIALIZATION_FAILED;
    if (session->gpio_output_overflow) return MM_WEB_EVENT_QUEUE_FULL;
    if (session->gpio_output_count == 0u) return MM_WEB_NO_EVENT;
    entry = session->gpio_output_events[session->gpio_output_head];
    session->gpio_output_head = (session->gpio_output_head + 1u) %
        MM_WEB_GPIO_OUTPUT_CAPACITY;
    session->gpio_output_count--;
    (void)snprintf(event_out->soc_pad, sizeof(event_out->soc_pad),
                   "P%c%d", 'A' + entry.bank, entry.pin);
    event_out->level = entry.pad.level;
    event_out->mode = entry.pad.mode;
    event_out->open_drain = entry.pad.open_drain;
    event_out->pull = entry.pad.pull;
    event_out->af = entry.pad.af;
    event_out->virtual_time_ns = entry.virtual_time_ns;
    return MM_WEB_OK;
}

enum mm_web_error mm_web_cpu_snapshot(
    const struct mm_web_session *session,
    struct mm_web_cpu_state *snapshot_out)
{
    const struct mm_execution_state *state;
    const struct mm_cpu *cpu;
    if (session == 0 || snapshot_out == 0) return MM_WEB_INVALID_ARGUMENT;
    if (!session->loaded) return MM_WEB_NOT_BOOTED;
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    state = &session->execution;
    cpu = &state->cores[0].cpu;
    memcpy(snapshot_out->r, cpu->r, sizeof(snapshot_out->r));
    snapshot_out->xpsr = cpu->xpsr;
    snapshot_out->msp_s = cpu->msp_s;
    snapshot_out->msp_ns = cpu->msp_ns;
    snapshot_out->psp_s = cpu->psp_s;
    snapshot_out->psp_ns = cpu->psp_ns;
    snapshot_out->cycle_total = state->clock.cycle_total;
    snapshot_out->virtual_cycles = state->clock.virtual_cycles;
    snapshot_out->virtual_time_ns =
        mm_execution_state_signal_now_ns(&session->execution);
    snapshot_out->cpu_hz = mm_target_cpu_hz(&state->target);
    snapshot_out->security_state = (mm_u32)cpu->sec_state;
    snapshot_out->mode = (mm_u32)cpu->mode;
    snapshot_out->sleeping = cpu->sleeping;
    return MM_WEB_OK;
}

enum mm_web_error mm_web_set_bus_host(struct mm_web_session *session,
                                      const struct mm_web_bus_host *host)
{
    struct mm_i2c_host i2c;
    struct mm_spi_host spi;
    if (session == 0) return MM_WEB_INVALID_ARGUMENT;
    if (session != g_web_active_session) return MM_WEB_INITIALIZATION_FAILED;
    if (host != 0) {
        session->bus_host = *host;
    } else {
        memset(&session->bus_host, 0, sizeof(session->bus_host));
    }
    i2c.xfer = web_i2c_host_xfer;
    i2c.lines_busy = web_i2c_host_busy;
    i2c.opaque = session;
    spi.xfer = web_spi_host_xfer;
    spi.end = web_spi_host_end;
    spi.opaque = session;
    mm_i2c_bus_set_host(host != 0 ? &i2c : 0);
    mm_spi_bus_set_host(host != 0 ? &spi : 0);
    return MM_WEB_OK;
}

size_t mm_web_uart_count(const struct mm_web_session *session)
{
    return session != 0 ? session->uart_count : 0u;
}

enum mm_web_error mm_web_uart_info(const struct mm_web_session *session,
                                   size_t index,
                                   const char **label_out,
                                   mm_u32 *base_out)
{
    if (session == 0 || index >= session->uart_count) return MM_WEB_INVALID_ARGUMENT;
    if (label_out != 0) *label_out = session->uarts[index].label;
    if (base_out != 0) *base_out = session->uarts[index].base;
    return MM_WEB_OK;
}

size_t mm_web_uart_read(struct mm_web_session *session, size_t index,
                        mm_u8 *buf, size_t max, mm_bool *overflow_out)
{
    struct mm_web_uart *u;
    size_t n = 0u;
    if (overflow_out != 0) *overflow_out = MM_FALSE;
    if (session == 0 || index >= session->uart_count || buf == 0) return 0u;
    u = &session->uarts[index];
    while (n < max && u->tx_count != 0u) {
        buf[n++] = u->tx[u->tx_head];
        u->tx_head = (u->tx_head + 1u) % MM_WEB_UART_TX_CAPACITY;
        u->tx_count--;
    }
    if (overflow_out != 0) *overflow_out = u->tx_overflow;
    u->tx_overflow = MM_FALSE;
    return n;
}

size_t mm_web_uart_write(struct mm_web_session *session, size_t index,
                         const mm_u8 *data, size_t len)
{
    struct mm_web_uart *u;
    size_t n = 0u;
    if (session == 0 || index >= session->uart_count || data == 0) return 0u;
    u = &session->uarts[index];
    while (n < len && u->rx_count < MM_WEB_UART_RX_CAPACITY) {
        u->rx[(u->rx_head + u->rx_count) % MM_WEB_UART_RX_CAPACITY] = data[n++];
        u->rx_count++;
    }
    return n;
}

void mm_web_destroy(struct mm_web_session *session)
{
    if (session == 0) return;
    if (session == g_web_active_session) {
        web_detach_uarts(session);
        mm_i2c_bus_set_host(0);
        mm_spi_bus_set_host(0);
        mm_gpio_set_pad_listener(0, 0);
        mm_gpio_set_pad_config_listener(0, 0);
        g_web_active_session = 0;
    }
    mm_execution_state_destroy(&session->execution);
    free(session);
}
