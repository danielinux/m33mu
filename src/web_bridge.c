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
#include "m33mu/vector.h"

#define MM_WEB_DEFAULT_IMAGE_LIMIT (32u * 1024u * 1024u)
#define MM_WEB_CPUID 0x410fc241u
#define MM_WEB_GPIO_EVENT_CAPACITY 64u
#define MM_WEB_GPIO_OUTPUT_CAPACITY 4096u
#define MM_WEB_NSEC_PER_SEC 1000000000u
#define MM_WEB_TARGET_POLL_CYCLES 64u

struct mm_web_gpio_event {
    int bank;
    int pin;
    mm_bool level;
    mm_u64 cycle;
    mm_u64 sequence;
};

struct mm_web_gpio_output_entry {
    int bank;
    int pin;
    mm_u8 level;
    mm_u64 virtual_time_ns;
};

struct mm_web_session {
    struct mm_execution_state execution;
    size_t max_image_size;
    mm_u64 cycle_origin;
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
};

static struct mm_web_session *g_web_active_session;

static void web_gpio_pad_changed(void *opaque,
                                 int bank,
                                 int pin,
                                 mm_u8 level)
{
    struct mm_web_session *session =
        (struct mm_web_session *)opaque;
    char pad_name[16];
    int pins;
    if (session == 0 || session != g_web_active_session ||
        !mm_gpio_bank_info(bank, 0, 0u, &pins) ||
        pin < 0 || pin >= pins) {
        return;
    }
    if (session->gpio_output_count < MM_WEB_GPIO_OUTPUT_CAPACITY) {
        size_t tail = (session->gpio_output_head +
                       session->gpio_output_count) %
            MM_WEB_GPIO_OUTPUT_CAPACITY;
        session->gpio_output_events[tail].bank = bank;
        session->gpio_output_events[tail].pin = pin;
        session->gpio_output_events[tail].level = level;
        session->gpio_output_events[tail].virtual_time_ns =
            mm_execution_state_signal_now_ns(&session->execution);
        session->gpio_output_count++;
    } else {
        session->gpio_output_overflow = MM_TRUE;
    }
    if (session->pad_listener == 0) return;
    (void)snprintf(pad_name, sizeof(pad_name), "P%c%d", 'A' + bank, pin);
    session->pad_listener(session->pad_listener_opaque, pad_name, level);
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
    mm_u64 hz;
    mm_u64 seconds;
    mm_u64 remainder;
    mm_u64 fractional_numerator;
    mm_u64 whole_cycles;
    mm_u64 fractional_cycles;
    if (session == 0 || cycle_out == 0) return MM_FALSE;
    hz = mm_target_cpu_hz(&session->execution.target);
    if (hz == 0u) return MM_FALSE;
    seconds = event_time_ns / MM_WEB_NSEC_PER_SEC;
    remainder = event_time_ns % MM_WEB_NSEC_PER_SEC;
    if (seconds > (mm_u64)-1 / hz ||
        remainder > (mm_u64)-1 / hz) {
        return MM_FALSE;
    }
    whole_cycles = seconds * hz;
    fractional_numerator = remainder * hz;
    fractional_cycles = fractional_numerator / MM_WEB_NSEC_PER_SEC;
    if ((fractional_numerator % MM_WEB_NSEC_PER_SEC) != 0u) {
        fractional_cycles++;
    }
    if (whole_cycles > (mm_u64)-1 - fractional_cycles) {
        return MM_FALSE;
    }
    whole_cycles += fractional_cycles;
    if (whole_cycles < session->cycle_origin) return MM_FALSE;
    *cycle_out = whole_cycles - session->cycle_origin;
    return MM_TRUE;
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

static mm_bool web_unhandled_mem_fault(struct mm_cpu *cpu,
                                       struct mm_memmap *map,
                                       struct mm_scs *scs,
                                       mm_u32 fault_pc,
                                       mm_u32 fault_xpsr,
                                       mm_u32 addr,
                                       mm_bool is_exec)
{
    if (g_web_active_session != 0) {
        g_web_active_session->pending_error = MM_WEB_EXECUTION_FAULT;
    }
    (void)cpu;
    (void)map;
    (void)scs;
    (void)fault_pc;
    (void)fault_xpsr;
    (void)addr;
    (void)is_exec;
    return MM_FALSE;
}

static mm_bool web_exception_enter(void *opaque,
                                   struct mm_cpu *cpu,
                                   struct mm_memmap *map,
                                   struct mm_scs *scs,
                                   mm_u32 exc_num,
                                   mm_u32 return_pc,
                                   mm_u32 xpsr,
                                   enum mm_sec_state handler_sec);

/* Match the production UsageFault policy for architectural undefined
 * instructions: set UFSR, deliver UsageFault when enabled, otherwise escalate
 * to HardFault and set HFSR.FORCED. The CLI adds diagnostics around this same
 * architectural entry path. */
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
    mm_u32 exception;
    if (cpu == 0 || map == 0 || scs == 0) return MM_FALSE;
    sec = cpu->sec_state;
    cfsr = mm_scs_cfsr_ptr(scs, sec);
    if (cfsr == 0) return MM_FALSE;
    *cfsr |= ufsr_bits != 0u ? ufsr_bits : (1u << 16);
    enabled = sec == MM_NONSECURE ? scs->shcsr_ns : scs->shcsr_s;
    if ((enabled & (1u << 18)) != 0u) {
        exception = MM_VECT_USAGEFAULT;
        mm_scs_exception_set_active(scs, sec, exception);
    } else {
        exception = MM_VECT_HARDFAULT;
        scs->hfsr |= 1u << 30; /* FORCED */
        mm_scs_exception_set_active(scs, sec, exception);
    }
    if (!web_exception_enter(g_web_active_session, cpu, map, scs,
                             exception, fault_pc, fault_xpsr, sec)) {
        if (g_web_active_session != 0) {
            g_web_active_session->pending_error = MM_WEB_EXECUTION_FAULT;
        }
        return MM_FALSE;
    }
    return MM_TRUE;
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
    mm_gpio_set_pad_listener(web_gpio_pad_changed, session);
    session->max_image_size = config != 0 && config->max_image_size != 0u ?
        config->max_image_size : MM_WEB_DEFAULT_IMAGE_LIMIT;
    if (!mm_execution_state_select_target(&session->execution, cpu_name) ||
        !mm_execution_state_allocate_storage(&session->execution) ||
        !web_initialize_target_hardware(session)) {
        mm_execution_state_destroy(&session->execution);
        g_web_active_session = 0;
        mm_gpio_set_pad_listener(0, 0);
        free(session);
        if (error_out != 0) *error_out = MM_WEB_INITIALIZATION_FAILED;
        return 0;
    }
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
    mm_system_clear_reset();
    mm_execution_state_end_run(&session->execution);
    mm_execution_state_init_code_cache(&session->execution);
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
    session->cycle_origin = 0u;
    session->gpio_output_head = 0u;
    session->gpio_output_count = 0u;
    session->gpio_output_overflow = MM_FALSE;
    (void)mm_execution_state_signal_reset(&session->execution);
    (void)mm_execution_state_signal_set_cpu_hz(
        &session->execution,
        mm_target_cpu_hz(&session->execution.target));
    (void)mm_execution_state_signal_prepare_for_run(&session->execution);
    session->loaded = MM_TRUE;
    session->stopped = MM_FALSE;
    return MM_WEB_OK;
}

