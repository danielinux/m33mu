/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_EXECUTION_STATE_H
#define M33MU_EXECUTION_STATE_H

#include "m33mu/cpu.h"
#include "m33mu/execution_clock.h"
#include "m33mu/nvic.h"
#include "m33mu/scs.h"
#include "m33mu/memmap.h"
#include "m33mu/code_cache.h"
#include "m33mu/mem_prot.h"
#include "m33mu/core_sys.h"
#include "m33mu/target_storage.h"
#include "m33mu/image_loader.h"
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/execute.h"
#include "m33mu/execution_irq.h"

struct mm_execution_core_state {
    struct mm_cpu cpu;
    struct mm_scs scs;
    struct mm_nvic nvic;
    mm_u8 it_pattern;
    mm_u8 it_remaining;
    mm_u8 it_cond;
};

typedef mm_bool (*mm_execution_fpu_access_fn)(const struct mm_cpu *cpu,
                                              const struct mm_scs *scs);
typedef struct mm_decoded (*mm_execution_decode_fn)(
    const struct mm_fetch_result *fetch,
    const struct mm_cpu *cpu,
    const struct mm_scs *scs);
typedef mm_bool (*mm_execution_mem_fault_fn)(
    struct mm_cpu *cpu,
    struct mm_memmap *map,
    struct mm_scs *scs,
    mm_u32 fault_pc,
    mm_u32 fault_xpsr,
    mm_u32 addr,
    mm_bool is_exec);

enum mm_execution_fetch_status {
    MM_EXECUTION_FETCH_INVALID = 0,
    MM_EXECUTION_FETCH_READY,
    MM_EXECUTION_FETCH_FAULT_HANDLED,
    MM_EXECUTION_FETCH_FAULT_UNHANDLED
};

/* Owned architectural, memory and scheduler state for a single emulation
 * session. Target peripheral instances and signal state remain to be moved. */
struct mm_execution_state {
    struct mm_target_cfg target;
    struct mm_memmap map;
    struct mmio_region regions[256];
    struct mm_code_cache code_cache;
    struct mm_prot_ctx protection[2];
    struct mm_core_sys core_sys;
    struct mm_target_storage storage;
    struct mm_load_targets load_targets;
    struct mm_execution_core_state cores[2];
    struct mm_scs_mux scs_mux;
    struct mm_execution_clock clock;
    mm_u32 active_core;
};

/* Signal and target runtime modules contain process-global state; only one
 * initialized execution session may be live at a time. */
mm_bool mm_execution_state_init(struct mm_execution_state *state);
void mm_execution_state_end_run(struct mm_execution_state *state);
void mm_execution_state_destroy(struct mm_execution_state *state);
/* Select one supported SoC before allocating its owned flash/RAM storage. */
mm_bool mm_execution_state_select_target(struct mm_execution_state *state,
                                         const char *cpu_name);
mm_bool mm_execution_state_allocate_storage(struct mm_execution_state *state);
/* Preserve CLI setup phases: initialize map storage before target resets, then
 * bind flash/RAM views and code cache after storage has been allocated. */
void mm_execution_state_init_memory_map(struct mm_execution_state *state);
mm_bool mm_execution_state_configure_memory(struct mm_execution_state *state);
void mm_execution_state_init_code_cache(struct mm_execution_state *state);
void mm_execution_state_reset_soc(struct mm_execution_state *state);
void mm_execution_state_init_target_devices(struct mm_execution_state *state);
/* Load bounded ELF32/ARM PT_LOAD records into the state's owned buffers. */
int mm_execution_state_load_elf(struct mm_execution_state *state,
                                const mm_u8 *image,
                                size_t image_size,
                                size_t max_image_size,
                                mm_u32 flash_offset,
                                struct mm_image_load_result *result);
/* Fetch one instruction, apply the FPU-aware cache key, decode on cache miss,
 * and update the map's last-PC field. A fetch fault is returned in fetch_out. */
