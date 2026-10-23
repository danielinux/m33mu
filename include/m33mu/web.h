/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_WEB_H
#define M33MU_WEB_H

#include <stddef.h>
#include "m33mu/types.h"

struct mm_web_session;

enum mm_web_error {
    MM_WEB_OK = 0,
    MM_WEB_INVALID_ARGUMENT,
    MM_WEB_UNSUPPORTED_TARGET,
    MM_WEB_SESSION_BUSY,
    MM_WEB_OUT_OF_MEMORY,
    MM_WEB_INITIALIZATION_FAILED,
    MM_WEB_IMAGE_REJECTED,
    MM_WEB_NOT_BOOTED,
    MM_WEB_EXECUTION_FAULT,
    MM_WEB_UNSUPPORTED_INSTRUCTION,
    MM_WEB_EVENT_IN_PAST,
    MM_WEB_EVENT_QUEUE_FULL,
    MM_WEB_NO_EVENT
};

struct mm_web_config {
    const char *cpu_name;       /* P0 supports stm32h563 */
    size_t max_image_size;      /* zero selects the 32 MiB default */
};

struct mm_web_cpu_state {
    mm_u32 r[16];
    mm_u32 xpsr;
    mm_u32 msp_s;
    mm_u32 msp_ns;
    mm_u32 psp_s;
    mm_u32 psp_ns;
    mm_u64 cycle_total;
    mm_u64 virtual_cycles;
    mm_u64 virtual_time_ns;
    mm_u64 cpu_hz;
    mm_u32 security_state;
    mm_u32 mode;
    mm_bool sleeping;
};

struct mm_web_step_result {
    mm_u32 instructions;
    mm_bool stopped;
    enum mm_web_error error;
};

struct mm_web_gpio_output_event {
    char soc_pad[16];
    mm_u8 level;
    mm_u64 virtual_time_ns;
};

typedef void (*mm_web_pad_listener_fn)(void *opaque,
                                       const char *soc_pad,
                                       mm_u8 level);

/* Initial P0 bridge. Stepping is instruction-budgeted and stops on BKPT;
 * supported architectural exceptions use shared entry/unstack behavior, while
 * unhandled faults return errors. It is not yet the complete CLI scheduler.
 * Sessions are process-singleton while target GPIO/peripheral state
 * is global; a second create fails with MM_WEB_SESSION_BUSY. */
struct mm_web_session *mm_web_create(const struct mm_web_config *config,
                                     enum mm_web_error *error_out);
enum mm_web_error mm_web_load_elf(struct mm_web_session *session,
                                  const void *data,
                                  size_t size);
enum mm_web_error mm_web_reset(struct mm_web_session *session);
enum mm_web_error mm_web_step_instructions(
    struct mm_web_session *session,
    mm_u32 instruction_budget,
    struct mm_web_step_result *result_out);
/* Apply an input transition at the current virtual time. */
enum mm_web_error mm_web_set_gpio_input(
    struct mm_web_session *session,
    const char *soc_pad,
    mm_bool level);
/* Schedule a digital input transition in virtual nanoseconds. Due transitions
 * are applied before instruction fetch. The queue holds 64 pending events. */
enum mm_web_error mm_web_schedule_gpio_input(
    struct mm_web_session *session,
    const char *soc_pad,
    mm_bool level,
    mm_u64 event_time_ns);
enum mm_web_error mm_web_set_pad_listener(
    struct mm_web_session *session,
    mm_web_pad_listener_fn listener,
    void *opaque);
/* Pop queued output transitions. Returns MM_WEB_NO_EVENT when empty and
 * MM_WEB_EVENT_QUEUE_FULL after any output was lost due to queue overflow. */
enum mm_web_error mm_web_pop_gpio_output(
    struct mm_web_session *session,
    struct mm_web_gpio_output_event *event_out);
enum mm_web_error mm_web_cpu_snapshot(
    const struct mm_web_session *session,
    struct mm_web_cpu_state *snapshot_out);
void mm_web_destroy(struct mm_web_session *session);

#endif