static enum mm_web_error web_reinitialize(
    struct mm_web_session *session,
    mm_bool reset_external_timeline)
{
    mm_u64 elapsed_cycles;
    size_t i;
    if (session == 0) return MM_WEB_INVALID_ARGUMENT;
    if (!session->loaded) return MM_WEB_NOT_BOOTED;
    mm_system_clear_reset();
    elapsed_cycles = session->execution.clock.virtual_cycles;
    if (reset_external_timeline) {
        session->cycle_origin = 0u;
        session->gpio_event_count = 0u;
        session->gpio_event_sequence = 0u;
        (void)mm_execution_state_signal_reset(&session->execution);
    } else {
        if (session->cycle_origin > (mm_u64)-1 - elapsed_cycles) {
            return MM_WEB_INITIALIZATION_FAILED;
        }
        session->cycle_origin += elapsed_cycles;
        for (i = 0u; i < session->gpio_event_count; ++i) {
            if (session->gpio_events[i].cycle < elapsed_cycles) {
                return MM_WEB_INITIALIZATION_FAILED;
            }
            session->gpio_events[i].cycle -= elapsed_cycles;
        }
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
    (void)mm_execution_state_signal_set_cpu_hz(
        &session->execution,
        mm_target_cpu_hz(&session->execution.target));
    (void)mm_execution_state_signal_prepare_for_run(&session->execution);
    session->stopped = MM_FALSE;
    return MM_WEB_OK;
}

enum mm_web_error mm_web_reset(struct mm_web_session *session)
{
    return web_reinitialize(session, MM_TRUE);
}

static enum mm_web_error web_execute_budgeted(
    struct mm_web_session *session,
    mm_u32 instruction_budget,
    mm_u32 work_budget,
    struct mm_web_step_result *result_out)
{
    struct mm_execution_state *state;
    mm_u32 i;
    mm_u32 work;
    if (result_out != 0) memset(result_out, 0, sizeof(*result_out));
    if (session == 0 || result_out == 0 || instruction_budget == 0u ||
        work_budget == 0u) {
        return MM_WEB_INVALID_ARGUMENT;
    }
    if (!session->loaded) return MM_WEB_NOT_BOOTED;
    if (session->stopped) {
        result_out->stopped = MM_TRUE;
        return MM_WEB_OK;
    }
    state = &session->execution;
    i = 0u;
    work = 0u;
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
            delta = mm_execution_state_cycles_until_wake(
                state, 0u, external_deadline);
            if (delta == (mm_u64)-1) break;
            poll_remaining = state->clock.cycles_since_poll >=
                MM_WEB_TARGET_POLL_CYCLES ? 1u :
                MM_WEB_TARGET_POLL_CYCLES - state->clock.cycles_since_poll;
            if (delta > poll_remaining) delta = poll_remaining;
            if (delta == 0u) delta = 1u;
            mm_execution_state_advance_cycles(
                state, 0u, delta, MM_TRUE,
                state->cores[0].cpu.r[15] & ~1u);
            work++;
            (void)mm_execution_state_poll_target_devices_if_due(
                state, MM_WEB_TARGET_POLL_CYCLES);
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
        context.raise_mem_fault = web_unhandled_mem_fault;
        context.raise_usage_fault = web_raise_usage_fault;
        context.exc_return_unstack = web_execution_exc_return;
        context.enter_exception = web_execution_instruction_exception;
        outcome = mm_execution_state_step_instruction(
            state, 0u, &context, mm_execution_exception_fpu_access_allowed, web_decode,
            web_unhandled_mem_fault, &fetch, &decoded, &fpu_ok,
            &exec_status);
        (void)mm_execution_state_poll_target_devices_if_due(
            state, MM_WEB_TARGET_POLL_CYCLES);
        if (session->pending_error != MM_WEB_OK) {
            result_out->error = session->pending_error;
            return result_out->error;
        }
        if (outcome == MM_EXECUTION_INSTRUCTION_UNDEFINED) {
            enum mm_execution_fault_status fault_status =
                mm_execution_state_raise_undefined(
                    state, 0u, web_raise_usage_fault, &fetch, &decoded);
            if (fault_status != MM_EXECUTION_FAULT_HANDLED) {
                result_out->error = session->pending_error != MM_WEB_OK ?
                    session->pending_error : MM_WEB_EXECUTION_FAULT;
                return result_out->error;
            }
            continue;
        }
        if (outcome == MM_EXECUTION_INSTRUCTION_FETCH_FAULT_UNHANDLED ||
            outcome == MM_EXECUTION_INSTRUCTION_INVALID) {
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
    result_out->error = MM_WEB_OK;
    return MM_WEB_OK;
}

enum mm_web_error mm_web_step_instructions(
    struct mm_web_session *session,
    mm_u32 instruction_budget,
    struct mm_web_step_result *result_out)
{
    return web_execute_budgeted(session, instruction_budget, instruction_budget,
                                result_out);
}

enum mm_web_error mm_web_run_slice(
    struct mm_web_session *session,
    mm_u32 instruction_budget,
    mm_u32 work_budget,
    struct mm_web_step_result *result_out)
{
    return web_execute_budgeted(session, instruction_budget, work_budget,
                                result_out);
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
           (session->gpio_events[insert_at - 1u].cycle > event_cycle ||
            (session->gpio_events[insert_at - 1u].cycle == event_cycle &&
             session->gpio_events[insert_at - 1u].sequence >
                session->gpio_event_sequence))) {
        session->gpio_events[insert_at] =
            session->gpio_events[insert_at - 1u];
        insert_at--;
    }
    session->gpio_events[insert_at].bank = bank;
    session->gpio_events[insert_at].pin = pin;
    session->gpio_events[insert_at].level = level;
    session->gpio_events[insert_at].cycle = event_cycle;
    session->gpio_events[insert_at].sequence = session->gpio_event_sequence++;
    session->gpio_event_count++;
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
    event_out->level = entry.level;
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

void mm_web_destroy(struct mm_web_session *session)
{
    if (session == 0) return;
    if (session == g_web_active_session) {
        mm_gpio_set_pad_listener(0, 0);
        g_web_active_session = 0;
    }
    mm_execution_state_destroy(&session->execution);
    free(session);
}
