/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/web.h"

static enum mm_web_error g_last_error = MM_WEB_OK;
static struct mm_web_step_result g_last_step;
static struct mm_web_gpio_output_event g_last_gpio_output;

void *mm_web_abi_create(void)
{
    const struct mm_web_config config = { "stm32h563", 32u * 1024u * 1024u };
    struct mm_web_session *session = mm_web_create(&config, &g_last_error);
    return session;
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

void mm_web_abi_destroy(void *session)
{
    mm_web_destroy((struct mm_web_session *)session);
}
