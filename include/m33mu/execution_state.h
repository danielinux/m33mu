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

void mm_execution_state_init(struct mm_execution_state *state);
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

/* Reset CPU/IT state while preserving the staged SCS and NVIC initialization
 * order used by target and protection setup. */
void mm_execution_state_reset_cores(struct mm_execution_state *state,
                                    mm_u32 core_count,
                                    mm_u32 vtor_s,
                                    mm_u32 vtor_ns);

#endif /* M33MU_EXECUTION_STATE_H */