mm_bool mm_execution_state_fetch_decode(
    struct mm_execution_state *state,
    mm_execution_fpu_access_fn fpu_access_allowed,
    mm_execution_decode_fn decode,
    struct mm_fetch_result *fetch_out,
    struct mm_decoded *decoded_out,
    mm_bool *fpu_ok_out);
enum mm_execution_fetch_status mm_execution_state_fetch_decode_or_fault(
    struct mm_execution_state *state,
    mm_execution_fpu_access_fn fpu_access_allowed,
    mm_execution_decode_fn decode,
    mm_execution_mem_fault_fn raise_mem_fault,
    struct mm_fetch_result *fetch_out,
    struct mm_decoded *decoded_out,
    mm_bool *fpu_ok_out);
mm_bool mm_execution_state_should_execute(struct mm_execution_state *state,
                                          mm_u32 core_index,
                                          const struct mm_decoded *decoded);
void mm_execution_state_advance_it(struct mm_execution_state *state,
                                   mm_u32 core_index,
                                   const struct mm_decoded *decoded);
mm_bool mm_execution_state_advance_it_if_sequential(
    struct mm_execution_state *state,
    mm_u32 core_index,
    const struct mm_fetch_result *fetch,
    const struct mm_decoded *decoded);
/* Bind owned architectural pointers, execute through the shared production
 * executor, and apply the same sequential IT update as the CLI loop. */
enum mm_exec_status mm_execution_state_execute_decoded(
    struct mm_execution_state *state,
    mm_u32 core_index,
    struct mm_execute_ctx *context,
    const struct mm_fetch_result *fetch,
    const struct mm_decoded *decoded,
    mm_bool execute_it);

enum mm_execution_instruction_outcome {
    MM_EXECUTION_INSTRUCTION_INVALID = 0,
    MM_EXECUTION_INSTRUCTION_SKIPPED,
    MM_EXECUTION_INSTRUCTION_EXECUTED,
    MM_EXECUTION_INSTRUCTION_YIELDED
};

/* Complete one decoded instruction. A worker may call this once per budgeted
 * instruction after fetch/decode and policy hooks have completed. */
enum mm_execution_instruction_outcome mm_execution_state_step_decoded(
    struct mm_execution_state *state,
    mm_u32 core_index,
    struct mm_execute_ctx *context,
    const struct mm_fetch_result *fetch,
    const struct mm_decoded *decoded,
    mm_bool execute_it,
    enum mm_exec_status *exec_status_out);

/* Reset CPU/IT state while preserving the staged SCS and NVIC initialization
 * order used by target and protection setup. */
void mm_execution_state_reset_cores(struct mm_execution_state *state,
                                    mm_u32 core_count,
                                    mm_u32 vtor_s,
                                    mm_u32 vtor_ns);
void mm_execution_state_advance_cycles(
    struct mm_execution_state *state,
    mm_u32 core_index,
    mm_u64 cycles,
    mm_bool check_signal_trigger,
    mm_u32 trigger_pc);
mm_u64 mm_execution_state_signal_cycles_until_event(
    const struct mm_execution_state *state);
mm_u64 mm_execution_state_signal_now_ns(
    const struct mm_execution_state *state);
mm_bool mm_execution_state_apply_reset_vector(
    struct mm_execution_state *state,
    enum mm_sec_state boot_security);

/* Apply the production WFI/WFE wake rules to one owned core. */
mm_bool mm_execution_state_wake_sleeping_core(
    struct mm_execution_state *state,
    mm_u32 core_index);
enum mm_execution_irq_dispatch_result
mm_execution_state_dispatch_pending(
    struct mm_execution_state *state,
    mm_u32 core_index,
    enum mm_execution_irq_block_policy block_policy,
    mm_execution_irq_enter_fn enter_exception,
    void *enter_opaque,
    mm_bool *done);

#endif /* M33MU_EXECUTION_STATE_H */
