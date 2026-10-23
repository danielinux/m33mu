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
    mm_bool system_reset;
    mm_bool deadline_reached;   /* mm_web_run_until only */
    mm_bool pad_changed;        /* a pad drive or configuration changed */
    enum mm_web_error error;
};

/* Pad mode/pull values follow MM_GPIO_MODE_* and MM_GPIO_PULL_* in gpio.h. */
struct mm_web_pad_state {
    mm_u8 level;                /* 0, 1 or 2 (Z: not driven by the MCU) */
    mm_u8 mode;
    mm_u8 open_drain;
    mm_u8 pull;
};

struct mm_web_gpio_output_event {
    char soc_pad[16];
    mm_u8 level;
    mm_u8 mode;
    mm_u8 open_drain;
    mm_u8 pull;
    mm_u64 virtual_time_ns;
};

#define MM_WEB_RUN_STOP_ON_PAD_CHANGE 1u

typedef void (*mm_web_pad_listener_fn)(void *opaque,
                                       const char *soc_pad,
                                       mm_u8 level);

/* Initial P0 bridge. Stepping is instruction-budgeted, bounds sleep-time
 * advancement by the same budget, and stops on BKPT. Architectural undefined
 * instructions set UFSR and enter enabled UsageFault or escalate to HardFault;
 * memory access/fetch faults enter enabled MemManage, pending SecureFault,
 * or HardFault. Full CLI fault diagnostics and lockup policy remain open.
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
/* Execute a yielding scheduler slice. The work budget counts scheduler
 * iterations, including bounded sleep-time advancement, so long WFI/WFE waits
 * return to the host between slices. */
enum mm_web_error mm_web_run_slice(
    struct mm_web_session *session,
    mm_u32 instruction_budget,
    mm_u32 work_budget,
    struct mm_web_step_result *result_out);
/* Run like mm_web_run_slice, but stop at the first instruction boundary at or
 * after deadline_ns (deadline_reached). Sleep-time advancement is capped at
 * the deadline. With MM_WEB_RUN_STOP_ON_PAD_CHANGE the slice also returns
 * after the instruction or sleep advance in which any pad drive level, mode,
 * output type or pull changed (pad_changed), so a host circuit model can
 * apply the new drive before time moves on. */
enum mm_web_error mm_web_run_until(
    struct mm_web_session *session,
    mm_u64 deadline_ns,
    mm_u32 instruction_budget,
    mm_u32 work_budget,
    mm_u32 flags,
    struct mm_web_step_result *result_out);
/* True while a sleeping core has a queued GPIO event or future SysTick/signal
 * deadline that requires virtual-time advancement. The event itself may not
 * wake the core unless firmware has configured the corresponding interrupt. */
mm_bool mm_web_has_pending_deadline(
    const struct mm_web_session *session);
/* Apply an input transition at the current virtual time. */
enum mm_web_error mm_web_set_gpio_input(
    struct mm_web_session *session,
    const char *soc_pad,
    mm_bool level);
/* Schedule a digital input transition in virtual nanoseconds. Due transitions
 * apply at the first CPU cycle at or after the timestamp, including across
 * firmware CPU-clock changes and system reset. The queue holds 64 events. */
enum mm_web_error mm_web_schedule_gpio_input(
    struct mm_web_session *session,
    const char *soc_pad,
    mm_bool level,
    mm_u64 event_time_ns);
/* Drop queued inputs at or after from_time_ns, for one pad or all pads
 * (soc_pad == NULL). Used when a host model retracts a predicted crossing. */
enum mm_web_error mm_web_cancel_gpio_inputs(
    struct mm_web_session *session,
    const char *soc_pad,
    mm_u64 from_time_ns,
    size_t *removed_out);
/* Current pad drive and configuration as last reported by the target. */
enum mm_web_error mm_web_get_pad_state(
    const struct mm_web_session *session,
    const char *soc_pad,
    struct mm_web_pad_state *state_out);
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
